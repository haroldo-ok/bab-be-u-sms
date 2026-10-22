#!/usr/bin/env python3
"""Convert a WAV sound effect to the SMS port's PSG event stream.

Per 1/60s frame we measure RMS amplitude (-> PSG volume) and the
dominant frequency via FFT (-> PSG period).  Noise-like frames
(high spectral flatness) are routed to the noise channel.
Events use PSG channel 2 (tone) / 3 (noise) so SFX sit over music.
Format documented in xm2psg.py.
"""
import wave, struct
import numpy as np

def convert(path, max_frames=150):
    w = wave.open(path, 'rb')
    sr = w.getframerate()
    nch = w.getnchannels()
    sw = w.getsampwidth()
    raw = w.readframes(w.getnframes())
    w.close()
    if sw == 2:
        a = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768.0
    elif sw == 1:
        a = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128) / 128.0
    elif sw == 4:
        a = np.frombuffer(raw, dtype='<i4').astype(np.float32) / 2147483648.0
    else:
        return b'\xff'
    if nch > 1:
        a = a.reshape(-1, nch).mean(axis=1)
    frame = int(sr / 60)
    if frame <= 0: return b'\xff'
    out = bytearray()
    pending = 0
    last = (None, 0, 0)  # chan, period, vol
    peak = max(0.05, float(np.max(np.abs(a))) if len(a) else 0.05)
    nframes = min(max_frames, len(a) // frame)
    for i in range(nframes):
        seg = a[i * frame:(i + 1) * frame]
        rms = float(np.sqrt(np.mean(seg * seg))) if len(seg) else 0.0
        vol = int(round(15 * min(1.0, rms / (peak * 0.6)))) if rms > 0.004 else 0
        chan, per = 2, 0
        if vol > 0 and len(seg) >= 64:
            spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
            freqs = np.fft.rfftfreq(len(seg), 1 / sr)
            band = (freqs >= 60) & (freqs <= 6000)
            if band.any():
                s = spec.copy(); s[~band] = 0
                k = int(np.argmax(s))
                flat = float(np.exp(np.mean(np.log(spec[band] + 1e-9))) / (np.mean(spec[band]) + 1e-9))
                if flat > 0.35:
                    chan = 3
                    per = 4 | (0 if freqs[k] > 3000 else 1 if freqs[k] > 1200 else 2)
                else:
                    f = float(freqs[k])
                    while f < 110: f *= 2
                    per = max(1, min(1023, int(round(3579545 / (32 * f)))))
        cur = (chan, per, vol)
        if cur != last and not (vol == 0 and last[2] == 0):
            while pending > 0:
                take = min(255, pending)
                out += bytes([0xFE, take]); pending -= take
            if vol == 0:
                out += bytes([(last[0] << 4), 0, 0])  # silence previous channel
            else:
                out += bytes([(chan << 4) | vol, per & 0xFF, (per >> 8) & 0xFF])
                if last[2] > 0 and last[0] != chan:
                    out += bytes([(last[0] << 4), 0, 0])
            last = cur
        pending += 1
    if last[2] > 0:
        while pending > 0:
            take = min(255, pending)
            out += bytes([0xFE, take]); pending -= take
        out += bytes([(last[0] << 4), 0, 0])
    out.append(0xFF)
    return bytes(out)

if __name__ == '__main__':
    import sys
    print(len(convert(sys.argv[1])), 'bytes')
