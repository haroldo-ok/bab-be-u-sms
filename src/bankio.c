/* Banked data access: all large tables live in 16KB ROM banks mapped
   into slot 2 (0x8000-0xBFFF).  "span" blobs occupy whole consecutive
   banks starting at offset 0 (tile table, colour lists, names, music). */
#include "engine.h"

unsigned char bread(unsigned char bank, unsigned int off) {
    unsigned char v;
    SMS_mapROMBank(bank);
    v = *(unsigned char *)(0x8000 + off);
    return v;
}
unsigned int bread16(unsigned char bank, unsigned int off) {
    unsigned int v;
    SMS_mapROMBank(bank);
    v = *(unsigned int *)(0x8000 + off);
    return v;
}
void bcopy(unsigned char bank, unsigned int off, unsigned char *dst, unsigned int len) {
    unsigned char *src = (unsigned char *)(0x8000 + off);
    SMS_mapROMBank(bank);
    while (len) { *dst++ = *src++; len--; }
}
unsigned char sread(unsigned char base_bank, unsigned long idx) {
    SMS_mapROMBank(base_bank + (unsigned char)(idx >> 14));
    return *(unsigned char *)(0x8000 + (unsigned int)(idx & 0x3FFF));
}
unsigned int sread16(unsigned char base_bank, unsigned long idx) {
    unsigned int lo = sread(base_bank, idx);
    return lo | ((unsigned int)sread(base_bank, idx + 1) << 8);
}
void scopy(unsigned char base_bank, unsigned long idx, unsigned char *dst, unsigned int len) {
    unsigned int i;
    for (i = 0; i < len; i++) dst[i] = sread(base_bank, idx + i);
}

void get_shape_ptr(unsigned int shape_id, unsigned char *bank, unsigned int *off) {
    unsigned long e = (unsigned long)shape_id * 3;
    *bank = bread(GD_SHAPEDIR_BANK, GD_SHAPEDIR_OFF + (unsigned int)e);
    *off = bread16(GD_SHAPEDIR_BANK, GD_SHAPEDIR_OFF + (unsigned int)e + 1);
}

/* Load one 20-byte global tile entry into a ClassInfo (level-local
   fields like sms_off/vram are filled by the level loader). */
void load_tile_entry(unsigned int tile_id, ClassInfo *c) {
    unsigned long e = (unsigned long)tile_id * TILE_ENTRY_SIZE;
    unsigned int color_off;
    unsigned char ref2lo, ref2hi;
    c->tile = tile_id;
    c->layer = sread(GD_TILETABLE_BANK, e + 8);
    c->flags = sread(GD_TILETABLE_BANK, e + 9);
    c->types = sread16(GD_TILETABLE_BANK, e + 10);
    c->ref = sread16(GD_TILETABLE_BANK, e + 12);
    ref2lo = sread(GD_TILETABLE_BANK, e + 14);
    ref2hi = sread(GD_TILETABLE_BANK, e + 15);
    c->prop_id = (c->types & TY_PROPERTY) ? ref2lo : 0xFF;
    c->cond_id = (c->types & (TY_COND_PREFIX | TY_COND_INFIX)) ? ref2hi : 0xFF;
    c->rel_cls = -1; /* resolved to a local class by the level loader via rel tile below */
    /* stash rel global tile id temporarily in sms_off high use: keep separate static */
    color_off = sread16(GD_TILETABLE_BANK, e + 18);
    c->sms_off = sread16(GD_TILETABLE_BANK, e + 16); /* TEMP: rel tile id; loader replaces */
    c->nsrc = sread(GD_COLORS_BANK, color_off);
    {
        unsigned char coord = sread(GD_COLORS_BANK, (unsigned long)color_off + 1);
        c->mcoord = coord;
    }
    c->flye = 0; c->flye_mode = 0; c->palbits = 0;
    c->vram_base = 0;
}

void get_level_dir(unsigned int menu_idx, unsigned char *entry13) {
    bcopy(GD_LEVELDIR_BANK, GD_LEVELDIR_OFF + menu_idx * LEVELDIR_ENTRY, entry13, LEVELDIR_ENTRY);
}

void read_name_str(unsigned int name_off, char *dst, unsigned char maxlen) {
    unsigned char i = 0;
    unsigned char ch;
    while (i < (unsigned char)(maxlen - 1)) {
        ch = sread(GD_NAMES_BANK, (unsigned long)name_off + i);
        if (!ch) break;
        dst[i++] = (char)ch;
    }
    dst[i] = 0;
}

unsigned int rng_state = 0xACE1;
unsigned int rng_next(void) {
    /* xorshift16 */
    rng_state ^= rng_state << 7;
    rng_state ^= rng_state >> 9;
    rng_state ^= rng_state << 8;
    return rng_state;
}

/* Shape ids live outside RAM (ClassInfo stays small): re-read on demand. */
void load_tile_shapes(unsigned int tile_id, unsigned int sh[4]) {
    unsigned long e = (unsigned long)tile_id * TILE_ENTRY_SIZE;
    unsigned char i;
    for (i = 0; i < 4; i++) sh[i] = sread16(GD_TILETABLE_BANK, e + i * 2);
}
