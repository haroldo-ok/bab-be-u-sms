/* Bab Be U - Sega Master System port.
   Original: "Bab Be U" Love2D fangame by the Bab Be U contributors
   (github.com/lilybeevee/bab-be-u), itself a fangame of Baba Is You
   by Arvi TeikariHempuli).  All game data (levels, sprite shapes,
   palettes, XM music, WAV sfx, font) is converted from the upstream
   repository by tools/convert.py - see CONVERSION.md.
   Built with devkitSMS/SMSlib. */
#include "engine.h"
#include <string.h>

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(0, 6, "SMS port (devkitSMS)", "Bab Be U",
    "Bab Be U fan port to SMS. Original game: github.com/lilybeevee/bab-be-u");

#define ST_TITLE 0
#define ST_SELECT 1
#define ST_GAME 2

static unsigned char state;
static unsigned int entered_from;
static unsigned int sel_cursor;
static unsigned char frame_count;
static unsigned char repeat_timer, undo_timer;
static unsigned char wobble_timer;

/* ---------------- SRAM: saves + undo snapshots ---------------- */
#define SAVE_CLEARED_OFF 8
#define SAVE_BONUS_OFF 72
#define SNAP_SIZE 1920
#define SNAP_SLOTS 16
static unsigned char snap_head, snap_count;

static void sram_write_byte(unsigned char bank, unsigned int off, unsigned char v) {
    SMS_enableSRAMBank(bank);
    SMS_SRAM[off] = v;
    SMS_disableSRAM();
}
static unsigned char sram_read_byte(unsigned char bank, unsigned int off) {
    unsigned char v;
    SMS_enableSRAMBank(bank);
    v = SMS_SRAM[off];
    SMS_disableSRAM();
    return v;
}
static void sram_init(void) {
    unsigned int i;
    if (sram_read_byte(0, 0) != 'B' || sram_read_byte(0, 1) != 'A' ||
        sram_read_byte(0, 2) != 'B' || sram_read_byte(0, 3) != 'U') {
        SMS_enableSRAMBank(0);
        SMS_SRAM[0] = 'B'; SMS_SRAM[1] = 'A'; SMS_SRAM[2] = 'B'; SMS_SRAM[3] = 'U';
        SMS_SRAM[4] = 1;
        for (i = SAVE_CLEARED_OFF; i < SAVE_BONUS_OFF + 64; i++) SMS_SRAM[i] = 0;
        SMS_disableSRAM();
    }
    snap_head = 0; snap_count = 0;
}
static void save_set(unsigned int base, unsigned int id, unsigned char on) {
    unsigned int off = base + (id >> 3);
    unsigned char b = sram_read_byte(0, (unsigned int)off);
    if (on) b |= (1 << (id & 7)); else b &= ~(1 << (id & 7));
    sram_write_byte(0, (unsigned int)off, b);
}
static unsigned char save_get(unsigned int base, unsigned int id) {
    return (sram_read_byte(0, (unsigned int)(base + (id >> 3))) >> (id & 7)) & 1;
}

static void snap_slot_addr(unsigned char slot, unsigned char *bank, unsigned int *off) {
    if (slot < 8) { *bank = 0; *off = 512 + slot * SNAP_SIZE; }
    else { *bank = 1; *off = (slot - 8) * SNAP_SIZE; }
}
void snapshot_push(void) {
    unsigned char bank, pi, n = (unsigned char)nunits;
    unsigned int off;
    unsigned char *sp;
    unsigned char *pc = u_cls, *px = u_x, *py = u_y, *pd = u_dir;
    snap_slot_addr(snap_head, &bank, &off);
    SMS_enableSRAMBank(bank);
    sp = SMS_SRAM + off;
    *sp++ = (unsigned char)nunits;
    *sp++ = (unsigned char)(nunits >> 8);
    *sp++ = (unsigned char)turn_no;
    *sp++ = (unsigned char)(turn_no >> 8);
    while (n) {
        *sp++ = *pc++; *sp++ = *px++;
        *sp++ = *py++; *sp++ = *pd++;
        n--;
    }
    *sp++ = paint_count;
    for (pi = 0; pi < paint_count; pi++) {
        *sp++ = paint_unit[pi];
        *sp++ = 0;              /* unit indices are 8-bit; format keeps a high byte */
        *sp++ = paint_coord[pi];
    }
    *sp++ = (unsigned char)rng_state;
    *sp = (unsigned char)(rng_state >> 8);
    SMS_disableSRAM();
    snap_head = (snap_head + 1) & (SNAP_SLOTS - 1);
    if (snap_count < SNAP_SLOTS - 1) snap_count++;
}

unsigned char snapshot_undo(void) {
    unsigned char bank, pi;
    unsigned int off, i, nn;
    unsigned char *sp;
    unsigned char *pc, *px, *py, *pd, *pu, *pcoord;
    /* remember NO UNDO units (they keep their current state) */
    unsigned int *keep_slot = (unsigned int *)scratch;            /* overlay (consumed before parse) */
    unsigned char *keep_data = scratch + 64;  /* [32][4] flat */
    unsigned char keep_n = 0;
    if (!snap_count) return 0;
    for (i = 0; i < nunits && keep_n < 32; i++) {
        if (u_cls[i] != DEAD_CLS && has_prop_u(i, PROP_noundo)) {
            keep_slot[keep_n] = i;
            keep_data[keep_n * 4 + 0] = u_cls[i]; keep_data[keep_n * 4 + 1] = u_x[i];
            keep_data[keep_n * 4 + 2] = u_y[i]; keep_data[keep_n * 4 + 3] = u_dir[i];
            keep_n++;
        }
    }
    snap_head = (snap_head + SNAP_SLOTS - 1) & (SNAP_SLOTS - 1);
    snap_count--;
    snap_slot_addr(snap_head, &bank, &off);
    SMS_enableSRAMBank(bank);
    nn = SMS_SRAM[off] | ((unsigned int)SMS_SRAM[off + 1] << 8);
    turn_no = SMS_SRAM[off + 2] | ((unsigned int)SMS_SRAM[off + 3] << 8);
    sp = SMS_SRAM + off + 4;
    nunits = nn;
    pc = u_cls; px = u_x; py = u_y; pd = u_dir;
    i = nn;
    while (i) {
        *pc++ = *sp++; *px++ = *sp++;
        *py++ = *sp++; *pd++ = *sp++;
        i--;
    }
    paint_count = *sp++;
    if (paint_count > MAXPAINT) paint_count = MAXPAINT;
    pu = paint_unit; pcoord = paint_coord;
    for (pi = 0; pi < paint_count; pi++) {
        *pu++ = sp[0];          /* sp[1] is the format's unused high byte */
        sp += 2;
        *pcoord++ = *sp++;
    }
    rng_state = sp[0] | ((unsigned int)sp[1] << 8);
    SMS_disableSRAM();
    for (pi = 0; pi < keep_n; pi++) {
        if (keep_slot[pi] < nunits) {
            i = keep_slot[pi];
            u_cls[i] = keep_data[pi * 4 + 0]; u_x[i] = keep_data[pi * 4 + 1];
            u_y[i] = keep_data[pi * 4 + 2]; u_dir[i] = keep_data[pi * 4 + 3];
        }
    }
    build_rows();
    parse_rules();
    camera_update();
    need_full_redraw = 1;
    return 1;
}
static unsigned char undo_blocked_by_lvl(void) {
    unsigned char c;
    for (c = 0; c < nclass; c++)
        if (classes[c].tile == TILE_lvl && has_prop_c(c, PROP_noundo)) return 1;
    return 0;
}

/* ---------------- level loading ---------------- */

static unsigned char load_blob(unsigned char bank, unsigned int off) {
    unsigned int i, p, sms_cursor;
    unsigned char nsp;
    level_bank = bank;
    level_off = off;
    bw = bread(bank, off);
    bh = bread(bank, off + 1);
    nclass = bread(bank, off + 2);
    nunits = bread16(bank, off + 4);
    parent_level = bread16(bank, off + 6);
    next_level = bread16(bank, off + 8);
    level_music = bread(bank, off + 10);
    level_bg = bread(bank, off + 12);
    if (!bw || !bh || bw > 128 || bh > 128 || !nclass || nclass > MAXCLASS || nunits > MAXUNITS)
        return 0;
    sms_cursor = 13 + (unsigned int)nclass * 2;
    for (i = 0; i < nclass; i++) {
        unsigned int tile = bread16(bank, off + 13 + i * 2);
        load_tile_entry(tile, &classes[i]);
    }
    /* resolve rel (temporarily held in sms_off by load_tile_entry) and set sms offsets */
    for (i = 0; i < nclass; i++) {
        unsigned int rel_tile = classes[i].sms_off;
        signed char rc = -1;
        unsigned char j;
        if (rel_tile != 0xFFFF) {
            for (j = 0; j < nclass; j++)
                if (classes[j].tile == rel_tile) { rc = (signed char)j; break; }
        }
        classes[i].rel_cls = rc;
        classes[i].sms_off = sms_cursor;
        sms_cursor += classes[i].nsrc * ((classes[i].flags & TF_TEXT) ? 2 : 1);
    }
    p = off + sms_cursor;
    for (i = 0; i < nunits; i++) {
        u_cls[i] = bread(bank, p); u_x[i] = bread(bank, p + 1);
        u_y[i] = bread(bank, p + 2);
        u_dir[i] = bread(bank, p + 3) & (DIR_MASK | U_HIDDEN_PATH);
        if (u_cls[i] >= nclass) u_cls[i] = DEAD_CLS;
        if (!(u_dir[i] & DIR_MASK)) u_dir[i] |= 1;
        p += 4;
    }
    spec_count = 0; custom_count = 0;
    nsp = bread(bank, p); p++;
    for (i = 0; i < nsp; i++) {
        unsigned int unit = bread16(bank, p);
        unsigned char kind = bread(bank, p + 2);
        if (kind == 1) {
            if (spec_count < MAXSPECIALS) {
                unsigned int target = bread16(bank, p + 3);
                unsigned char vis = bread(bank, p + 5);
                spec_unit[spec_count] = unit;
                spec_target[spec_count] = target;
                spec_vis[spec_count] = vis;
                /* Open lvl units switch to their lvl_won+icon shape once
                   the target level is marked cleared in SRAM. */
                spec_shape[spec_count] = (vis == 0 && target < NUM_LEVELS &&
                    save_get(SAVE_CLEARED_OFF, target)) ? bread16(bank, p + 8)
                                                        : bread16(bank, p + 6);
                spec_count++;
            }
            p += 10;
        } else if (kind == 2) {
            unsigned char len = bread(bank, p + 3);
            unsigned char j;
            if (custom_count < MAXCUSTOM) {
                custom_unit[custom_count] = unit;
                for (j = 0; j < len && j < 6; j++)
                    custom_str[custom_count][j] = bread(bank, p + 4 + j);
                custom_str[custom_count][len < 6 ? len : 6] = 0;
                custom_count++;
            }
            p += 4 + len;
        } else {
            break;
        }
    }
    return 1;
}

static unsigned char find_level_ptr(unsigned int level_id, unsigned char *bank, unsigned int *off, unsigned char *reason) {
    unsigned int m;
    unsigned char e[13];
    for (m = 0; m < NUM_LEVELS; m++) {
        get_level_dir(m, e);
        if ((e[7] | ((unsigned int)e[8] << 8)) == level_id) {
            *bank = e[0];
            *off = e[1] | ((unsigned int)e[2] << 8);
            *reason = e[9];
            return 1;
        }
    }
    return 0;
}

static void start_level_common(void) {
    cam_x = cam_y = 0;
    engine_reset_level_state();
    parse_rules();
    camera_update();
    need_full_redraw = 1;
    snap_head = 0; snap_count = 0;
    repeat_timer = 0; undo_timer = 0;
}

static void draw_select(void);

void load_level_by_id(unsigned int level_id, unsigned int from_id) {
    unsigned char bank, reason;
    unsigned int off;
    if (!find_level_ptr(level_id, &bank, &off, &reason) || reason) {
        sfx_play(SFX_fail);
        return;
    }
    entered_from = from_id;
    cur_level = level_id;
    music_stop();
    SMS_displayOff();
    vram_start = 1;
    if (!load_blob(bank, off)) {
        state = ST_SELECT;
        draw_select();
        return;
    }
    bake_level_tiles();
    clear_screen(0);
    start_level_common();
    state = ST_GAME;
    draw_board();              /* complete first frame while display is off */
    SMS_displayOn();
    music_play(level_music);
    dbg_stage = 6;
}

static void load_title(void) {
    dbg_stage = 1;
    music_stop();
    SMS_displayOff();
    /* The generated font occupies tiles 1..84.  Keep the title-level
       tiles safely above it so load_font() cannot overwrite the board. */
    vram_start = 96;
    cur_level = 0xFFFF;
    load_blob(GD_TITLE_BANK, GD_TITLE_OFF);
    dbg_stage = 2;
    bake_level_tiles();
    dbg_stage = 3;
    load_font();           /* font tiles 1..84 coexist with title tiles at 96+ */
    clear_screen(0);
    cam_x = cam_y = 0;
    engine_reset_level_state();
    dbg_stage = 4;
    parse_rules();
    dbg_stage = 5;
    need_full_redraw = 1;
    state = ST_TITLE;
    draw_board();              /* complete first frame while display is off */
    /* The title floor occupies board row 8 (tile rows 16-17), so keep
       the overlay menu in the blank rows below it. */
    print_str2(7, 19, "1: PLAY - THE MAPP", 0);
    print_str2(7, 21, "2: LEVEL SELECT", 0);
    print_str2(1, 23, "DPAD MOVE  1 UNDO  2 WAIT/ENTER", 0);
    SMS_displayOn();
    music_play(level_music);
    dbg_stage = 7;             /* title is actually ready for input */
}

static void draw_select(void) {
    unsigned int page = sel_cursor / 14;
    unsigned int i;
    SMS_displayOff();
    load_font();
    set_menu_palette();
    clear_screen(0);
    print_str2(10, 0, "LEVEL SELECT", TILE_USE_SPRITE_PALETTE);
    for (i = 0; i < 14; i++) {
        unsigned int m = page * 14 + i;
        unsigned char e[13];
        unsigned int lid;
        if (m >= NUM_LEVELS) break;
        get_level_dir(m, e);
        lid = e[7] | ((unsigned int)e[8] << 8);
        print_str(3, 2 + (unsigned char)i, (const char *)"");
        print_str_banked_name(3, 2 + (unsigned char)i, e[3] | ((unsigned int)e[4] << 8));
        if (e[9]) {
            print_str(30, 2 + (unsigned char)i, "X");
        } else if (save_get(SAVE_CLEARED_OFF, lid)) {
            print_str2(29, 2 + (unsigned char)i, ":)", TILE_USE_SPRITE_PALETTE);
        }
        if (save_get(SAVE_BONUS_OFF, lid)) print_str(28, 2 + (unsigned char)i, "o");
    }
    print_str(1, 2 + (unsigned char)(sel_cursor % 14), ">");
    {
        unsigned char e[13];
        char buf[32];
        get_level_dir(sel_cursor, e);
        print_str_banked_name(1, 18, e[5] | ((unsigned int)e[6] << 8)); /* world */
        if (e[9]) {
            const char *r = "CANT FIT ON SMS";
            if (e[9] == 1) r = "TOO BIG FOR SMS RAM";
            else if (e[9] == 2) r = "TOO MANY UNITS FOR SMS";
            else if (e[9] == 3) r = "TOO MANY TILES FOR VRAM";
            else if (e[9] == 4) r = "MISSING TILE UPSTREAM";
            print_str2(1, 20, r, TILE_USE_SPRITE_PALETTE);
        }
        buf[0] = 0;
        print_str(1, 22, "1 PLAY  2 BACK  L/R PAGE");
    }
    SMS_displayOn();
}

static void goto_select(void) {
    unsigned int m;
    music_stop();
    state = ST_SELECT;
    for (m = 0; m < NUM_LEVELS; m++)
        if (level_menu_to_id[m] == cur_level) { sel_cursor = m; break; }
    draw_select();
    music_play(1);
}

/* ---------------- input helpers ---------------- */

static unsigned char pad_dir(unsigned int keys, unsigned char pad2) {
    if (!pad2) {
        if (keys & PORT_A_KEY_RIGHT) return 1;
        if (keys & PORT_A_KEY_DOWN) return 3;
        if (keys & PORT_A_KEY_LEFT) return 5;
        if (keys & PORT_A_KEY_UP) return 7;
    } else {
        if (keys & PORT_B_KEY_RIGHT) return 1;
        if (keys & PORT_B_KEY_DOWN) return 3;
        if (keys & PORT_B_KEY_LEFT) return 5;
        if (keys & PORT_B_KEY_UP) return 7;
    }
    return 0;
}

/* Gameplay direction in the engine's 1..8 encoding.  An SMS D-pad can
   report two adjacent contacts at once, so combine the axes instead
   of snapping a diagonal press to whichever cardinal is tested first.
   Opposite contacts cancel.  The level selector keeps pad_dir above
   because its cursor/page controls are intentionally cardinal. */
static unsigned char pad_dir8(unsigned int keys, unsigned char pad2) {
    signed char x = 0, y = 0;
    if (!pad2) {
        if (keys & PORT_A_KEY_RIGHT) x++;
        if (keys & PORT_A_KEY_LEFT) x--;
        if (keys & PORT_A_KEY_DOWN) y++;
        if (keys & PORT_A_KEY_UP) y--;
    } else {
        if (keys & PORT_B_KEY_RIGHT) x++;
        if (keys & PORT_B_KEY_LEFT) x--;
        if (keys & PORT_B_KEY_DOWN) y++;
        if (keys & PORT_B_KEY_UP) y--;
    }
    if (x > 0) return y > 0 ? 2 : (y < 0 ? 8 : 1);  /* DR / UR / R */
    if (x < 0) return y > 0 ? 4 : (y < 0 ? 6 : 5);  /* DL / UL / L */
    return y > 0 ? 3 : (y < 0 ? 7 : 0);             /* D / U / none */
}

static unsigned int active_hash(void) {
    unsigned int h = 0, i;
    for (i = 0; i < nunits; i++)
        if (u_dir[i] & U_ACTIVE) h = h * 33 + i + ((unsigned int)u_x[i] << 4) + u_y[i];
    return h;
}

static void after_turn(unsigned char result) {
    if (result == 1) {
        unsigned char f;
        sfx_play(SFX_win);
        save_set(SAVE_CLEARED_OFF, cur_level, 1);
        if (level_bonus_got) save_set(SAVE_BONUS_OFF, cur_level, 1);
        for (f = 0; f < 70; f++) SMS_waitForVBlank();
        if (entered_from != 0xFFFF) load_level_by_id(entered_from, 0xFFFF);
        else if (parent_level != 0xFFFF && parent_level != cur_level) load_level_by_id(parent_level, 0xFFFF);
        else goto_select();
    } else if (result == 2) {
        sfx_play(SFX_fail);
        save_set(SAVE_CLEARED_OFF, cur_level, 0);
        save_set(SAVE_BONUS_OFF, cur_level, 0);
        if (entered_from != 0xFFFF) load_level_by_id(entered_from, 0xFFFF);
        else if (parent_level != 0xFFFF && parent_level != cur_level) load_level_by_id(parent_level, 0xFFFF);
        else goto_select();
    } else if (result == 3) {
        if (next_level != 0xFFFF) load_level_by_id(next_level, cur_level);
    } else if (result == 4 || result == 5) {
        if (result == 5) sfx_play(SFX_fail);
        load_level_by_id(cur_level, entered_from);
    } else if (result == 6) {
        while (undo_prop_count && snapshot_undo()) undo_prop_count--;
        undo_prop_count = 0;
    }
}

static void run_turn(unsigned char d1, unsigned char d2) {
    unsigned int before = active_hash();
    unsigned char result;
    snapshot_push();
    result = do_turn(d1, d2);
    if (active_hash() != before && result != 1) sfx_play(SFX_rule);
    else if ((d1 || d2) && result != 1) sfx_play(SFX_move);
    /* A board that fits the view is always shown from (0,0). */
    if (bw > VIEW_W || bh > VIEW_H || cam_x || cam_y) camera_update();
    board_dirty = 1;
    after_turn(result);
}

/* ---------------- per-state frame ---------------- */

static void title_frame(void) {
    unsigned int pressed = SMS_getKeysPressed();
    SMS_getKeysStatus();
    if (pressed & PORT_A_KEY_1) {
        load_level_by_id(level_menu_to_id[FIRST_LEVEL_MENU], 0xFFFF);
        return;
    }
    if (pressed & PORT_A_KEY_2) { sel_cursor = FIRST_LEVEL_MENU; state = ST_SELECT; music_stop(); draw_select(); music_play(1); return; }
    SMS_waitForVBlank();
}

static void select_frame(void) {
    unsigned int pressed = SMS_getKeysPressed();
    unsigned int held = SMS_getKeysStatus();
    unsigned char d = pad_dir(held, 0);
    static unsigned char srepeat;
    if (pressed & PORT_A_KEY_1) {
        unsigned int lid = level_menu_to_id[sel_cursor];
        load_level_by_id(lid, 0xFFFF);
        return;
    }
    if (pressed & PORT_A_KEY_2) { load_title(); return; }
    if (d && (!srepeat || srepeat > 8)) {
        if (!(srepeat & 1) || srepeat > 8) { }
        if ((srepeat & 3) == 0) {
            if (d == 3 && sel_cursor + 1 < NUM_LEVELS) sel_cursor++;
            if (d == 7 && sel_cursor > 0) sel_cursor--;
            if (d == 1) sel_cursor = (sel_cursor + 14 < NUM_LEVELS) ? sel_cursor + 14 : sel_cursor;
            if (d == 5) sel_cursor = sel_cursor >= 14 ? sel_cursor - 14 : sel_cursor;
            draw_select();
        }
        srepeat++;
    } else if (!d) srepeat = 0;
    SMS_waitForVBlank();
}

static void game_frame(void) {
    unsigned int pressed = SMS_getKeysPressed();
    unsigned int held = SMS_getKeysStatus();
    unsigned char d1 = pad_dir8(held, 0);
    unsigned char d2 = pad_dir8(held, 1);
    if (SMS_queryPauseRequested()) {
        SMS_resetPauseRequest();
        goto_select();
        return;
    }
    rng_state ^= frame_count;
    if ((pressed & PORT_A_KEY_1) && (held & PORT_A_KEY_2)) {
        load_level_by_id(cur_level, entered_from);
        return;
    }
    if ((pressed & PORT_A_KEY_2) && !(held & PORT_A_KEY_1)) {
        unsigned int ent = find_lvl_entrance();
        if (ent != 0xFFFF) {
            sfx_play(SFX_clicc);
            load_level_by_id(ent, cur_level);
            return;
        }
        run_turn(0, 0);
    } else if (pressed & PORT_A_KEY_1 || ((held & PORT_A_KEY_1) && undo_timer == 0 && !(held & PORT_A_KEY_2))) {
        if (!undo_blocked_by_lvl() && snapshot_undo()) {
            board_dirty = 1;
            sfx_play(SFX_undo);
        } else sfx_play(SFX_fail);
        undo_timer = 7;
    } else if ((d1 || d2) && repeat_timer == 0) {
        run_turn(d1, d2);
        repeat_timer = 9;
    }
    if (state != ST_GAME) return;
    if (repeat_timer) repeat_timer--;
    if (undo_timer) undo_timer--;
    SMS_waitForVBlank();
    wobble_timer++;
    if (wobble_timer >= 12) { wobble_timer = 0; upload_wobble_frame(); }
    /* The tilemap can only change after a turn/undo/load/camera move.
       Avoid a full 192-cell key scan on every idle console frame. */
    if (board_dirty || need_full_redraw) draw_board();
    frame_count++;
}

void main(void) {
    SMS_displayOff();
    render_init();
    load_font();
    set_menu_palette();
    clear_screen(0);
    audio_init();
    sram_init();
    /* Music/SFX are ticked by the VBlank (frame) interrupt, not by the
       main loop, so they keep time during multi-frame turns/redraws. */
    SMS_setFrameInterruptHandler(audio_frame);
    entered_from = 0xFFFF;
    sel_cursor = FIRST_LEVEL_MENU;
    load_title();
    for (;;) {
        switch (state) {
            case ST_TITLE: title_frame(); break;
            case ST_SELECT: select_frame(); break;
            default: game_frame(); break;
        }
    }
}
