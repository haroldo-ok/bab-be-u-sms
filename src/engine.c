/* Bab Be U SMS rule engine: sentence parser + turn simulation.
   Ported from the upstream Lua engine (game/parser.lua, game/rules.lua,
   game/movement.lua) and the word definitions in assets/tiles/**.json.
   See CONVERSION.md for the per-word implementation status. */
#include "engine.h"
#include <string.h>

ClassInfo classes[MAXCLASS];
unsigned char nclass;
unsigned char u_cls[MAXUNITS];
unsigned char u_x[MAXUNITS];
unsigned char u_y[MAXUNITS];
unsigned char u_dir[MAXUNITS];
unsigned char u_next[MAXUNITS];
unsigned char row_head[128];
unsigned int nunits;
unsigned char bw, bh;

unsigned char cls_props[MAXCLASS][MAXPROPS_PER_CLASS];
unsigned char cls_props_cnt[MAXCLASS][MAXPROPS_PER_CLASS];
unsigned char cls_props_extra[MAXCLASS][MAXPROPS_PER_CLASS];
unsigned char cls_nprops[MAXCLASS];
signed char cls_transform[MAXCLASS];
unsigned char cls_notransform[MAXCLASS];   /* bit0 selfpos bit1 selfneg bit2 resolved */
unsigned char cls_group[MAXCLASS];
unsigned char cls_rel_verb[MAXCLASS][MAXREL_PER_CLASS];
unsigned int cls_rel_tgt[MAXCLASS][MAXREL_PER_CLASS];
unsigned char cls_nrel[MAXCLASS];

unsigned char level_bank;
unsigned int level_off;
unsigned char level_bg;
unsigned int cur_level;
unsigned int parent_level, next_level;
unsigned char level_music;
unsigned int turn_no;
unsigned char turn_waited;

unsigned char paint_unit[MAXPAINT];
unsigned char paint_coord[MAXPAINT];
unsigned char paint_count;
unsigned char spec_unit[MAXSPECIALS];
unsigned int spec_target[MAXSPECIALS];
unsigned int spec_shape[MAXSPECIALS];
unsigned char spec_vis[MAXSPECIALS];
unsigned char spec_count;
unsigned char custom_unit[MAXCUSTOM];
char custom_str[MAXCUSTOM][7];
unsigned char custom_count;
unsigned char level_bonus_got;
unsigned char undo_prop_count;

unsigned char scratch[SCRATCH_SIZE];
unsigned char dbg_stage;

CondRule condrules[MAXCONDRULES];
unsigned char ncondrules;
unsigned char cls_wurd[MAXCLASS];
unsigned char any_giv, any_condrules;
unsigned char cls_hot1[MAXCLASS], cls_hot2[MAXCLASS], cls_hot3[MAXCLASS];
unsigned char cls_anti_hot[MAXCLASS];
unsigned char any_hot1, any_hot2, any_hot3;
unsigned char cls_dir_code[MAXCLASS];
unsigned char any_dir_work;
unsigned char any_transform_work;
unsigned char any_eot_pair;
unsigned char any_u_cell_effects;
unsigned char any_flye_complex;
unsigned char any_rel_stalkmoov;
unsigned char any_rel_spoop;
unsigned char any_rel_copkat;
unsigned char any_rel_yeet;
unsigned char any_rel_creat;
unsigned char any_rel_sing;
unsigned char any_rel_soko;
unsigned char any_diagortho;
signed char empty_trans[4];
unsigned char empty_trans_n;
unsigned char her_wx[MAXCLASS], her_wy[MAXCLASS], her_wdir[MAXCLASS];

/* Hash of the state the rule parser actually depends on (see
   word_state_hash below).  Lets do_turn skip a reparse when no
   word-eligible unit has moved/rotated/appeared/disappeared. */
static unsigned long parsed_word_hash;
static unsigned char parsed_word_valid;

const signed char dx8[9] = {0, 1, 1, 0, -1, -1, -1, 0, 1};
const signed char dy8[9] = {0, 0, 1, 1, 1, 0, -1, -1, -1};

/* build_rows() is implemented in propfast.s.  Even a pointer-walking C
   version cost ~40k cycles for a 75-unit level under SDCC, and this
   runs several times per turn. */

void engine_reset_level_state(void) {
    unsigned char c;
    turn_no = 0;
    paint_count = 0;
    level_bonus_got = 0;
    undo_prop_count = 0;
    parsed_word_valid = 0;
    for (c = 0; c < MAXCLASS; c++) {
        cls_wurd[c] = (c < nclass && (classes[c].flags & TF_TEXT)) ? 1 : 0;
        cls_nprops[c] = 0; cls_nrel[c] = 0;
    }
    build_rows();
}

/* ---------------- property store ---------------- */

unsigned char find_prop(unsigned char cls, unsigned char prop, unsigned char mode) {
    /* mode 0 normal, 1 anti, 2 block; returns index or 0xFF */
    unsigned char i, n;
    unsigned char *pp, *pc;
    if (cls >= MAXCLASS) return 0xFF;
    n = cls_nprops[cls];
    pp = &cls_props[cls][0];
    pc = &cls_props_cnt[cls][0];
    for (i = 0; i < n; i++) {
        if (pp[i] != prop) continue;
        if (mode == 0) {
            if (pc[i] < PCNT_ANTI) return i;
        } else if (mode == 1) {
            if (pc[i] == PCNT_ANTI) return i;
        } else if (pc[i] == PCNT_BLOCK) return i;
    }
    return 0xFF;
}
void prop_append(unsigned char cls, unsigned char prop, unsigned char cnt, unsigned char extra) {
    unsigned char i = cls_nprops[cls];
    if (i >= MAXPROPS_PER_CLASS) return;
    cls_props[cls][i] = prop;
    cls_props_cnt[cls][i] = cnt;
    cls_props_extra[cls][i] = extra;
    cls_nprops[cls] = i + 1;
}
void block_prop(unsigned char cls, unsigned char prop) {
    if (find_prop(cls, prop, 2) == 0xFF) prop_append(cls, prop, PCNT_BLOCK, 0);
}
void add_prop_ex(unsigned char cls, unsigned char prop, unsigned char anti, unsigned char extra);
void add_prop0(unsigned char cls, unsigned char prop) { add_prop_ex(cls, prop, 0, 0); }
void add_prop_ex(unsigned char cls, unsigned char prop, unsigned char anti, unsigned char extra) {
    unsigned char i;
    if (prop >= NUM_PROPS) return;
    if (anti) {
        signed char repl = anti_prop_tab[prop];
        if (repl == (signed char)prop) {
            /* anti of itself (graey): just add normally below */
        } else if (repl >= 0) {
            block_prop(cls, prop);
            add_prop_ex(cls, (unsigned char)repl, 0, extra);
            return;
        } else {
            if (find_prop(cls, prop, 1) == 0xFF) prop_append(cls, prop, PCNT_ANTI, extra);
            return;
        }
    }
    i = find_prop(cls, prop, 0);
    if (i != 0xFF) {
        if (cls_props_cnt[cls][i] < 250) cls_props_cnt[cls][i]++;
        return;
    }
    prop_append(cls, prop, 1, extra);
}
/* has_prop_c()/has_anti_c() are implemented in propfast.s: they are the
   engine's hottest tiny functions and SDCC's IX-frame C version cost
   roughly 450 cycles per call. */
unsigned char count_prop_c(unsigned char cls, unsigned char prop) {
    unsigned char i = find_prop(cls, prop, 0);
    return (i == 0xFF) ? 0 : cls_props_cnt[cls][i];
}
unsigned char prop_extra_of(unsigned char cls, unsigned char prop) {
    unsigned char i = find_prop(cls, prop, 0);
    return (i == 0xFF) ? 0 : cls_props_extra[cls][i];
}

static void rebuild_hot_props(void) {
    unsigned char c, i;
    unsigned char seen_nedkee = 0, seen_fordor = 0, seen_noswim = 0;
    unsigned char seen_hotte = 0, seen_fridgd = 0, seen_sad = 0;
    any_hot1 = any_hot2 = any_hot3 = 0;
    any_dir_work = 0;
    any_transform_work = empty_trans_n != 0;
    any_eot_pair = 0;
    any_u_cell_effects = 0;
    any_flye_complex = 0;
    any_rel_stalkmoov = any_rel_spoop = any_rel_copkat = any_rel_yeet = 0;
    any_rel_creat = any_rel_sing = any_rel_soko = 0;
    any_diagortho = 0;
    for (c = 0; c < MAXCLASS; c++) {
        unsigned char h1 = 0, h2 = 0, h3 = 0, ha = 0;
        signed int dir_sx = 0, dir_sy = 0;
        unsigned char dir_spin = 0, dir_anti = 0, dir_code;
        unsigned char wurd_normal = 0, wurd_anti = 0, wurd_block = 0;
        unsigned char p1diag = 0;
        if (c < nclass) {
            if (classes[c].flye_mode || classes[c].flye != classes[0].flye)
                any_flye_complex = 1;
            if (cls_transform[c] >= 0 && cls_transform[c] != (signed char)c)
                any_transform_work = 1;
            for (i = 0; i < cls_nprops[c]; i++) {
                unsigned char p = cls_props[c][i];
                unsigned char cnt = cls_props_cnt[c][i];
                unsigned char d = prop_dir_of(p);
                if (p == PROP_wurd) {
                    if (cnt == PCNT_ANTI) wurd_anti = 1;
                    else if (cnt == PCNT_BLOCK) wurd_block = 1;
                    else wurd_normal = 1;
                }
                if (cnt == PCNT_ANTI) {
                    if (p == PROP_u) ha |= CHA_U;
                    else if (p == PROP_walk) ha |= CHA_WALK;
                    if (d) dir_anti = 1;
                    continue;
                }
                if (cnt >= PCNT_ANTI) continue;
                switch (p) {
                    case PROP_slep: h1 |= CHF_SLEP; break;
                    case PROP_icy: h1 |= CHF_ICY; break;
                    case PROP_walk: h1 |= CHF_WALK; break;
                    case PROP_go: h1 |= CHF_GO; break;
                    case PROP_haetskye: h1 |= CHF_HAETSKYE; break;
                    case PROP_haetflor: h1 |= CHF_HAETFLOR; break;
                    case PROP_ouch: h1 |= CHF_OUCH; break;
                    case PROP_nuek: h1 |= CHF_NUEK; break;
                    case PROP_moar: h2 |= CHF2_MOAR; break;
                    case PROP_split: h2 |= CHF2_SPLIT; break;
                    case PROP_visitfren: h2 |= CHF2_VISITFREN; break;
                    case PROP_rithere: h2 |= CHF2_RITHERE; break;
                    case PROP_her: h2 |= CHF2_HER; break;
                    case PROP_thr: h2 |= CHF2_THR; break;
                    case PROP_u:
                    case PROP_y_all:
                    case PROP_w: h2 |= CHF2_P1; h3 |= CHF3_ISU; p1diag = 1; break;
                    case PROP_you: h2 |= CHF2_P1; h3 |= CHF3_ISU; break;
                    case PROP_curse: h2 |= CHF2_P1; h3 |= CHF3_CURSE; p1diag = 1; break;
                    case PROP_diag:
                    case PROP_ortho: any_diagortho = 1; break;
                    case PROP_utoo:
                    case PROP_utres: h2 |= CHF2_P2; h3 |= CHF3_ISU; break;
                    case PROP_gone: h3 |= CHF3_GONE; break;
                    case PROP_notranform: h3 |= CHF3_NOTRANFORM; break;
                    case PROP_txtify: h3 |= CHF3_TXTIFY; break;
                    case PROP_thingify: h3 |= CHF3_THINGIFY; break;
                    case PROP_stelth: h3 |= CHF3_STELTH; break;
                    case PROP_undo: h3 |= CHF3_UNDO; break;
                    case PROP_nedkee: seen_nedkee = 1; break;
                    case PROP_fordor: seen_fordor = 1; break;
                    case PROP_noswim: seen_noswim = 1; break;
                    case PROP_hotte: seen_hotte = 1; break;
                    case PROP_fridgd: seen_fridgd = 1; break;
                    case PROP_SAD: seen_sad = 1; break;
                    case PROP_WIN:
                    case PROP_UNWIN:
                    case PROP_BONUS:
                    case PROP_delet:
                    case PROP_nxt:
                    case PROP_tryagain: any_u_cell_effects = 1; break;
                    default: break;
                }
                /* Precompute do_dir_rules' per-class result.  It reads
                   only cls_props, so this is exact even when conditional
                   rules exist.  Anti-direction entries keep the ordered
                   fallback in the movement bank (DIR_CODE_ANTI). */
                if (d) {
                    dir_sx += dx8[d] * (signed int)cnt;
                    dir_sy += dy8[d] * (signed int)cnt;
                } else if (p == PROP_spin) {
                    dir_spin = (unsigned char)((dir_spin + cls_props_extra[c][i] * cnt) & 7);
                } else if (p == PROP_direction) {
                    unsigned char dd = cls_props_extra[c][i] ? cls_props_extra[c][i] : 1;
                    if (dd <= 8) {
                        dir_sx += dx8[dd];
                        dir_sy += dy8[dd];
                    } else dir_anti = 1;
                }
            }
        }
        {
            unsigned char desired = 0, d;
            if (dir_sx || dir_sy) {
                signed char gx = dir_sx > 0 ? 1 : dir_sx < 0 ? -1 : 0;
                signed char gy = dir_sy > 0 ? 1 : dir_sy < 0 ? -1 : 0;
                for (d = 1; d <= 8; d++)
                    if (dx8[d] == gx && dy8[d] == gy) desired = d;
            }
            /* Keep desired/spin even with DIR_CODE_ANTI: the movement
               bank applies them first, then only the small ordered
               anti-direction loop remains there. */
            dir_code = desired | (unsigned char)(dir_spin << 4) |
                       (dir_anti ? DIR_CODE_ANTI : 0);
        }
        cls_dir_code[c] = dir_code;
        if (dir_code) any_dir_work = 1;
        /* Exact copy of word_state_hash(1)'s eligibility test, resolved
           once per parse instead of with three property searches on
           every hash call. */
        if (c < nclass && !wurd_block &&
            ((classes[c].flags & TF_TEXT) || classes[c].tile == TILE_this ||
             (wurd_normal && !wurd_anti)))
            ha |= CHA_WURD_NEXT;
        if (h3 & (CHF3_GONE | CHF3_TXTIFY | CHF3_THINGIFY)) any_transform_work = 1;
        if (c < nclass) {
            if ((h1 & (CHF_OUCH | CHF_NUEK)) || (h2 & 0x3F) || (h3 & CHF3_UNDO))
                cls_group[c] |= CLS_GROUP_EOT;
            if (!((h1 & CHF_SLEP) && !(h1 & CHF_ICY)) &&
                ((h1 & CHF_WALK) || (h2 & (CHF2_P1 | CHF2_P2)) ||
                 (ha & (CHA_U | CHA_WALK))))
                cls_group[c] |= CLS_GROUP_MOVER;
            if (p1diag) cls_group[c] |= CLS_GROUP_P1DIAG;
        }
        cls_hot1[c] = h1; cls_hot2[c] = h2; cls_hot3[c] = h3;
        cls_anti_hot[c] = ha;
        any_hot1 |= h1; any_hot2 |= h2; any_hot3 |= h3;
    }
    if (seen_noswim || seen_sad ||
        (seen_nedkee && seen_fordor) || (seen_hotte && seen_fridgd))
        any_eot_pair = 1;
    if (any_u_cell_effects)
        for (c = 0; c < nclass; c++)
            if (cls_hot3[c] & CHF3_ISU) cls_group[c] |= CLS_GROUP_EOT;
    for (i = 0; i < ncondrules; i++)
        if (condrules[i].verb == VERB_be && (condrules[i].flags & CF_OBJKIND) &&
            (condrules[i].obj == PROP_diag || condrules[i].obj == PROP_ortho))
            any_diagortho = 1;
    for (c = 0; c < nclass; c++) {
        for (i = 0; i < cls_nrel[c]; i++) {
            unsigned char v = cls_rel_verb[c][i] & 0x7F;
            switch (v) {
                case VERB_stalk: any_rel_stalkmoov = 1; cls_group[c] |= CLS_GROUP_MOVER; break;
                case VERB_moov:
                    if (cls_rel_tgt[c][i] >= 0xFF21 && cls_rel_tgt[c][i] <= 0xFF28) {
                        any_rel_stalkmoov = 1;
                        cls_group[c] |= CLS_GROUP_MOVER;
                    }
                    break;
                case VERB_spoop: any_rel_spoop = 1; break;
                case VERB_copkat: any_rel_copkat = 1; break;
                case VERB_yeet: any_rel_yeet = 1; break;
                case VERB_creat: any_rel_creat = 1; cls_group[c] |= CLS_GROUP_EOT; break;
                case VERB_sing: any_rel_sing = 1; cls_group[c] |= CLS_GROUP_EOT; break;
                case VERB_soko: any_rel_soko = 1; break;
                case VERB_giv:
                    if (cls_rel_tgt[c][i] == PROP_diag || cls_rel_tgt[c][i] == PROP_ortho)
                        any_diagortho = 1;
                    break;
                case VERB_snacc:
                case VERB_vs:
                case VERB_paint: any_eot_pair = 1; break;
                default: break;
            }
        }
    }
}

void add_rel(unsigned char cls, unsigned char verb, unsigned int tgt, unsigned char dir) {
    unsigned char i = cls_nrel[cls];
    if (i >= MAXREL_PER_CLASS) return;
    cls_rel_verb[cls][i] = verb;
    cls_rel_tgt[cls][i] = tgt;
    (void)dir;  /* directions now encoded in tgt sentinel for moov */
    cls_nrel[cls] = i + 1;
    if (verb == VERB_giv) any_giv = 1;
}
/* relation with noun target (in converter noun space). dir used by moov/yeet GO^. */
unsigned char has_rel(unsigned char cls, unsigned char verb, unsigned int tgt_tile) {
    unsigned char i;
    for (i = 0; i < cls_nrel[cls]; i++)
        if (cls_rel_verb[cls][i] == verb && cls_rel_tgt[cls][i] == tgt_tile) return 1;
    return 0;
}

/* ---------------- noun matching ---------------- */

static unsigned char class_is_special_obj(unsigned char cls) {
    unsigned int t = classes[cls].tile;
    return t == TILE_lvl || t == TILE_bordr || t == TILE_no1;
}
unsigned char noun_matches_class(unsigned int noun, unsigned char cls) {
    if (noun < 0xFF00) return classes[cls].tile == noun;
    switch (noun) {
        case NOUN_TXT: return (classes[cls].flags & TF_TEXT) != 0;
        case NOUN_EVERY1: return !(classes[cls].flags & TF_TEXT) && !class_is_special_obj(cls);
        case NOUN_EVERY2: return !class_is_special_obj(cls);
        case NOUN_FRENS: return (cls_group[cls] & 1) != 0;
        case NOUN_GROOP: return (cls_group[cls] & 2) != 0;
        default: return 0;
    }
}
unsigned char unit_matches_noun(unsigned int unit, unsigned int noun) {
    return noun_matches_class(noun, u_cls[unit]);
}
signed char local_class_of_tile(unsigned int tile) {
    unsigned char c;
    for (c = 0; c < nclass; c++)
        if (classes[c].tile == tile) return (signed char)c;
    return -1;
}
/* txt_ variant tile id of a global tile (rel field), via tile table */
unsigned int global_rel_tile(unsigned int tile) {
    unsigned int r = sread16(GD_TILETABLE_BANK, (unsigned long)tile * TILE_ENTRY_SIZE + 16);
    SMS_mapROMBank(GD_PARSER_BANK);  /* caller is banked parser code */
    return r;
}

/* ---------------- cell queries ---------------- */

static unsigned char eff_flye_mode(unsigned char cls) { return classes[cls].flye_mode; }
unsigned char same_flye_cls(unsigned char a, unsigned char b) {
    unsigned char ma = classes[a].flye_mode, mb = classes[b].flye_mode;
    signed char fa = classes[a].flye, fb = classes[b].flye;
    if (ma == 3 || mb == 3) return 1;
    if (ma == 1 && mb == 1) return 1;
    if (ma == 2 && mb == 2) return 1;
    if (ma == 1) return fb >= 0 || mb == 2 ? (fb >= 0) : 1;
    if (mb == 1) return fa >= 0;
    if (ma == 2) return fb <= 0;
    if (mb == 2) return fa <= 0;
    return fa == fb;
}
unsigned char same_flye_u(unsigned int a, unsigned int b) {
    /* In most levels every class is mode 0 at the same flye height;
       rebuild_hot_props proves that after each parse. */
    if (!any_flye_complex) return 1;
    return same_flye_cls(u_cls[a], u_cls[b]);
}

/* iterate units in a cell: use row list */
#define FIRST_IN_CELL(x, y) row_head[(y)]
unsigned int cell_next(unsigned int i, unsigned char x) {
    for (i = u_next[i]; i != NIL_UNIT; i = u_next[i])
        if (u_x[i] == x && u_cls[i] != DEAD_CLS) return i;
    return NIL_UNIT;
}
unsigned int cell_first(unsigned char x, unsigned char y) {
    unsigned int i;
    if (y >= 128) return NIL_UNIT;
    for (i = row_head[y]; i != NIL_UNIT; i = u_next[i])
        if (u_x[i] == x && u_cls[i] != DEAD_CLS) return i;
    return NIL_UNIT;
}

/* ---------------- conditions ---------------- */

unsigned char is_colour_cond(unsigned char cond) {
    return cond == COND_blacc || cond == COND_bleu || cond == COND_brwn || cond == COND_cyeann ||
           cond == COND_graey || cond == COND_grun || cond == COND_orang || cond == COND_pinc ||
           cond == COND_purp || cond == COND_reed || cond == COND_whit || cond == COND_yello;
}
signed char unit_colour(unsigned int unit) {
    unsigned char i;
    for (i = 0; i < paint_count; i++)
        if (paint_unit[i] == unit)
            return (paint_coord[i] < 35) ? colour_of_coord_tab[paint_coord[i]] : -1;
    return (classes[u_cls[unit]].mcoord < 35) ? colour_of_coord_tab[classes[u_cls[unit]].mcoord] : -1;
}
static unsigned char ray_find(unsigned int unit, unsigned char dir, unsigned int noun) {
    signed int x = u_x[unit] + dx8[dir];
    signed int y = u_y[unit] + dy8[dir];
    unsigned int i;
    while (x >= 0 && y >= 0 && x < bw && y < bh) {
        unsigned char blocked = 0;
        for (i = cell_first((unsigned char)x, (unsigned char)y); i != NIL_UNIT; i = cell_next(i, (unsigned char)x)) {
            if (unit_matches_noun(i, noun)) return 1;
            if (has_prop_c(u_cls[i], PROP_nogo) && !has_prop_c(u_cls[i], PROP_tranparnt)) blocked = 1;
        }
        if (blocked) return 0;
        x += dx8[dir]; y += dy8[dir];
    }
    return 0;
}
unsigned char eval_cond(unsigned int unit, unsigned char cond, unsigned int cnoun, unsigned char cextra, unsigned char depth);
unsigned char has_prop_u(unsigned char unit, unsigned char prop);

unsigned char eval_cond(unsigned int unit, unsigned char cond, unsigned int cnoun, unsigned char cextra, unsigned char depth) {
    unsigned int i;
    unsigned char x = u_x[unit], y = u_y[unit];
    unsigned char dir = u_dir[unit] & DIR_MASK;
    if (is_colour_cond(cond)) return unit_colour(unit) == (signed char)cond;
    switch (cond) {
        case COND_w_fren:
            for (i = cell_first(x, y); i != NIL_UNIT; i = cell_next(i, x))
                if (i != unit && unit_matches_noun(i, cnoun)) return 1;
            return 0;
        case COND_sans:
            return !eval_cond(unit, COND_w_fren, cnoun, 0, depth);
        case COND_frenles:
            for (i = cell_first(x, y); i != NIL_UNIT; i = cell_next(i, x))
                if (i != unit) return 0;
            return 1;
        case COND_arond: {
            signed char ox, oy;
            for (oy = -1; oy <= 1; oy++) for (ox = -1; ox <= 1; ox++) {
                signed int nx = x + ox, ny = y + oy;
                if ((ox || oy) && nx >= 0 && ny >= 0 && nx < bw && ny < bh)
                    for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx))
                        if (unit_matches_noun(i, cnoun)) return 1;
            }
            return 0;
        }
        case COND_lookat:
            return ray_find(unit, dir ? dir : 1, cnoun);
        case COND_lookaway: {
            unsigned char od = dir ? (unsigned char)(((dir - 1 + 4) & 7) + 1) : 5;
            return ray_find(unit, od, cnoun);
        }
        case COND_behind: {
            signed int nx = x - dx8[dir], ny = y - dy8[dir];
            if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) return 0;
            for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx))
                if (unit_matches_noun(i, cnoun)) return 1;
            return 0;
        }
        case COND_beside: {
            unsigned char d1 = (unsigned char)(((dir - 1 + 2) & 7) + 1);
            unsigned char d2 = (unsigned char)(((dir - 1 + 6) & 7) + 1);
            signed int nx, ny;
            nx = x + dx8[d1]; ny = y + dy8[d1];
            if (nx >= 0 && ny >= 0 && nx < bw && ny < bh)
                for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx))
                    if (unit_matches_noun(i, cnoun)) return 1;
            nx = x + dx8[d2]; ny = y + dy8[d2];
            if (nx >= 0 && ny >= 0 && nx < bw && ny < bh)
                for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx))
                    if (unit_matches_noun(i, cnoun)) return 1;
            return 0;
        }
        case COND_seenby: {
            unsigned char d;
            for (d = 1; d <= 8; d++) {
                /* someone of cnoun looking back along direction d toward us */
                signed int ox = x, oy = y;
                unsigned char od = (unsigned char)(((d - 1 + 4) & 7) + 1);
                for (;;) {
                    unsigned char blocked = 0;
                    ox += dx8[d]; oy += dy8[d];
                    if (ox < 0 || oy < 0 || ox >= bw || oy >= bh) break;
                    for (i = cell_first((unsigned char)ox, (unsigned char)oy); i != NIL_UNIT; i = cell_next(i, (unsigned char)ox)) {
                        if (unit_matches_noun(i, cnoun) && (u_dir[i] & DIR_MASK) == od) return 1;
                        if (has_prop_c(u_cls[i], PROP_nogo) && !has_prop_c(u_cls[i], PROP_tranparnt)) blocked = 1;
                    }
                    if (blocked) break;
                }
            }
            return 0;
        }
        case COND_samepaint: {
            signed char tc = -1;
            unsigned char c;
            for (c = 0; c < nclass; c++)
                if (noun_matches_class(cnoun, c)) { tc = (classes[c].mcoord < 35) ? colour_of_coord_tab[classes[c].mcoord] : -1; break; }
            return tc >= 0 && unit_colour(unit) == tc;
        }
        case COND_thatbe:
        case COND_that:
            if (depth > 1) return 0;
            return has_prop_u(unit, cextra);
        case COND_corekt: return 1;
        case COND_rong: return 0;
        case COND_the: return 1;
        case COND_an: return 1;
        case COND_deez: return 1;
        case COND_meow: return classes[u_cls[unit]].tile == classes[u_cls[unit]].tile && ((rng_next() >> 8) & 7) == 0;
        case COND_mayb: {
            unsigned int h = (unsigned int)(turn_no * 131 + unit * 17 + u_x[unit] * 7 + u_y[unit]);
            return (h & 1) != 0;
        }
        case COND_alt: return (turn_no & 1) != 0;
        case COND_past: return !(u_dir[unit] & U_NEW);
        case COND_timles: return 0;
        case COND_clikt: return 0;
        case COND_oob: return 0;
        case COND_wait: return turn_waited;
        case COND_wun: {
            for (i = 0; i < unit && i < nunits; i++)
                if (u_cls[i] == u_cls[unit]) return 0;
            return 1;
        }
        case COND_lit: {
            unsigned char d;
            if (has_prop_c(u_cls[unit], PROP_brite)) return 1;
            for (d = 1; d <= 7; d += 2) {
                signed int ox = x, oy = y;
                for (;;) {
                    unsigned char blocked = 0;
                    ox += dx8[d]; oy += dy8[d];
                    if (ox < 0 || oy < 0 || ox >= bw || oy >= bh) break;
                    for (i = cell_first((unsigned char)ox, (unsigned char)oy); i != NIL_UNIT; i = cell_next(i, (unsigned char)ox)) {
                        if (has_prop_c(u_cls[i], PROP_brite)) return 1;
                        if (has_prop_c(u_cls[i], PROP_torc) &&
                            (u_dir[i] & DIR_MASK) == (unsigned char)(((d - 1 + 4) & 7) + 1)) return 1;
                        if (has_prop_c(u_cls[i], PROP_nogo) && !has_prop_c(u_cls[i], PROP_tranparnt)) blocked = 1;
                    }
                    if (blocked) break;
                }
            }
            return 0;
        }
        default: return 0;
    }
}

static unsigned char has_prop_u_slow(unsigned char unit, unsigned char prop,
                                     unsigned char cls, unsigned char base) {
    unsigned char r, k;
    if (any_condrules) {
        for (r = 0; r < ncondrules; r++) {
            CondRule *cr = &condrules[r];
            unsigned char ok = 1;
            if (cr->verb != VERB_be || !(cr->flags & CF_OBJKIND) || cr->obj != prop) continue;
            /* CF_SNT: rule applies to classes that do NOT match subj. */
            if (noun_matches_class(cr->subj, cls) == ((cr->flags & CF_SNT) ? 1 : 0)) continue;
            for (k = 0; k < cr->nconds && ok; k++) {
                unsigned char res = eval_cond(unit, cr->cond[k], cr->cnoun[k], cr->cextra[k], 0);
                if (cr->cnt[k]) res = !res;
                if (!res) ok = 0;
            }
            if (!ok) continue;
            if (cr->flags & (CF_VNT | CF_ONT)) return 0;
            base = 1;
        }
    }
    if (base) return 1;
    if (any_giv) {
        unsigned int i;
        for (i = cell_first(u_x[unit], u_y[unit]); i != NIL_UNIT; i = cell_next(i, u_x[unit])) {
            unsigned char j;
            if (i == unit || !same_flye_u(i, unit)) continue;
            for (j = 0; j < cls_nrel[u_cls[i]]; j++)
                if (cls_rel_verb[u_cls[i]][j] == VERB_giv && cls_rel_tgt[u_cls[i]][j] == prop) return 1;
        }
    }
    return 0;
}

unsigned char has_prop_u(unsigned char unit, unsigned char prop) {
    unsigned char cls = u_cls[unit];
    if (cls >= nclass) return 0;
    if (!any_condrules && !any_giv) return has_prop_c(cls, prop);
    return has_prop_u_slow(unit, prop, cls, has_prop_c(cls, prop));
}

unsigned char dict_lookup(const char *s, unsigned char len, unsigned char *kind, unsigned int *ref) {
    unsigned int i;
    unsigned char bank = GD_DICT_TAB_BANK;
    SMS_mapROMBank(bank);
    for (i = 0; i < NUM_DICT; i++) {
        unsigned char *e = (unsigned char *)(0x8000 + GD_DICT_TAB_OFF + i * 6);
        unsigned char elen = e[2];
        unsigned int soff;
        unsigned char *sp;
        unsigned char j, match = 1;
        if (elen != len) continue;
        soff = e[0] | ((unsigned int)e[1] << 8);
        SMS_mapROMBank(GD_DICT_STR_BANK);
        sp = (unsigned char *)(0x8000 + GD_DICT_STR_OFF + soff);
        for (j = 0; j < len; j++) if (sp[j] != (unsigned char)s[j]) { match = 0; break; }
        SMS_mapROMBank(bank);
        if (match) {
            *ref = e[3] | ((unsigned int)e[4] << 8);
            *kind = e[5];
            SMS_mapROMBank(GD_PARSER_BANK);
            return 1;
        }
    }
    SMS_mapROMBank(GD_PARSER_BANK);
    return 0;
}


unsigned char dir_prop_of(unsigned char dir) {
    switch (dir) {
        case 1: return PROP_right; case 2: return PROP_downright;
        case 3: return PROP_down; case 4: return PROP_downleft;
        case 5: return PROP_left; case 6: return PROP_upleft;
        case 7: return PROP_up; case 8: return PROP_upright;
    }
    return PROP_right;
}

unsigned char prop_dir_of(unsigned char prop) {
    /* Direct inverse of dir_prop_of().  The former loop called
       dir_prop_of() eight times and cost ~1.7k cycles per call in
       do_dir_rules' inner loop. */
    switch (prop) {
        case PROP_right: return 1; case PROP_downright: return 2;
        case PROP_down: return 3; case PROP_downleft: return 4;
        case PROP_left: return 5; case PROP_upleft: return 6;
        case PROP_up: return 7; case PROP_upright: return 8;
    }
    return 0;
}

unsigned char cond_is_infix(unsigned char cond) {
    return cond == COND_w_fren || cond == COND_arond || cond == COND_sans ||
           cond == COND_lookat || cond == COND_lookaway || cond == COND_behind ||
           cond == COND_beside || cond == COND_seenby || cond == COND_that ||
           cond == COND_thatbe || cond == COND_samepaint;
}


/* ---------------- unit spawn/destroy ---------------- */

void destroy_unit_raw(unsigned int u) {
    u_cls[u] = DEAD_CLS;
}
unsigned int spawn_unit(unsigned char cls, unsigned char x, unsigned char y, unsigned char dir) {
    unsigned int i;
    for (i = 0; i < nunits; i++)
        if (u_cls[i] == DEAD_CLS) break;
    if (i >= MAXUNITS) return NIL_UNIT;
    if (i == nunits) nunits++;
    u_cls[i] = cls; u_x[i] = x; u_y[i] = y;
    u_dir[i] = (dir & DIR_MASK) | U_NEW;
    u_next[i] = row_head[y];
    row_head[y] = i;
    return i;
}
static void got_spawn(unsigned int dead_unit) {
    unsigned char cls = u_cls[dead_unit];
    unsigned char r;
    for (r = 0; r < cls_nrel[cls]; r++) {
        signed char tc = -1;
        if (cls_rel_verb[cls][r] != VERB_got) continue;
        if (cls_rel_tgt[cls][r] == NOUN_TXT) tc = classes[cls].rel_cls;
        else if (cls_rel_tgt[cls][r] < 0xFF00) tc = local_class_of_tile(cls_rel_tgt[cls][r]);
        if (tc >= 0) spawn_unit((unsigned char)tc, u_x[dead_unit], u_y[dead_unit], u_dir[dead_unit] & DIR_MASK);
    }
}
void destroy_unit(unsigned int u) {
    unsigned char oldcls = u_cls[u];
    unsigned char x = u_x[u], y = u_y[u], d = u_dir[u];
    if (oldcls == DEAD_CLS || has_prop_u(u, PROP_protecc)) return;
    destroy_unit_raw(u);
    /* got drops from the dead unit's class */
    {
        unsigned char r;
        for (r = 0; r < cls_nrel[oldcls]; r++) {
            signed char tc = -1;
            if (cls_rel_verb[oldcls][r] != VERB_got) continue;
            if (cls_rel_tgt[oldcls][r] == NOUN_TXT) tc = classes[oldcls].rel_cls;
            else if (cls_rel_tgt[oldcls][r] < 0xFF00) tc = local_class_of_tile(cls_rel_tgt[oldcls][r]);
            if (tc >= 0) spawn_unit((unsigned char)tc, x, y, d & DIR_MASK);
        }
    }
    (void)got_spawn;
}

unsigned char unit_is_u(unsigned int u) {
    if (!any_condrules && !any_giv) {
        unsigned char cls = u_cls[u];
        return cls < nclass && (cls_hot3[cls] & CHF3_ISU) != 0;
    }
    return has_prop_u((unsigned char)u, PROP_u) || has_prop_u((unsigned char)u, PROP_utoo) ||
           has_prop_u((unsigned char)u, PROP_utres) || has_prop_u((unsigned char)u, PROP_you) ||
           has_prop_u((unsigned char)u, PROP_y_all) || has_prop_u((unsigned char)u, PROP_w);
}
unsigned char any_u_exists(void) {
    unsigned int i;
    for (i = 0; i < nunits; i++)
        if (u_cls[i] != DEAD_CLS && unit_is_u(i)) return 1;
    return 0;
}


/* ---------------- turn ---------------- */


/* Hash every input that can change the parser's output:
 *  - which classes are word-eligible (for next_snapshot=1 this mirrors
 *    the snapshot parse_rules_b would take from the current props;
 *    for 0 it is the cls_wurd snapshot the last parse actually used)
 *  - each eligible unit's class, position and facing (letters and
 *    txt_direction/spin are facing-sensitive)
 *  - which classes have at least one live unit (VERB_rp existence test)
 * Two independent 16-bit hashes are combined. */
static unsigned long word_state_hash(unsigned char next_snapshot) {
    /* elig/present live in scratch: every call site (parse_rules after
       the parser bank returns, do_turn around movement) has scratch
       free, and this avoids a 72-byte IX frame on a per-turn hot path. */
    unsigned char *elig = scratch;
    unsigned char *present = scratch + MAXCLASS;
    unsigned int h1 = 5381, h2 = 0x811C;
    unsigned char c, i, n = (unsigned char)nunits;
    unsigned char *pc = u_cls, *px = u_x, *py = u_y, *pd = u_dir;
    for (c = 0; c < nclass; c++) {
        unsigned char w = next_snapshot ? ((cls_anti_hot[c] & CHA_WURD_NEXT) != 0)
                                        : cls_wurd[c];
        present[c] = 0;
        elig[c] = w;
        if (w) { h1 = h1 * 33 ^ (0xE000 + c); h2 = h2 * 131 ^ (0xE000 + c); }
    }
    for (i = 0; i < n; i++, pc++, px++, py++, pd++) {
        unsigned char cls = *pc;
        unsigned int v;
        if (cls == DEAD_CLS || cls >= nclass) continue;
        present[cls] = 1;
        if (!elig[cls]) continue;
        v = cls | ((unsigned int)*px << 8);
        h1 = h1 * 33 ^ v; h2 = h2 * 131 ^ (v ^ 0x9E37);
        v = *py | ((unsigned int)(*pd & DIR_MASK) << 8) | ((unsigned int)i << 4);
        h1 = h1 * 33 ^ v; h2 = h2 * 131 ^ (v ^ 0x7F4A);
    }
    for (c = 0; c < nclass; c++)
        if (present[c]) { h1 = h1 * 33 ^ (0xA000 + c); h2 = h2 * 131 ^ (0xA000 + c); }
    return ((unsigned long)h1 << 16) | h2;
}

/* ---- banked-code wrappers (parser in GD_PARSER_BANK, movement in GD_MOVE_BANK) ---- */
void parse_rules(void) {
    dbg_stage = 40;
    SMS_mapROMBank(GD_PARSER_BANK);
    parse_rules_b();
    rebuild_hot_props();
    parsed_word_hash = word_state_hash(0);
    parsed_word_valid = 1;
    dbg_stage = 41;
}
static unsigned char do_transforms(void) { SMS_mapROMBank(GD_MOVE_BANK); return do_transforms_b(); }
static void do_dir_rules(void) { SMS_mapROMBank(GD_MOVE_BANK); do_dir_rules_b(); }
static void do_movement(unsigned char a, unsigned char b) { SMS_mapROMBank(GD_MOVE_BANK); do_movement_b(a, b); }
static unsigned char end_of_turn(void) { SMS_mapROMBank(GD_MOVE_BANK); return end_of_turn_b(); }

unsigned char do_turn(unsigned char dir1, unsigned char dir2) {
    unsigned char result, iter;
    unsigned int i;
    turn_no++;
    turn_waited = (dir1 == 0 && dir2 == 0);
    undo_prop_count = 0;
    /* units spawned last turn are no longer new */
    for (i = 0; i < nunits; i++) u_dir[i] &= ~U_NEW;
    /* A full reparse is ~900k cycles; skip it when the parser's inputs
       are exactly the state the last parse already consumed. */
    {
        unsigned long initial_hash = 0;
        unsigned char initial_current = 0;
        if (!parsed_word_valid) {
            parse_rules();
        } else {
            initial_hash = word_state_hash(1);
            if (initial_hash != parsed_word_hash) parse_rules();
            else initial_current = 1;
        }
        do_dir_rules();
        iter = 0;
        while (iter < 8 && do_transforms()) { parse_rules(); do_dir_rules(); iter++; }
        if (iter >= 8) return 5; /* infloop */
        /* The initial hash is still the pre-movement hash when no
           parse, direction rule or transform could have changed a
           parser input since it was taken. */
        {
        unsigned long before_move = (initial_current && !any_dir_work && iter == 0)
                                  ? initial_hash : word_state_hash(1);
        do_movement(dir1, dir2);
        if (word_state_hash(1) != before_move) parse_rules();
        }
    }
    do_dir_rules();
    iter = 0;
    while (iter < 4 && do_transforms()) { parse_rules(); iter++; }
    if (iter >= 4) return 5;
    result = end_of_turn();
    build_rows();
    return result;
}

unsigned int find_lvl_entrance(void) {
    unsigned int i, j;
    unsigned char s;
    /* A successful result must come from an entrance special, so
       ordinary levels with none can skip the full unit/cell scan. */
    if (!spec_count) return 0xFFFF;
    for (i = 0; i < nunits; i++) {
        if (u_cls[i] == DEAD_CLS) continue;
        if (!has_prop_u(i, PROP_curse) && classes[u_cls[i]].tile != TILE_selctr) continue;
        for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
            if (classes[u_cls[j]].tile != TILE_lvl) continue;
            for (s = 0; s < spec_count; s++)
                if (spec_unit[s] == j && spec_target[s] != 0xFFFF) return spec_target[s];
        }
    }
    return 0xFFFF;
}
