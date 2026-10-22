/* PSG stream player for converted XM music + WAV-derived SFX.
   Stream format (tools/xm2psg.py, tools/wav2sfx.py):
     0xFF            end (music loops to start)
     0xFE nn         wait nn frames
     0b00ccvvvv l h  channel cc volume v (15=loudest) + period
   Music streams live in whole consecutive banks (span); SFX blobs are
   single-bank.  SFX steal channels 2/3 while active.

   audio_frame() is registered as the SMSlib frame (VBlank) interrupt
   handler, so playback keeps its 60 Hz cadence while the main thread
   is busy with a multi-frame turn or redraw.  The handler preserves
   the slot-2 ROM bank and SRAM mapper state it interrupted. */
#include "engine.h"

#define AUDIO_IRQ_OFF() __asm di __endasm
#define AUDIO_IRQ_ON()  __asm ei __endasm

unsigned char psg_val;
void psg_flush(void) __naked {
    __asm
    ld a,(_psg_val)
    out (0x7f),a
    ret
    __endasm;
}
static void psg(unsigned char v) { psg_val = v; psg_flush(); }

/* shadow of what we want per channel: period + volume(0..15, 15 loud) */
static unsigned int mus_per[4];
static unsigned char mus_vol[4];
static unsigned int sfx_per[4];
static unsigned char sfx_vol[4];
static unsigned int out_per[4];
static unsigned char out_vol[4];

static unsigned char mus_bank;
static unsigned long mus_pos;
static unsigned int mus_wait;
static unsigned char mus_on;

static unsigned char sfx_bank;
static unsigned int sfx_off, sfx_pos;
static unsigned int sfx_wait;
static unsigned char sfx_on;

static unsigned int sing_per;
static unsigned char sing_frames;

void audio_init(void) {
    unsigned char i;
    for (i = 0; i < 4; i++) {
        mus_vol[i] = sfx_vol[i] = out_vol[i] = 0;
        mus_per[i] = sfx_per[i] = out_per[i] = 0;
        psg(0x90 | (i << 5) | 0x0F); /* attenuation 15 = silent */
    }
    mus_on = sfx_on = 0;
    sing_frames = 0;
}

static unsigned char mus_read(void) {
    unsigned char v = sread(mus_bank, mus_pos);
    mus_pos++;
    return v;
}

void music_play(unsigned char music_id) {
    unsigned char i;
    unsigned char bank = 0;
    unsigned char valid = (music_id && music_id <= NUM_MUSIC);
    /* Read the directory while the old stream is still playing, then
       swap the multi-byte stream state atomically w.r.t. the VBlank
       handler.  An invalid id stops the music, as before. */
    if (valid)
        bank = bread(GD_MUSICDIR_BANK, GD_MUSICDIR_OFF + (music_id - 1) * 3);
    AUDIO_IRQ_OFF();
    mus_on = 0;
    for (i = 0; i < 4; i++) { mus_vol[i] = 0; mus_per[i] = 0; }
    if (valid) {
        mus_bank = bank;
        mus_pos = 0;
        mus_wait = 0;
        mus_on = 1;
    }
    AUDIO_IRQ_ON();
}
void music_stop(void) {
    unsigned char i;
    AUDIO_IRQ_OFF();
    mus_on = 0;
    for (i = 0; i < 4; i++) mus_vol[i] = 0;
    AUDIO_IRQ_ON();
}
void sfx_play(unsigned char sfx_id) {
    unsigned char bank;
    unsigned int off;
    if (sfx_id >= NUM_SFX) return;
    bank = bread(GD_SFXDIR_BANK, GD_SFXDIR_OFF + sfx_id * 3);
    if (!bank) return;
    off = bread16(GD_SFXDIR_BANK, GD_SFXDIR_OFF + sfx_id * 3 + 1);
    AUDIO_IRQ_OFF();
    sfx_bank = bank;
    sfx_off = off;
    sfx_pos = 0;
    sfx_wait = 0;
    sfx_vol[2] = sfx_vol[3] = 0;
    sfx_on = 1;
    AUDIO_IRQ_ON();
}
void audio_sing(unsigned int period) {
    AUDIO_IRQ_OFF();
    sing_per = period;
    sing_frames = 12;
    AUDIO_IRQ_ON();
}

static void advance_music(void) {
    unsigned char b, ch, lo, hi;
    if (!mus_on) return;
    if (mus_wait) { mus_wait--; return; }
    for (;;) {
        b = mus_read();
        if (b == 0xFF) { mus_pos = 0; continue; }
        if (b == 0xFE) { mus_wait = mus_read(); if (mus_wait) mus_wait--; return; }
        ch = (b >> 4) & 3;
        lo = mus_read(); hi = mus_read();
        mus_per[ch] = lo | ((unsigned int)hi << 8);
        mus_vol[ch] = b & 15;
        if (mus_pos > 4000000UL) { mus_on = 0; return; }
    }
}
static unsigned char sfx_read(void) {
    unsigned char v;
    SMS_mapROMBank(sfx_bank);
    v = *(unsigned char *)(0x8000 + sfx_off + sfx_pos);
    sfx_pos++;
    return v;
}
static void advance_sfx(void) {
    unsigned char b, ch, lo, hi;
    if (!sfx_on) return;
    if (sfx_wait) { sfx_wait--; return; }
    for (;;) {
        b = sfx_read();
        if (b == 0xFF) { sfx_on = 0; sfx_vol[2] = sfx_vol[3] = 0; return; }
        if (b == 0xFE) { sfx_wait = sfx_read(); if (sfx_wait) sfx_wait--; return; }
        ch = (b >> 4) & 3;
        lo = sfx_read(); hi = sfx_read();
        sfx_per[ch] = lo | ((unsigned int)hi << 8);
        sfx_vol[ch] = b & 15;
    }
}

void audio_frame(void) {
    /* VBlank handler: the interrupted code may be executing from a
       slot-2 ROM bank or accessing slot-2 SRAM (undo snapshots).
       Preserve both mapper registers around the banked stream reads. */
    SMS_saveROMBank();
    unsigned char saved_sram_bank = SRAM_bank_to_be_mapped_on_slot2;
    unsigned char ch;
    unsigned int per;
    unsigned char vol;
    if (saved_sram_bank) SMS_disableSRAM();
    advance_music();
    advance_sfx();
    if (sing_frames) {
        sing_frames--;
        sfx_per[1] = sing_per; sfx_vol[1] = 10;
        if (!sing_frames) sfx_vol[1] = (sfx_on ? sfx_vol[1] : 0);
    }
    for (ch = 0; ch < 4; ch++) {
        if (ch >= 2 && sfx_on) { per = sfx_per[ch]; vol = sfx_vol[ch]; }
        else if (ch == 1 && sing_frames) { per = sfx_per[1]; vol = sfx_vol[1]; }
        else { per = mus_per[ch]; vol = mus_vol[ch]; }
        if (per != out_per[ch] && vol) {
            if (ch < 3) {
                psg(0x80 | (ch << 5) | (per & 0x0F));
                psg((per >> 4) & 0x3F);
            } else {
                psg(0xE0 | (per & 7));
            }
            out_per[ch] = per;
        }
        if (vol != out_vol[ch]) {
            psg(0x90 | (ch << 5) | (15 - vol));
            out_vol[ch] = vol;
        }
    }
    SRAM_bank_to_be_mapped_on_slot2 = saved_sram_bank;
    SMS_restoreROMBank();
}
