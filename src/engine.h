/* Bab Be U SMS - shared engine state and data-format definitions.
   Data blobs are produced by tools/convert.py from the upstream game. */
#ifndef ENGINE_H
#define ENGINE_H

#include "SMSlib.h"
#include "../gen/gamedata.h"

#define MAXUNITS 255
#define MAXCLASS 36
#define VIEW_W 16
#define VIEW_H 12
#define NIL_UNIT 255   /* unit indices are 0..254; 255 = none */
#define DEAD_CLS 0xFF

/* u_dir packing: bits0-3 = dirs8 (1..8), bit4 hidden lin, bit5 NEW,
   bit6 ACTIVE, bit7 MOVED */
#define DIR_MASK 0x0F
#define U_HIDDEN_PATH 0x10
#define U_NEW 0x20
#define U_ACTIVE 0x40
#define U_MOVED 0x80

/* tile-entry flag bits (converter) */
#define TF_TEXT 1
#define TF_ROTATE 2
#define TF_NT 4
#define TF_WOBBLE 8
#define TF_HASROT 16
#define TF_PORTAL 32
#define TF_HASDIM 64

/* tile-entry type mask bits */
#define TY_OBJECT 1
#define TY_PROPERTY 2
#define TY_VERB 4
#define TY_VERB_CLASS 8
#define TY_VERB_PROPERTY 16
#define TY_VERB_UNIT 32
#define TY_COND_PREFIX 64
#define TY_COND_INFIX 128
#define TY_AND 256
#define TY_NOT 512
#define TY_LETTER 1024
#define TY_ELLIPSIS 2048
#define TY_DIRECTION 4096
#define TY_GROUP 8192
#define TY_VERB_DIRECTION 16384

/* special noun codes */
#define NOUN_NONE 0xFFFF
#define NOUN_TXT 0xFF01
#define NOUN_EVERY1 0xFF02
#define NOUN_EVERY2 0xFF03
#define NOUN_FRENS 0xFF04
#define NOUN_GROOP 0xFF05
#define NOUN_THEMSELF 0xFF06
#define NOUN_MOUS 0xFF07
#define NOUN_LOCAL_FLAG 0x200   /* noun value | flag => local class index */

typedef struct {
    unsigned int tile;
    unsigned int types;
    unsigned int ref;          /* verb id / noun(global tile or special) / cond id */
    unsigned int sms_off;      /* offset of colour table within level blob */
    unsigned int vram_base;    /* rot = +4, dim = +4+(rotate?4:0) */
    unsigned int palbits;      /* quadrant palette bits: base0-3 rot4-7 dim8-11 */
    unsigned char layer, flags, nsrc;
    unsigned char mcoord;      /* main palette coord 0..34, 255 none */
    unsigned char prop_id;     /* ref2 low: property id or 0xFF */
    unsigned char cond_id;     /* ref2 high: cond id or 0xFF */
    signed char rel_cls;       /* local class of txt_ variant / base tile */
    signed char flye;          /* computed each parse */
    unsigned char flye_mode;   /* 0 normal 1 tall 2 antitall 3 both */
} ClassInfo;

extern ClassInfo classes[MAXCLASS];
extern unsigned char nclass;

extern unsigned char u_cls[MAXUNITS];
extern unsigned char u_x[MAXUNITS];
extern unsigned char u_y[MAXUNITS];
extern unsigned char u_dir[MAXUNITS];
extern unsigned char u_next[MAXUNITS];
extern unsigned char row_head[128];
extern unsigned int nunits;
extern unsigned char bw, bh;

/* per-class properties: parallel arrays, cls_nprops entries each.
   count: 1..250 = stacking count, 0xFE = anti, 0xFF = blocked(wont) */
#define MAXPROPS_PER_CLASS 6
extern unsigned char cls_props[MAXCLASS][MAXPROPS_PER_CLASS];
extern unsigned char cls_props_cnt[MAXCLASS][MAXPROPS_PER_CLASS];
extern unsigned char cls_props_extra[MAXCLASS][MAXPROPS_PER_CLASS];
extern unsigned char cls_nprops[MAXCLASS];
extern signed char cls_transform[MAXCLASS];      /* local class or -1 */
extern unsigned char cls_notransform[MAXCLASS];
extern unsigned char cls_group[MAXCLASS];        /* bit0 frens bit1 groop */
#define CLS_GROUP_EOT   0x04   /* class can do single-unit end-of-turn work */
#define CLS_GROUP_MOVER 0x08   /* class can be collected as a mover */
#define CLS_GROUP_P1DIAG 0x10  /* class has a diagonal-capable P1 control
                                  property (u/y'all/w/curse, not you-only) */

/* Hot class-property bits rebuilt after every parse.  They are exact
   class-level property presence; movement code uses them only when no
   conditional/GIV rule can grant or revoke a property per-unit. */
#define CHF_SLEP     0x01
#define CHF_ICY      0x02
#define CHF_WALK     0x04
#define CHF_GO       0x08
#define CHF_HAETSKYE 0x10
#define CHF_HAETFLOR 0x20
#define CHF_OUCH     0x40
#define CHF_NUEK     0x80
#define CHF2_MOAR       0x01
#define CHF2_SPLIT      0x02
#define CHF2_VISITFREN  0x04
#define CHF2_RITHERE    0x08
#define CHF2_HER        0x10
#define CHF2_THR        0x20
#define CHF2_P1         0x40   /* u/you/y'all/w/curse */
#define CHF2_P2         0x80   /* utoo/utres */
#define CHF3_GONE       0x01
#define CHF3_NOTRANFORM 0x02
#define CHF3_TXTIFY     0x04
#define CHF3_THINGIFY   0x08
#define CHF3_ISU        0x10   /* u/utoo/utres/you/y'all/w (not curse) */
#define CHF3_STELTH     0x20
#define CHF3_UNDO       0x40
#define CHF3_CURSE      0x80   /* curse (map-cursor movement uses RS_CURSE) */
#define CHA_U    0x01
#define CHA_WALK 0x02
#define CHA_WURD_NEXT 0x04   /* next parse's word eligibility (spare bit) */
extern unsigned char cls_hot1[MAXCLASS], cls_hot2[MAXCLASS], cls_hot3[MAXCLASS];
extern unsigned char cls_anti_hot[MAXCLASS];
extern unsigned char any_hot1, any_hot2, any_hot3;

/* Parse-time work summaries.  These let the movement bank skip whole
   scans when the current class-level rule set cannot produce that kind
   of work.  Property-derived gates are used only in the fast-props mode
   (no conditional/GIV rule can change a property per unit); relation
   gates describe cls_rel_* exactly, which is all the gated loops read.
   cls_dir_code packs do_dir_rules' per-class result:
     bits0-3 desired facing (0=none, 1..8), bits4-6 spin steps,
     bit7 = an anti-direction property is present, use the exact
     ordered fallback loop for this class. */
#define DIR_CODE_ANTI 0x80
extern unsigned char cls_dir_code[MAXCLASS];
extern unsigned char any_dir_work;
extern unsigned char any_transform_work;
extern unsigned char any_eot_pair;
extern unsigned char any_u_cell_effects;
extern unsigned char any_flye_complex;
extern unsigned char any_rel_stalkmoov;
extern unsigned char any_rel_spoop;
extern unsigned char any_rel_copkat;
extern unsigned char any_rel_yeet;
extern unsigned char any_rel_creat;
extern unsigned char any_rel_sing;
extern unsigned char any_rel_soko;
extern unsigned char any_diagortho;

/* verb relations per class: verb (with 0x80 = nt), target noun */
#define MAXREL_PER_CLASS 4
extern unsigned char cls_rel_verb[MAXCLASS][MAXREL_PER_CLASS];
extern unsigned int cls_rel_tgt[MAXCLASS][MAXREL_PER_CLASS];
extern unsigned char cls_nrel[MAXCLASS];

extern unsigned char level_bank;
extern unsigned int level_off;
extern unsigned char level_bg;
extern unsigned int cur_level;      /* global level id */
extern unsigned int parent_level, next_level;
extern unsigned char level_music;
extern unsigned int turn_no;
extern unsigned char turn_waited;
extern unsigned int rng_state;

/* paint overrides: unit -> palette coord (36 = none marker via count) */
#define MAXPAINT 16
extern unsigned char paint_unit[MAXPAINT];
extern unsigned char paint_coord[MAXPAINT];
extern unsigned char paint_count;

/* custom-letter specials */
#define MAXCUSTOM 6
extern unsigned char custom_unit[MAXCUSTOM];
extern char custom_str[MAXCUSTOM][7];
extern unsigned char custom_count;
extern unsigned char level_bonus_got;
extern unsigned char undo_prop_count;

/* lvl-entrance specials of current level (largest included map has 38) */
#define MAXSPECIALS 40
extern unsigned char spec_unit[MAXSPECIALS];
extern unsigned int spec_target[MAXSPECIALS];
extern unsigned int spec_shape[MAXSPECIALS]; /* lvl visibility/icon shape */
extern unsigned char spec_vis[MAXSPECIALS];
extern unsigned char spec_count;

/* scratch buffer shared by parser/movement/render (never simultaneously live):
   parser words[96]*11 at 0..1056, scan tlist u8 at 1056, run u16 at 1312;
   stalk BFS 21x21 dist at 0 + queue at 448; render bake uses 0..416 (+freq 512);
   draw_board cell keys/count/top/layer at 448..1408, class stealth at 1408 */
#define SCRATCH_SIZE 1600
extern unsigned char scratch[SCRATCH_SIZE];
extern unsigned char dbg_stage;

/* property count markers */
#define PCNT_ANTI 0xFE
#define PCNT_BLOCK 0xFF

/* conditional rules (written by banked parser, read by fixed has_prop_u) */
#define MAXCONDRULES 20
typedef struct {
    unsigned int subj;
    unsigned char nconds;
    unsigned char cond[3];
    unsigned int cnoun[3];
    unsigned char cnt[3];
    unsigned char cextra[3];
    unsigned char verb, flags;   /* CF_SNT=1 CF_VNT=2 CF_ONT=4 CF_OBJKIND=8 (objkind: 1=prop) */
    unsigned int obj;
    unsigned char extra;
} CondRule;
#define CF_SNT 1
#define CF_VNT 2
#define CF_ONT 4
#define CF_OBJKIND 8
extern CondRule condrules[MAXCONDRULES];
extern unsigned char ncondrules;
extern unsigned char cls_wurd[MAXCLASS];
extern unsigned char any_giv, any_condrules;
extern signed char empty_trans[4];
extern unsigned char empty_trans_n;
extern unsigned char her_wx[MAXCLASS], her_wy[MAXCLASS], her_wdir[MAXCLASS];
extern const signed char dx8[9];
extern const signed char dy8[9];

/* bankio.c */
unsigned char bread(unsigned char bank, unsigned int off);
unsigned int bread16(unsigned char bank, unsigned int off);
void bcopy(unsigned char bank, unsigned int off, unsigned char *dst, unsigned int len);
unsigned char sread(unsigned char base_bank, unsigned long idx);
unsigned int sread16(unsigned char base_bank, unsigned long idx);
void scopy(unsigned char base_bank, unsigned long idx, unsigned char *dst, unsigned int len);
void get_shape_ptr(unsigned int shape_id, unsigned char *bank, unsigned int *off);
void load_tile_entry(unsigned int tile_id, ClassInfo *c);
void load_tile_shapes(unsigned int tile_id, unsigned int sh[4]);
void get_level_dir(unsigned int menu_idx, unsigned char *entry13);
void read_name_str(unsigned int name_off, char *dst, unsigned char maxlen);
unsigned int rng_next(void);

/* render.c */
void render_init(void);
void load_font(void);
void print_str(unsigned char x, unsigned char y, const char *s);
void print_str_banked_name(unsigned char x, unsigned char y, unsigned int name_off);
void print_str2(unsigned char x, unsigned char y, const char *s, unsigned int extra);
unsigned char has_prop_c(unsigned char cls, unsigned char prop);
unsigned char has_prop_u(unsigned char unit, unsigned char prop);
void clear_screen(unsigned int tile);
void bake_level_tiles(void);
void draw_board(void);
void camera_update(void);
extern unsigned char cam_x, cam_y;
extern unsigned char need_full_redraw;
extern unsigned char board_dirty;
extern unsigned int vram_start;
void set_menu_palette(void);
void upload_wobble_frame(void);

/* engine.c */
void build_rows(void);
void parse_rules(void);
unsigned char do_turn(unsigned char dir1, unsigned char dir2); /* 0=none 1=win 2=delet 3=nxt 4=tryagain 5=infloop 6=undo */
unsigned int find_lvl_entrance(void);  /* level id under a curse unit or 0xFFFF */
unsigned char any_u_exists(void);
void engine_reset_level_state(void);

/* audio.c */
void audio_init(void);
void audio_frame(void);
void music_play(unsigned char music_id);
void music_stop(void);
void sfx_play(unsigned char sfx_id);
void audio_sing(unsigned int period);

/* shared engine helpers (fixed code, callable from banked code) */
unsigned char find_prop(unsigned char cls, unsigned char prop, unsigned char mode);
void block_prop(unsigned char cls, unsigned char prop);
void add_prop_ex(unsigned char cls, unsigned char prop, unsigned char anti, unsigned char extra);
void add_prop0(unsigned char cls, unsigned char prop);
unsigned char has_anti_c(unsigned char cls, unsigned char prop);
unsigned char count_prop_c(unsigned char cls, unsigned char prop);
unsigned char prop_extra_of(unsigned char cls, unsigned char prop);
void add_rel(unsigned char cls, unsigned char verb, unsigned int tgt, unsigned char dir);
unsigned char noun_matches_class(unsigned int noun, unsigned char cls);
unsigned char unit_matches_noun(unsigned int unit, unsigned int noun);
signed char local_class_of_tile(unsigned int tile);
unsigned int global_rel_tile(unsigned int tile);
unsigned char same_flye_u(unsigned int a, unsigned int b);
unsigned int cell_first(unsigned char x, unsigned char y);
unsigned int cell_next(unsigned int i, unsigned char x);
unsigned char eval_cond(unsigned int unit, unsigned char cond, unsigned int cnoun, unsigned char cextra, unsigned char depth);
unsigned char dict_lookup(const char *s, unsigned char len, unsigned char *kind, unsigned int *ref);
unsigned char dir_prop_of(unsigned char dir);
unsigned char prop_dir_of(unsigned char prop);
unsigned char cond_is_infix(unsigned char cond);
unsigned int spawn_unit(unsigned char cls, unsigned char x, unsigned char y, unsigned char dir);
void destroy_unit(unsigned int u);
void destroy_unit_raw(unsigned int u);
unsigned char unit_is_u(unsigned int u);
/* banked entry points (call only via wrappers in engine.c) */
void parse_rules_b(void);
unsigned char do_transforms_b(void);
void do_dir_rules_b(void);
void do_movement_b(unsigned char dir1, unsigned char dir2);
unsigned char end_of_turn_b(void);

/* main.c helpers */
void snapshot_push(void);
unsigned char snapshot_undo(void);
void load_level_by_id(unsigned int level_id, unsigned int from_id);

#endif
