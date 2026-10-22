/* Rendering: 16x16 cells made of 2x2 SMS tiles, baked per level from
   the converted upstream sprites.  Stacked units are composited into
   a small cache of generated metatiles (the VDP has no per-cell
   layers), giving the same visual stacking as the original. */
#include "engine.h"
#include "../gen/font.h"

unsigned char cam_x, cam_y;
unsigned char need_full_redraw;
unsigned char board_dirty;
unsigned int vram_start = 1;

static unsigned char bgpal[16], sprpal[16];
static unsigned char colour_home[64];      /* 0xFF none, else pal<<4|idx */
static unsigned char snap_tab[2][64];      /* nearest slot in palette */
static unsigned char cls_opaque[MAXCLASS];
unsigned int sh_key[VIEW_W * VIEW_H];   /* global: used by propfast.s scan */

#define COMP_SLOTS 8
#define COMP_VRAM0 416
static unsigned int comp_key[COMP_SLOTS];
static unsigned char comp_stamp[COMP_SLOTS];
static unsigned char comp_palbits[COMP_SLOTS];
static unsigned char frame_stamp;
static unsigned char comp_evict_happened;

static const unsigned char blank_tile[32] = {0};
static const char font_charset[] = FONT_CHARS;

extern unsigned char has_prop_c(unsigned char cls, unsigned char prop);
/* Assembly draw helpers (propfast.s).  They use the fixed scratch
   layout of draw_board: cell_key +448, cell_count +832, cell_top
   +1024, changed list +1216.  Keep those offsets in sync. */
extern void draw_clear_cells(void);
extern unsigned char draw_scan_changed(void);

void render_init(void) {
    SMS_loadTiles(blank_tile, 0, 32);
    /* hide all 64 sprites: SAT at VRAM 0x3F00, Y=0xE0 (below screen).
       Direct VRAM write avoids linking SMSlib's sprite module (192B RAM). */
    {
        unsigned char i;
        for (i = 0; i < 64; i++) scratch[i] = 0xE0;
        SMS_VRAMmemcpy(0x3F00, scratch, 64);
    }
    need_full_redraw = 1;
    board_dirty = 1;
}

void load_font(void) {
    SMS_loadTiles(font_tiles, 1, sizeof(font_tiles));
}

void set_menu_palette(void) {
    unsigned char i;
    for (i = 0; i < 16; i++) { bgpal[i] = 0x10; sprpal[i] = 0x10; }
    /* SMS colour bytes: --bbggrr.  Taken from the upstream default
       palette (quantised): bg #1b1b22, white, bab pink. */
    bgpal[0] = 0x10; bgpal[1] = 0x3F;
    sprpal[0] = 0x10; sprpal[1] = 0x37;
    SMS_loadBGPalette(bgpal);
    SMS_loadSpritePalette(sprpal);
    SMS_setBackdropColor(0);
}

static unsigned int font_tile(char ch) {
    unsigned char i;
    for (i = 0; font_charset[i]; i++)
        if (font_charset[i] == ch) return 1 + i;
    return 1;
}
void print_str2(unsigned char x, unsigned char y, const char *s, unsigned int extra) {
    while (*s && x < 32) {
        SMS_setTileatXY(x, y, font_tile(*s) | extra);
        x++; s++;
    }
}
void print_str(unsigned char x, unsigned char y, const char *s) {
    print_str2(x, y, s, 0);
}
void print_str_banked_name(unsigned char x, unsigned char y, unsigned int name_off) {
    char buf[32];
    read_name_str(name_off, buf, 32);
    print_str(x, y, buf);
}

void clear_screen(unsigned int tile) {
    unsigned int i;
    SMS_setNextTileatLoc(0);
    for (i = 0; i < 32 * 24; i++) SMS_setTile(tile);
}

/* ---------------- colour slot assignment + tile baking ---------------- */

static unsigned char chan_dist(unsigned char a, unsigned char b) {
    signed char d;
    unsigned char s = 0;
    d = (a & 3) - (b & 3); s += (d < 0 ? -d : d);
    d = ((a >> 2) & 3) - ((b >> 2) & 3); s += (d < 0 ? -d : d);
    d = ((a >> 4) & 3) - ((b >> 4) & 3); s += (d < 0 ? -d : d);
    return s;
}

static void load_shape(unsigned int shape_id, unsigned char *dst) {
    unsigned char bank; unsigned int off;
    get_shape_ptr(shape_id, &bank, &off);
    bcopy(bank, off, dst, 128);
}
static void load_sms_table(ClassInfo *c, unsigned char dim, unsigned char *dst) {
    unsigned int off = level_off + c->sms_off + (dim ? c->nsrc : 0);
    unsigned char i;
    for (i = 0; i < 16; i++) dst[i] = level_bg;
    bcopy(level_bank, off, dst, c->nsrc);
}

/* Pack one 8x8 quadrant from shape-value pixels via an SMS-colour
   table; returns chosen palette bit.  pix points to 256 shape values
   (for baking) when from_shape, or 256 SMS colours (composites). */
static unsigned char pack_quadrant(const unsigned char *pix, const unsigned char *sms,
                                   unsigned char from_shape, unsigned char qx, unsigned char qy,
                                   unsigned char *out32) {
    /* Pointer row walks: the indexed form cost ~108k cycles per 8x8
       quadrant under SDCC.  Output bytes are identical. */
    const unsigned char *row = pix + ((unsigned int)qy << 4) + qx;
    unsigned char x, y, pal;
    unsigned int score0 = 0, score1 = 0;
    unsigned char col, home, idx;
    for (y = 0; y < 8; y++) {
        const unsigned char *pp = row;
        for (x = 0; x < 8; x++) {
            unsigned char v = *pp++;
            col = from_shape ? (v ? sms[v - 1] : level_bg) : v;
            home = colour_home[col & 63];
            if (home == 0xFF) continue;
            if (home & 0x10) score1++; else score0++;
        }
        row += 16;
    }
    pal = (score1 > score0) ? 1 : 0;
    row = pix + ((unsigned int)qy << 4) + qx;
    for (y = 0; y < 8; y++) {
        unsigned char p0 = 0, p1 = 0, p2 = 0, p3 = 0, bit = 0x80;
        const unsigned char *pp = row;
        for (x = 0; x < 8; x++) {
            unsigned char v = *pp++;
            col = from_shape ? (v ? sms[v - 1] : level_bg) : v;
            home = colour_home[col & 63];
            if (home != 0xFF && ((home >> 4) & 1) == pal) idx = home & 15;
            else idx = snap_tab[pal][col & 63];
            if (idx & 1) p0 |= bit;
            if (idx & 2) p1 |= bit;
            if (idx & 4) p2 |= bit;
            if (idx & 8) p3 |= bit;
            bit >>= 1;
        }
        *out32++ = p0; *out32++ = p1; *out32++ = p2; *out32++ = p3;
        row += 16;
    }
    return pal;
}

static void bake_variant(ClassInfo *c, unsigned int shape_id, unsigned char dim,
                         unsigned int vram, unsigned char palbit_shift) {
    unsigned char sms[16];
    unsigned char *shape = scratch;          /* 128 packed */
    unsigned char *pix = scratch + 128;      /* 256 values */
    unsigned char *tile = scratch + 384;     /* 32 */
    unsigned char q, i;
    load_shape(shape_id, shape);
    load_sms_table(c, dim, sms);
    for (i = 0; i < 128; i++) { pix[i * 2] = shape[i] >> 4; pix[i * 2 + 1] = shape[i] & 15; }
    for (q = 0; q < 4; q++) {
        unsigned char pal = pack_quadrant(pix, sms, 1, (q & 1) * 8, (q >> 1) * 8, tile);
        if (pal) c->palbits |= (unsigned int)1 << (palbit_shift + q);
        UNSAFE_SMS_loadTiles(tile, vram + q, 32);
    }
}

void bake_level_tiles(void) {
    dbg_stage = 20;
    unsigned int *freq = (unsigned int *)(scratch + 512); /* bake-only overlay */
    unsigned char order[64];
    unsigned char sms[16];
    unsigned char *shape = scratch;
    unsigned char ci, i, j, q;
    unsigned int vram;
    for (i = 0; i < 64; i++) { freq[i] = 0; order[i] = i; colour_home[i] = 0xFF; }
    /* frequency of each SMS colour across all class shapes */
    for (ci = 0; ci < nclass; ci++) {
        ClassInfo *c = &classes[ci];
        unsigned int sh[4];
        unsigned char stage;
        load_tile_shapes(c->tile, sh);
        cls_opaque[ci] = 1;
        for (stage = 0; stage < 3; stage++) {
            unsigned int sid;
            unsigned char dim = 0;
            if (stage == 0) {
                sid = sh[0];
            } else if (stage == 1 && (c->flags & TF_ROTATE)) {
                sid = sh[1];
            } else if ((c->flags & TF_TEXT) &&
                       stage == ((c->flags & TF_ROTATE) ? 2 : 1)) {
                /* Count inactive-text colours too.  The former stage
                   test skipped this variant for every non-rotating text
                   tile, often snapping dim text to the background. */
                sid = (c->flags & TF_HASDIM) ? sh[3] : sh[0];
                dim = 1;
            } else {
                continue;
            }
            if (sid == 0xFFFF) continue;
            load_shape(sid, shape);
            load_sms_table(c, dim, sms);
            for (i = 0; i < 128; i++) {
                unsigned char v = shape[i] >> 4;
                if (v) freq[sms[v - 1] & 63]++; else if (stage == 0) cls_opaque[ci] = 0;
                v = shape[i] & 15;
                if (v) freq[sms[v - 1] & 63]++; else if (stage == 0) cls_opaque[ci] = 0;
            }
        }
    }
    dbg_stage = 21;
    /* insertion sort colours by freq desc (bg handled separately) */
    for (i = 1; i < 64; i++) {
        unsigned char o = order[i];
        j = i;
        while (j > 0 && freq[order[j - 1]] < freq[o]) { order[j] = order[j - 1]; j--; }
        order[j] = o;
    }
    for (i = 0; i < 16; i++) { bgpal[i] = level_bg; sprpal[i] = level_bg; }
    q = 1;
    for (i = 0; i < 64 && q < 16; i++) {
        if (order[i] == (level_bg & 63) || !freq[order[i]]) continue;
        bgpal[q] = order[i]; colour_home[order[i]] = q; q++;
    }
    q = 1;
    for (i = 0; i < 64 && q < 16; i++) {
        if (order[i] == (level_bg & 63) || !freq[order[i]] || colour_home[order[i]] != 0xFF) continue;
        sprpal[q] = order[i]; colour_home[order[i]] = 0x10 | q; q++;
    }
    colour_home[level_bg & 63] = 0;
    for (i = 0; i < 64; i++) {
        unsigned char best = 255, bi = 0;
        for (j = 0; j < 16; j++) { unsigned char d = chan_dist(i, bgpal[j]); if (d < best) { best = d; bi = j; } }
        snap_tab[0][i] = bi;
        best = 255; bi = 0;
        for (j = 0; j < 16; j++) { unsigned char d = chan_dist(i, sprpal[j]); if (d < best) { best = d; bi = j; } }
        snap_tab[1][i] = bi;
    }
    dbg_stage = 22;
    vram = vram_start;
    for (ci = 0; ci < nclass; ci++) {
        dbg_stage = 30 + ci;
        ClassInfo *c = &classes[ci];
        unsigned int sh[4];
        load_tile_shapes(c->tile, sh);
        c->palbits = 0;
        c->vram_base = vram; bake_variant(c, sh[0], 0, vram, 0); vram += 4;
        if (c->flags & TF_ROTATE) { bake_variant(c, sh[1], 0, vram, 4); vram += 4; }
        if (c->flags & TF_TEXT) {
            bake_variant(c, (c->flags & TF_HASDIM) ? sh[3] : sh[0], 1, vram, 8);
            vram += 4;
        }
    }
    SMS_loadBGPalette(bgpal);
    SMS_loadSpritePalette(sprpal);
    SMS_setBackdropColor(0);
    for (i = 0; i < COMP_SLOTS; i++) comp_key[i] = 0;
    need_full_redraw = 1;
    board_dirty = 1;
    dbg_stage = 23;
}

/* wobble animation: re-bake wobble classes' base tiles from frame shapes */
static unsigned char wob_frame;
void upload_wobble_frame(void) {
    unsigned char ci;
    wob_frame = (wob_frame + 1) % 3;
    for (ci = 0; ci < nclass; ci++) {
        ClassInfo *c = &classes[ci];
        unsigned int sh[4];
        if (!(c->flags & TF_WOBBLE)) continue;
        load_tile_shapes(c->tile, sh);
        bake_variant(c, wob_frame == 0 ? sh[0] : (wob_frame == 1 ? sh[2] : sh[3]), 0, c->vram_base, 0);
    }
}

/* ---------------- board drawing ---------------- */

/* variant kinds */
#define VK_BASE 0
#define VK_ROT 1
#define VK_DIM 2

static unsigned char variant_of(unsigned int unit, unsigned char *fx, unsigned char *fy) {
    ClassInfo *c = &classes[u_cls[unit]];
    unsigned char dir = u_dir[unit] & DIR_MASK;
    *fx = *fy = 0;
    if ((c->flags & TF_TEXT) && !(u_dir[unit] & U_ACTIVE)) {
        if (!(c->flags & TF_ROTATE) || dir == 1 || dir == 0) return VK_DIM;
    }
    if (c->flags & TF_ROTATE) {
        /* snap dirs8 to cardinal: R=1,8  D=2,3  L=4,5  U=6,7 */
        if (dir == 2 || dir == 3) return VK_ROT;
        if (dir == 4 || dir == 5) { *fx = 1; *fy = 1; return VK_BASE; }
        if (dir == 6 || dir == 7) { *fx = 1; *fy = 1; return VK_ROT; }
    }
    return VK_BASE;
}

static unsigned int variant_vram(ClassInfo *c, unsigned char vk, unsigned char *shift) {
    if (vk == VK_ROT) { *shift = 4; return c->vram_base + 4; }
    if (vk == VK_DIM) { *shift = 8; return c->vram_base + 4 + ((c->flags & TF_ROTATE) ? 4 : 0); }
    *shift = 0; return c->vram_base;
}

static void cell_entries_direct(unsigned int unit, unsigned int *e) {
    ClassInfo *c = &classes[u_cls[unit]];
    unsigned char fx, fy, shift, q;
    unsigned char vk = variant_of(unit, &fx, &fy);
    unsigned int vram = variant_vram(c, vk, &shift);
    for (q = 0; q < 4; q++) {
        unsigned char sqx = (q & 1) ^ (fx ? 1 : 0);
        unsigned char sqy = ((q >> 1) & 1) ^ (fy ? 1 : 0);
        unsigned char sq = sqx | (sqy << 1);
        unsigned int ent = vram + sq;
        if (fx) ent |= TILE_FLIPPED_X;
        if (fy) ent |= TILE_FLIPPED_Y;
        if (c->palbits & ((unsigned int)1 << (shift + sq))) ent |= TILE_USE_SPRITE_PALETTE;
        e[q] = ent;
    }
}

/* Per-unit overworld artwork: lvl entrances carry a pre-rendered
   visibility/icon shape in their entrance special, and hidden lin units
   carry U_HIDDEN_PATH in u_dir.  Returns 1 and fills *sid when the unit
   must not use its class's plain baked shape. */
static unsigned char render_shape_for_unit(unsigned int unit, unsigned int *sid) {
    unsigned int tile = classes[u_cls[unit]].tile;
    unsigned char i;
    if (tile == TILE_lvl) {
        for (i = 0; i < spec_count; i++) {
            if (spec_unit[i] == unit && spec_shape[i] != 0xFFFF) {
                *sid = spec_shape[i];
                return 1;
            }
        }
    } else if (tile == TILE_lin && (u_dir[unit] & U_HIDDEN_PATH)) {
        *sid = SHAPE_LIN_HIDDEN;
        return SHAPE_LIN_HIDDEN != 0xFFFF;
    }
    return 0;
}

static void gen_composite(const unsigned int *stack, unsigned char n, unsigned char slot) {
    unsigned char *pix = scratch;             /* 256 SMS colours */
    unsigned char *shape = scratch + 256;     /* 128 packed */
    unsigned char *tile = scratch + 384;      /* 32 */
    unsigned char sms[16];
    unsigned char i, q, x, y;
    unsigned int ui;
    { unsigned char *pp = pix; for (i = 0; i < 128; i++) { *pp++ = level_bg; *pp++ = level_bg; } }
    for (i = 0; i < n; i++) {
        ClassInfo *c;
        unsigned char fx, fy, vk;
        unsigned int sid;
        ui = stack[i];
        c = &classes[u_cls[ui]];
        vk = variant_of(ui, &fx, &fy);
        if (!render_shape_for_unit(ui, &sid)) {
            unsigned int sh[4];
            load_tile_shapes(c->tile, sh);
            if (vk == VK_ROT) sid = sh[1];
            else if (vk == VK_DIM) sid = (c->flags & TF_HASDIM) ? sh[3] : sh[0];
            else sid = sh[0];
        }
        if (sid == 0xFFFF) continue;
        load_shape(sid, shape);
        load_sms_table(c, vk == VK_DIM, sms);
        if (!fx && !fy) {
            /* Packed shape order is exactly pix order: walk linearly. */
            unsigned char *pp = pix;
            unsigned char *sp = shape;
            for (y = 0; y < 128; y++) {
                unsigned char packed = *sp++;
                unsigned char v = packed >> 4;
                if (v) *pp = sms[v - 1];
                pp++;
                v = packed & 15;
                if (v) *pp = sms[v - 1];
                pp++;
            }
        } else {
        for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) {
            unsigned char sx = fx ? 15 - x : x;
            unsigned char sy = fy ? 15 - y : y;
            unsigned char packed = shape[sy * 8 + (sx >> 1)];
            unsigned char v = (sx & 1) ? (packed & 15) : (packed >> 4);
            if (v) pix[y * 16 + x] = sms[v - 1];
        }
        }
    }
    comp_palbits[slot] = 0;
    for (q = 0; q < 4; q++) {
        unsigned char pal = pack_quadrant(pix, sms, 0, (q & 1) * 8, (q >> 1) * 8, tile);
        if (pal) comp_palbits[slot] |= (1 << q);
        SMS_loadTiles(tile, COMP_VRAM0 + slot * 4 + q, 32);
    }
}

void camera_update(void) {
    unsigned int i;
    unsigned char found = 0, tx = 0, ty = 0;
    unsigned int sx = 0, sy = 0, cnt = 0;
    for (i = 0; i < nunits; i++) {
        if (u_cls[i] == DEAD_CLS) continue;
        if (cls_hot2[u_cls[i]] & (CHF2_P1 | CHF2_P2)) {
            sx += u_x[i]; sy += u_y[i]; cnt++; found = 1;
        }
    }
    if (!found) return;
    tx = sx / cnt; ty = sy / cnt;
    {
        signed int ncx = (signed int)tx - 8;
        signed int ncy = (signed int)ty - 6;
        unsigned char maxx = bw > VIEW_W ? bw - VIEW_W : 0;
        unsigned char maxy = bh > VIEW_H ? bh - VIEW_H : 0;
        if (ncx < 0) ncx = 0; if (ncx > maxx) ncx = maxx;
        if (ncy < 0) ncy = 0; if (ncy > maxy) ncy = maxy;
        /* only move camera when target approaches the edge */
        if (tx >= cam_x + 3 && tx < cam_x + VIEW_W - 3) ncx = cam_x;
        if (ty >= cam_y + 2 && ty < cam_y + VIEW_H - 2) ncy = cam_y;
        if (ncx != cam_x || ncy != cam_y) {
            cam_x = (unsigned char)ncx; cam_y = (unsigned char)ncy;
            need_full_redraw = 1;
            board_dirty = 1;
        }
    }
}

/* Upstream updateUnitSprite() draws curse units 24 layers higher, so
   the map selctr stays visible on top of the lvl/lin cell it occupies. */
static unsigned char render_layer(unsigned char cls) {
    unsigned int l = classes[cls].layer;
    if (cls_hot3[cls] & CHF3_CURSE) l += 24;
    return l > 255 ? 255 : (unsigned char)l;
}

/* Build the layer-sorted stack for one board cell.  This is only needed
   when a changed multi-unit cell requires a generated composite; the
   common draw path below scans the unit list once instead of rescanning
   a row for every one of the 192 view cells. */
#define DF_STEALTH 1
#define DF_SPECIAL 2
static unsigned char build_cell_stack(unsigned char bx, unsigned char by,
                                      unsigned int *stack,
                                      const unsigned char *cls_dflags) {
    unsigned char sn = 0;
    unsigned int ui;
    for (ui = row_head[by]; ui != NIL_UNIT; ui = u_next[ui]) {
        unsigned char lay, pos;
        if (u_x[ui] != bx || u_cls[ui] == DEAD_CLS) continue;
        if (cls_dflags[u_cls[ui]] & DF_STEALTH) continue;
        lay = render_layer(u_cls[ui]);
        pos = sn;
        while (pos > 0 && render_layer(u_cls[stack[pos - 1]]) > lay) {
            if (pos < 8) stack[pos] = stack[pos - 1];
            pos--;
        }
        if (pos < 8) { stack[pos] = ui; if (sn < 8) sn++; }
    }
    return sn;
}

static void put_cell_tiles(unsigned char sx, unsigned char sy, const unsigned int *e) {
    SMS_setNextTileatXY(sx * 2, sy * 2);
    SMS_setTile(e[0]);
    SMS_setTile(e[1]);
    SMS_setNextTileatXY(sx * 2, sy * 2 + 1);
    SMS_setTile(e[2]);
    SMS_setTile(e[3]);
}

void draw_board(void) {
    unsigned char sx, sy;
    unsigned char offx = bw < VIEW_W ? (VIEW_W - bw) / 2 : 0;
    unsigned char offy = bh < VIEW_H ? (VIEW_H - bh) / 2 : 0;
    /* Scratch overlays (gen_composite uses only scratch[0..415]). */
    unsigned int *cell_key = (unsigned int *)(scratch + 448);       /* 192*2 */
    unsigned char *cell_count = scratch + 832;                      /* 192 */
    unsigned char *cell_top = scratch + 1024;                       /* 192 */
    unsigned char *changed_list = scratch + 1216;                   /* 192 */
    unsigned char *cls_dflags = scratch + 1408;                     /* MAXCLASS */
    unsigned char *cls_layer = scratch + 1444;                      /* MAXCLASS */
    unsigned char idx, ui, nun = (unsigned char)nunits;
    unsigned char ci;

    frame_stamp++;
    if (!frame_stamp) frame_stamp = 1;
    comp_evict_happened = 0;

    {
        ClassInfo *cp = classes;
        unsigned char *ph = cls_hot3, *pdf = cls_dflags, *pl = cls_layer;
        for (ci = 0; ci < nclass; ci++, cp++) {
            unsigned int l = cp->layer;
            unsigned char h3 = *ph++;
            unsigned char df = 0;
            if (h3 & CHF3_STELTH) df |= DF_STEALTH;
            if (cp->tile == TILE_lvl || cp->tile == TILE_lin) df |= DF_SPECIAL;
            *pdf++ = df;
            if (h3 & CHF3_CURSE) l += 24;
            *pl++ = l > 255 ? 255 : (unsigned char)l;
        }
    }
    draw_clear_cells();

    /* One pass over units: compute each visible cell's change key,
       visible-unit count and topmost unit.  Ascending unit order gives
       the same equal-layer top choice as the former row-chain sort.
       View coordinates use wrapping 8-bit subtraction: off-view board
       coordinates always wrap to >= VIEW_W/VIEW_H for legal levels. */
    {
        unsigned char *pcls = u_cls, *px = u_x, *py = u_y, *pdir = u_dir;
        for (ui = 0; ui < nun; ui++, pcls++, px++, py++, pdir++) {
            unsigned char cls = *pcls;
            unsigned char vx, vy, df;
            unsigned int *kp;
            unsigned char *cntp, *topp;
            if (cls == DEAD_CLS || cls >= nclass) continue;
            df = cls_dflags[cls];
            if (df & DF_STEALTH) continue;
            vx = *px - cam_x + offx;
            if (vx >= VIEW_W) continue;
            vy = *py - cam_y + offy;
            if (vy >= VIEW_H) continue;
            idx = (unsigned char)((vy << 4) + vx);
            kp = cell_key + idx;
            cntp = cell_count + idx;
            topp = cell_top + idx;
            /* Top layer is re-derived from the current top unit's
               class; second-and-later units in a cell are rare, and
               this frees the old top-layer array for the changed list. */
            if (*cntp == 0 ||
                cls_layer[cls] > cls_layer[u_cls[*topp]])
                *topp = ui;
            if (*cntp != 255) (*cntp)++;
            *kp = *kp * 33 ^ ((unsigned int)cls << 4) ^
                  (*pdir & (DIR_MASK | U_ACTIVE | U_HIDDEN_PATH));
            if (df & DF_SPECIAL) {
                unsigned int sid;
                if (render_shape_for_unit(ui, &sid)) *kp ^= sid + 0x9E37;
            }
        }
    }

    {
        /* Assembly scan: appends every cell whose key differs from
           sh_key (all cells on a full redraw) and updates sh_key. */
        unsigned char nch = draw_scan_changed();
        unsigned char k;
        for (k = 0; k < nch; k++) {
            unsigned char cellidx = changed_list[k];
            unsigned char sn = cell_count[cellidx];
            unsigned int key = sn ? cell_key[cellidx] : 0;
            unsigned int e[4];
            unsigned char qi, top = cell_top[cellidx];
            if (sn && !key) key = 0x8001;
            sx = cellidx & 15;
            sy = cellidx >> 4;
            if (!sn) {
                e[0] = e[1] = e[2] = e[3] = 0;
            } else if ((sn == 1 || cls_opaque[u_cls[top]]) &&
                       !render_shape_for_unit(top, &e[0])) {
                cell_entries_direct(top, e);
            } else {
                unsigned int stack[8];
                unsigned char stackn;
                signed int bx = (signed int)cam_x + sx - offx;
                signed int by = (signed int)cam_y + sy - offy;
                unsigned char slot;
                if (bx < 0 || by < 0 || bx >= bw || by >= bh) {
                    e[0] = e[1] = e[2] = e[3] = 0;
                    put_cell_tiles(sx, sy, e);
                    continue;
                }
                stackn = build_cell_stack((unsigned char)bx, (unsigned char)by,
                                          stack, cls_dflags);
                if (stackn == 0) {
                    cell_entries_direct(top, e);
                } else if ((stackn == 1 || cls_opaque[u_cls[stack[stackn - 1]]]) &&
                           !render_shape_for_unit(stack[stackn - 1], &e[0])) {
                    cell_entries_direct(stack[stackn - 1], e);
                } else {
                    for (slot = 0; slot < COMP_SLOTS; slot++)
                        if (comp_key[slot] == key) break;
                    if (slot == COMP_SLOTS) {
                        unsigned char oldest = 0, os = 255;
                        for (slot = 0; slot < COMP_SLOTS; slot++) {
                            if (!comp_key[slot]) { oldest = slot; break; }
                            if (comp_stamp[slot] != frame_stamp && comp_stamp[slot] < os) { os = comp_stamp[slot]; oldest = slot; }
                        }
                        slot = oldest;
                        if (comp_key[slot]) comp_evict_happened = 1;
                        gen_composite(stack, stackn, slot);
                        comp_key[slot] = key;
                    }
                    comp_stamp[slot] = frame_stamp;
                    for (qi = 0; qi < 4; qi++) {
                        e[qi] = COMP_VRAM0 + slot * 4 + qi;
                        if (comp_palbits[slot] & (1 << qi)) e[qi] |= TILE_USE_SPRITE_PALETTE;
                    }
                }
            }
            put_cell_tiles(sx, sy, e);
        }
    }
    need_full_redraw = comp_evict_happened;
    board_dirty = comp_evict_happened;
}
