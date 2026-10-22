#!/usr/bin/env python3
"""Convert an XM module to the SMS port's compact PSG event stream.

Stream format (shared with wav2sfx.py and src/audio.c):
  0xFF              end of stream (music loops, sfx stop)
  0xFE nn           wait nn frames
  0b00ccvvvv lo hi  set channel cc (0-2 tone, 3 noise) volume vvvv (15=loud)
                    and period (lo | hi<<8); for noise, lo bits0-1 = rate,
                    bit2 = white/periodic
Notes are converted note-for-note; instrument volume envelopes are baked
in.  XM channels beyond the 3 PSG tone channels are voice-allocated
(first channels win); instruments whose names suggest drums/noise are
routed to the noise channel.
"""
import struct, math

def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]

def parse_xm(path):
    b = open(path, 'rb').read()
    assert b[:17] == b'Extended Module: ', 'not XM'
    hdr_size = _u32(b, 60)
    song_len = _u16(b, 64)
    nchan = _u16(b, 68)
    npat = _u16(b, 70)
    ninst = _u16(b, 72)
    speed = _u16(b, 76) or 6
    bpm = _u16(b, 78) or 125
    order = list(b[80:80 + song_len])
    off = 60 + hdr_size
    patterns = []
    for _ in range(npat):
        phlen = _u32(b, off)
        rows = _u16(b, off + 5)
        dsize = _u16(b, off + 7)
        data = b[off + phlen: off + phlen + dsize]
        pat = []
        p = 0
        for r in range(rows):
            row = []
            for c in range(nchan):
                note = inst = vol = eff = epar = 0
                if p < len(data):
                    first = data[p]; p += 1
                    if first & 0x80:
                        if first & 1: note = data[p]; p += 1
                        if first & 2: inst = data[p]; p += 1
                        if first & 4: vol = data[p]; p += 1
                        if first & 8: eff = data[p]; p += 1
                        if first & 16: epar = data[p]; p += 1
                    else:
                        note = first
                        inst, vol, eff, epar = data[p:p + 4]; p += 4
                row.append((note, inst, vol, eff, epar))
            pat.append(row)
        patterns.append(pat)
        off += phlen + dsize
    instruments = []
    for _ in range(ninst):
        isize = _u32(b, off) if off + 4 <= len(b) else 0
        name = b[off + 4:off + 26].split(b'\x00')[0].decode('latin1', 'ignore').lower()
        nsamp = _u16(b, off + 27) if isize >= 29 else 0
        env = []          # (tick, vol0-64)
        sustain = -1
        env_on = False
        if nsamp and isize >= 241:
            npts = b[off + 225]
            sustain = b[off + 227]
            env_on = bool(b[off + 233] & 1)
            has_sustain = bool(b[off + 233] & 2)
            if not has_sustain: sustain = -1
            for i in range(min(npts, 12)):
                x = _u16(b, off + 129 + i * 4)
                y = _u16(b, off + 131 + i * 4)
                env.append((x, y))
        instruments.append({'name': name, 'env': env if env_on else [], 'sustain': sustain})
        if nsamp:
            shsize = _u32(b, off + 29)
            total = 0
            for si in range(nsamp):
                total += _u32(b, off + isize + si * shsize)
            off += isize + nsamp * shsize + total
        else:
            off += isize if isize else 29
    return {'order': order, 'patterns': patterns, 'speed': speed, 'bpm': bpm,
            'nchan': nchan, 'instruments': instruments}

NOISE_WORDS = ('nois', 'hat', 'drum', 'snare', 'kick', 'perc', 'cymbal', 'clap')

def note_period(note):
    midi = note + 11
    freq = 440.0 * 2 ** ((midi - 69) / 12.0)
    while freq < 110.0: freq *= 2
    per = int(round(3579545 / (32 * freq)))
    return max(1, min(1023, per))

class Voice:
    __slots__ = ('note', 'inst', 'vol', 'tick', 'released', 'off_tick')
    def __init__(self):
        self.note = 0; self.inst = 0; self.vol = 64; self.tick = 0
        self.released = False; self.off_tick = 0

def env_volume(inst, v):
    env = inst['env']
    if not env:
        return 0 if v.released else v.vol
    sus = inst['sustain']
    tick = v.tick
    # find position along envelope
    if not v.released and 0 <= sus < len(env) and tick >= env[sus][0]:
        return env[sus][1] * v.vol // 64
    if v.released and 0 <= sus < len(env):
        tick = env[sus][0] + (v.tick - v.off_tick)
    prev = (0, env[0][1])
    for pt in env:
        if tick <= pt[0]:
            if pt[0] == prev[0]: return pt[1] * v.vol // 64
            return int((prev[1] + (pt[1] - prev[1]) * (tick - prev[0]) / (pt[0] - prev[0])) * v.vol // 64)
        prev = pt
    return env[-1][1] * v.vol // 64

def convert(path):
    xm = parse_xm(path)
    speed, bpm = xm['speed'], xm['bpm']
    frames_per_tick = 150.0 / bpm
    voices = [Voice() for _ in range(xm['nchan'])]
    out = bytearray()
    frame_acc = 0.0
    pending_wait = 0
    last_emitted = [(0, 0)] * 4   # (period, volume) per PSG channel

    def flush_wait(final=False):
        nonlocal pending_wait
        while pending_wait > 0:
            n = min(255, pending_wait)
            out.extend(bytes([0xFE, n]))
            pending_wait -= n

    def emit_states(states):
        nonlocal pending_wait
        for ch in range(4):
            per, vol = states[ch]
            if (per, vol) != last_emitted[ch]:
                if pending_wait: flush_wait()
                out.extend(bytes([(ch << 4) | (vol & 15), per & 0xFF, (per >> 8) & 0xFF]))
                last_emitted[ch] = (per, vol)

    total_ticks = 0
    for pat_idx in xm['order']:
        if pat_idx >= len(xm['patterns']): continue
        pat = xm['patterns'][pat_idx]
        for row in pat:
            # row start: note events
            for ci, (note, inst, vol, eff, epar) in enumerate(row):
                v = voices[ci]
                if vol >= 0x10 and vol <= 0x50:
                    v.vol = vol - 0x10
                if eff == 0x0F and epar:  # set speed/bpm
                    if epar < 32: speed = epar
                    else: bpm = epar; frames_per_tick = 150.0 / bpm
                if note == 97:
                    v.released = True; v.off_tick = v.tick
                elif 1 <= note <= 96:
                    v.note = note; v.tick = 0; v.released = False
                    if inst: v.inst = inst - 1
                    if not (vol >= 0x10 and vol <= 0x50): v.vol = 64
                elif inst:
                    v.inst = inst - 1
            for t in range(speed):
                # build PSG states from voices
                states = [(0, 0)] * 4
                tone_used = 0
                for ci, v in enumerate(voices):
                    if not v.note: continue
                    inst = xm['instruments'][v.inst] if 0 <= v.inst < len(xm['instruments']) else {'env': [], 'sustain': -1, 'name': ''}
                    vol64 = env_volume(inst, v)
                    vol = vol64 * 15 // 64
                    if vol <= 0: continue
                    if any(w in inst['name'] for w in NOISE_WORDS):
                        if states[3][1] == 0:
                            rate = 3 - min(3, v.note // 24)
                            states[3] = ((4 | rate), vol)
                    elif tone_used < 3:
                        states[tone_used] = (note_period(v.note), vol)
                        tone_used += 1
                frame_acc += frames_per_tick
                while frame_acc >= 1.0:
                    emit_states(states)
                    pending_wait += 1
                    frame_acc -= 1.0
                for v in voices:
                    if v.note: v.tick += 1
                total_ticks += 1
                if total_ticks > 60 * 60 * 6:  # cap ~6 minutes
                    break
    out.append(0xFF)
    return bytes(out)

if __name__ == '__main__':
    import sys
    d = convert(sys.argv[1])
    print(len(d), 'bytes')
