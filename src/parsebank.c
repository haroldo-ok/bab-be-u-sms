/* Rule parser - lives in ROM bank GD_PARSER_BANK (see Makefile --codeseg). */
#include "engine.h"
#include <string.h>

/* ---------------- parser ---------------- */

typedef struct {
    unsigned char cls;
    unsigned int unit;
    unsigned int types;
    unsigned int ref;
    unsigned char prop, cond, nt, dir;
} Word;

#define MAXWORDS 96
#define words ((Word *)scratch)
static unsigned char nwords;
/* Word is 12 bytes in SDCC (2+2+2+2+1+1+1+1 packed by member order below) */

typedef struct {
    unsigned int noun;
    unsigned char nt;
    unsigned char nconds;
    unsigned char cond[3];
    unsigned int cnoun[3];
    unsigned char cnt[3];
    unsigned char cextra[3];
} PNoun;

static unsigned char parse_mode;  /* 0 groups only, 1 full */

static void letter_piece(unsigned int unit, char *dst) {
    unsigned int tile = classes[u_cls[unit]].tile;
    unsigned char dir = u_dir[unit] & DIR_MASK;
    unsigned char i;
    dst[0] = 0;
    for (i = 0; i < custom_count; i++)
        if (custom_unit[i] == unit) { strcpy(dst, custom_str[i]); return; }
    if (tile == TILE_letter_colon) { strcpy(dst, dir == 3 ? ".." : ":"); return; }
    if (tile == TILE_letter_parenthesis) { strcpy(dst, (dir >= 5 && dir <= 7) ? ")" : "("); return; }
    if (tile == TILE_letter_h && (dir == 3 || dir == 7)) { strcpy(dst, "i"); return; }
    if (tile == TILE_letter_i && (dir == 3 || dir == 7)) { strcpy(dst, "h"); return; }
    if (tile == TILE_letter_n && (dir == 3 || dir == 7)) { strcpy(dst, "z"); return; }
#ifdef TILE_letter_z
    if (tile == TILE_letter_z && (dir == 3 || dir == 7)) { strcpy(dst, "n"); return; }
#endif
    if (tile == TILE_letter_m && dir == 5) { strcpy(dst, "w"); return; }
    if (tile == TILE_letter_w && dir == 5) { strcpy(dst, "m"); return; }
    if (tile == TILE_letter_6 && dir == 5) { strcpy(dst, "9"); return; }
    if (tile == TILE_letter_9 && dir == 5) { strcpy(dst, "6"); return; }
#ifdef TILE_letter_no
    if (tile == TILE_letter_no && dir == 5) { strcpy(dst, "on"); return; }
#endif
    if (tile == TILE_letter_9 && dir == 5) { strcpy(dst, "6"); return; }
    {
        unsigned int ref = classes[u_cls[unit]].ref;
        if (ref < NUM_LETTER_PIECES) strcpy(dst, letter_pieces[ref]);
    }
}

/* Build Word list from a raw consecutive run of text units. */
static void build_words(const unsigned int *units, unsigned char nunits) {
    unsigned char i = 0;
    nwords = 0;
    while (i < nunits && nwords < MAXWORDS) {
        Word *w = &words[nwords];
        ClassInfo *c = &classes[u_cls[units[i]]];
        unsigned int tile = c->tile;
        if (c->types & TY_LETTER) {
            /* gather letter run and segment with the spelling dictionary */
            unsigned char run_end = i;
            char buf[28];
            unsigned char plen[16];
            unsigned char npieces = 0, total = 0;
            unsigned char start_piece;
            while (run_end < nunits && (classes[u_cls[units[run_end]]].types & TY_LETTER) && npieces < 16) {
                char piece[8];
                unsigned char l;
                letter_piece(units[run_end], piece);
                l = (unsigned char)strlen(piece);
                if (total + l >= 27) break;
                strcpy(buf + total, piece);
                total += l;
                plen[npieces++] = l;
                run_end++;
            }
            start_piece = 0;
            {
                unsigned char charpos = 0;
                unsigned char piece_i = 0;
                while (piece_i < npieces && nwords < MAXWORDS) {
                    unsigned char kind = 0xFF;
                    unsigned int ref = 0xFFFF;
                    unsigned char take_pieces = 0, take_len = 0;
                    unsigned char p2, l2 = 0;
                    /* longest match by pieces */
                    for (p2 = piece_i; p2 < npieces; p2++) {
                        unsigned char k2; unsigned int r2;
                        l2 += plen[p2];
                        {
                            char saved = buf[charpos + l2];
                            unsigned char found;
                            buf[charpos + l2] = 0;
                            found = dict_lookup(buf + charpos, l2, &k2, &r2);
                            buf[charpos + l2] = saved;
                            if (found) { kind = k2; ref = r2; take_pieces = p2 - piece_i + 1; take_len = l2; }
                        }
                    }
                    w = &words[nwords];
                    w->unit = units[i + piece_i];
                    w->cls = u_cls[w->unit];
                    w->nt = 0; w->prop = 0xFF; w->cond = 0xFF;
                    w->dir = u_dir[w->unit] & DIR_MASK;
                    if (take_pieces) {
                        switch (kind) {
                            case 0: w->types = TY_OBJECT; w->ref = ref; break;
                            case 1: w->types = TY_PROPERTY; w->prop = (unsigned char)ref; w->ref = 0xFFFF; break;
                            case 2: w->types = TY_VERB; w->ref = ref; break;
                            case 3: w->types = cond_is_infix((unsigned char)ref) ? TY_COND_INFIX : TY_COND_PREFIX; w->cond = (unsigned char)ref; w->ref = ref; break;
                            case 4: w->types = TY_AND; break;
                            case 5: w->types = TY_NOT; break;
                            default: w->types = TY_ELLIPSIS; break;
                        }
                        /* spelled direction property keeps its prop id */
                        nwords++;
                        piece_i += take_pieces; charpos += take_len;
                    } else {
                        w->types = 0; w->ref = 0xFFFF;
                        nwords++;
                        charpos += plen[piece_i]; piece_i++;
                    }
                    (void)start_piece;
                }
            }
            i = run_end;
            continue;
        }
        w->cls = u_cls[units[i]];
        w->unit = units[i];
        w->types = c->types;
        w->ref = c->ref;
        w->prop = c->prop_id;
        w->cond = c->cond_id;
        w->nt = (c->flags & TF_NT) ? 1 : 0;
        w->dir = u_dir[units[i]] & DIR_MASK;
        if (tile == TILE_txt_direction) {
            w->types = TY_PROPERTY;
            w->prop = dir_prop_of(w->dir);
        } else if (tile == TILE_txt_spin) {
            w->types = TY_PROPERTY;
            w->prop = PROP_spin;
        }
        /* wurd objects: word data comes from their txt_ variant class */
        if (!(c->flags & TF_TEXT) && c->rel_cls >= 0) {
            ClassInfo *tc = &classes[c->rel_cls];
            w->types = tc->types;
            w->ref = tc->ref;
            w->prop = tc->prop_id;
            w->cond = tc->cond_id;
            w->nt = (tc->flags & TF_NT) ? 1 : 0;
        }
        nwords++;
        i++;
    }
}

static unsigned char word_nt_at(unsigned char idx, unsigned char *consumed_not) {
    *consumed_not = 0;
    if (idx < nwords && words[idx].nt) return 1;
    return 0;
}
/* after a word at pos, a following NOT word also negates */
static unsigned char take_nt(unsigned char *pos) {
    unsigned char nt = 0;
    if (*pos > 0 && words[*pos - 1].nt) nt = 1;
    while (*pos < nwords && (words[*pos].types & TY_NOT)) { nt ^= 1; (*pos)++; }
    return nt;
}

static signed int parse_pnoun(unsigned char *pos_io, PNoun *out) {
    unsigned char pos = *pos_io;
    unsigned char nt;
    memset(out, 0, sizeof(PNoun));
    /* prefix conditions */
    for (;;) {
        Word *w;
        if (pos >= nwords) return -1;
        w = &words[pos];
        if ((w->types & TY_COND_PREFIX) && w->cond != 0xFF &&
            (!(w->types & TY_OBJECT) || w->cond == COND_the)) {
            pos++;
            nt = take_nt(&pos);
            if (out->nconds < 3) {
                unsigned char ci = out->nconds++;
                out->cond[ci] = w->cond;
                out->cnoun[ci] = 0xFFFF;
                out->cnt[ci] = nt;
                if (cond_reverse_tab[w->cond] && nt) { out->cnt[ci] = 1; }
                else if (nt) {
                    signed char ac = anti_cond_tab[w->cond];
                    if (ac >= 0) { out->cond[ci] = (unsigned char)ac; out->cnt[ci] = 0; }
                }
            }
            continue;
        }
        break;
    }
    if (pos >= nwords) return -1;
    {
        Word *w = &words[pos];
        if (!(w->types & TY_OBJECT)) return -1;
        out->noun = w->ref;
        pos++;
        out->nt = take_nt(&pos);
    }
    out->nt = out->nt; /* head word nt flag handled by take_nt via previous word */
    words[pos - 1].nt = words[pos - 1].nt;
    /* gang: "<noun> txt" -> text of noun */
    while (pos < nwords && (words[pos].types & TY_OBJECT) && words[pos].ref == NOUN_TXT) {
        if (out->noun < 0xFF00) {
            unsigned int txt_tile = global_rel_tile(out->noun);
            if (txt_tile != 0xFFFF) out->noun = txt_tile;
        }
        pos++;
        if (take_nt(&pos)) { /* txtn't: all txt except this one -> approximate as nt noun */ }
    }
    /* infix conditions */
    for (;;) {
        Word *w;
        unsigned char cond;
        if (pos >= nwords) break;
        w = &words[pos];
        if (!((w->types & TY_COND_INFIX) && w->cond != 0xFF)) break;
        cond = w->cond;
        pos++;
        nt = take_nt(&pos);
        if (cond == COND_thatbe || cond == COND_that) {
            /* optional verb, then a property */
            if (pos < nwords && (words[pos].types & TY_VERB)) { pos++; take_nt(&pos); }
            if (pos < nwords && (words[pos].types & TY_PROPERTY) && words[pos].prop != 0xFF) {
                if (out->nconds < 3) {
                    unsigned char ci = out->nconds++;
                    out->cond[ci] = cond;
                    out->cextra[ci] = words[pos].prop;
                    out->cnt[ci] = nt;
                }
                pos++;
                take_nt(&pos);
                continue;
            }
            return -1;
        }
        if (pos >= nwords || !(words[pos].types & TY_OBJECT)) return -1;
        if (out->nconds < 3) {
            unsigned char ci = out->nconds++;
            out->cond[ci] = cond;
            out->cnoun[ci] = words[pos].ref;
            out->cnt[ci] = nt;
            if (nt && !cond_reverse_tab[cond]) {
                signed char ac = anti_cond_tab[cond];
                if (ac >= 0) { out->cond[ci] = (unsigned char)ac; out->cnt[ci] = 0; }
            }
        }
        pos++;
        {
            unsigned char n2 = take_nt(&pos);
            if (n2 && out->nconds) out->cnoun[out->nconds - 1] = out->cnoun[out->nconds - 1]; /* noun nt: rare, ignored */
        }
    }
    *pos_io = pos;
    return 0;
}

static void apply_rule(PNoun *subj, unsigned char verb, unsigned char vnt,
                       unsigned char objkind, unsigned int obj, unsigned char ont,
                       unsigned char extra, unsigned int wunit);

static signed int parse_from(unsigned char start) {
    PNoun subjects[3];
    unsigned char nsubj = 0;
    unsigned char pos = start;
    unsigned char phrases = 0;
    for (;;) {
        if (nsubj >= 3) return -1;
        if (parse_pnoun(&pos, &subjects[nsubj])) return -1;
        subjects[nsubj].nt = words[start].nt ? subjects[nsubj].nt : subjects[nsubj].nt;
        nsubj++;
        if (pos < nwords && (words[pos].types & TY_AND) &&
            pos + 1 < nwords && ((words[pos + 1].types & TY_OBJECT) || (words[pos + 1].types & TY_COND_PREFIX))) {
            pos++;
            continue;
        }
        break;
    }
    if (!nsubj) return -1;
    for (;;) {
        Word *wv;
        unsigned char verb, vnt;
        unsigned int objs[6];
        unsigned char okind[6], ont[6], oextra[6];
        unsigned int owunit[6];
        unsigned char nobj = 0;
        if (pos >= nwords || !(words[pos].types & TY_VERB)) return -1;
        wv = &words[pos];
        verb = (unsigned char)wv->ref;
        if (verb >= NUM_VERBS) return -1;
        pos++;
        vnt = take_nt(&pos) | wv->nt;
        for (;;) {
            Word *w;
            if (pos >= nwords) return -1;
            w = &words[pos];
            if (nobj >= 6) return -1;
            owunit[nobj] = w->unit;
            oextra[nobj] = w->dir;
            if (verb == VERB_be || verb == VERB_giv || verb == VERB_wont) {
                if ((w->types & TY_PROPERTY) && w->prop != 0xFF && !(w->types & TY_OBJECT)) {
                    okind[nobj] = 1; objs[nobj] = w->prop;
                    pos++; ont[nobj] = take_nt(&pos) | w->nt;
                } else if (w->types & TY_OBJECT) {
                    PNoun on;
                    unsigned char save = pos;
                    if (verb == VERB_be) {
                        if (parse_pnoun(&pos, &on)) return -1;
                        okind[nobj] = 0; objs[nobj] = on.noun; ont[nobj] = on.nt;
                    } else { return -1; }
                    (void)save;
                } else if ((w->types & TY_PROPERTY) && w->prop != 0xFF) {
                    okind[nobj] = 1; objs[nobj] = w->prop;
                    pos++; ont[nobj] = take_nt(&pos) | w->nt;
                } else return -1;
            } else if (verb == VERB_sing) {
                unsigned int tile = classes[w->cls].tile;
                objs[nobj] = 0xFF10;
                if (tile == TILE_letter_a) objs[nobj] = 0xFF10;
                else if (tile == TILE_letter_b) objs[nobj] = 0xFF11;
                else if (tile == TILE_letter_c) objs[nobj] = 0xFF12;
                else if (tile == TILE_letter_d) objs[nobj] = 0xFF13;
                else if (tile == TILE_letter_e) objs[nobj] = 0xFF14;
                else if (tile == TILE_letter_f) objs[nobj] = 0xFF15;
                else if (tile == TILE_letter_g) objs[nobj] = 0xFF16;
                else return -1;
                okind[nobj] = 0; ont[nobj] = 0;
                pos++; take_nt(&pos);
            } else {
                PNoun on;
                if ((verb == VERB_moov || verb == VERB_yeet) &&
                    (w->types & TY_PROPERTY) && prop_dir_of(w->prop)) {
                    okind[nobj] = 0; objs[nobj] = 0xFF20; oextra[nobj] = prop_dir_of(w->prop);
                    ont[nobj] = 0; pos++; take_nt(&pos);
                } else {
                    if (parse_pnoun(&pos, &on)) return -1;
                    okind[nobj] = 0; objs[nobj] = on.noun; ont[nobj] = on.nt;
                }
            }
            nobj++;
            if (pos < nwords && (words[pos].types & TY_AND)) {
                /* & could start another complement or another verb phrase */
                if (pos + 1 < nwords && (words[pos + 1].types & TY_VERB)) break;
                pos++;
                continue;
            }
            break;
        }
        if (!nobj) return -1;
        {
            unsigned char si, oi;
            for (si = 0; si < nsubj; si++)
                for (oi = 0; oi < nobj; oi++)
                    apply_rule(&subjects[si], verb, vnt, okind[oi], objs[oi], ont[oi], oextra[oi], owunit[oi]);
        }
        phrases++;
        if (pos < nwords && (words[pos].types & TY_AND) && pos + 1 < nwords && (words[pos + 1].types & TY_VERB)) {
            pos++;
            if (phrases >= 3) break;
            continue;
        }
        break;
    }
    return pos;
}

static void apply_rule(PNoun *subj, unsigned char verb, unsigned char vnt,
                       unsigned char objkind, unsigned int obj, unsigned char ont,
                       unsigned char extra, unsigned int wunit) {
    unsigned char cls;
    /* anti-verb replacements (creat<->snacc, liek<->haet) */
    if (vnt && verb != VERB_be) {
        signed char av = anti_verb_tab[verb];
        if (av >= 0) { verb = (unsigned char)av; vnt = 0; ont = ont; }
    }
    /* group membership (also collected in pass 0) */
    if (verb == VERB_be && objkind == 0 && (obj == NOUN_FRENS || obj == NOUN_GROOP) && !vnt && !ont) {
        if (!subj->nconds) {
            for (cls = 0; cls < nclass; cls++) {
                unsigned char m = noun_matches_class(subj->noun, cls);
                if (subj->nt) m = !m;
                if (m) cls_group[cls] |= (obj == NOUN_FRENS) ? 1 : 2;
            }
        }
        return;
    }
    if (parse_mode == 0) return;
    /* conditional rules stored whole */
    if (subj->nconds) {
        if (ncondrules < MAXCONDRULES) {
            CondRule *cr = &condrules[ncondrules++];
            unsigned char k;
            cr->subj = subj->noun; cr->nconds = subj->nconds;
            for (k = 0; k < 3; k++) {
                cr->cond[k] = subj->cond[k]; cr->cnoun[k] = subj->cnoun[k];
                cr->cnt[k] = subj->cnt[k]; cr->cextra[k] = subj->cextra[k];
            }
            cr->verb = verb; cr->obj = obj; cr->extra = extra;
            cr->flags = (subj->nt ? CF_SNT : 0) | (vnt ? CF_VNT : 0) | (ont ? CF_ONT : 0) | (objkind ? CF_OBJKIND : 0);
            any_condrules = 1;
        }
        return;
    }
    for (cls = 0; cls < nclass; cls++) {
        unsigned char m = noun_matches_class(subj->noun, cls);
        if (subj->nt) m = !m;
        if (!m) continue;
        if (verb == VERB_be) {
            if (objkind == 1) {
                add_prop_ex(cls, (unsigned char)obj, (vnt || ont), extra);
                if (obj == PROP_her || obj == PROP_thr || obj == PROP_rithere) {
                    her_wx[cls] = u_x[wunit]; her_wy[cls] = u_y[wunit];
                    her_wdir[cls] = u_dir[wunit] & DIR_MASK;
                }
            } else {
                signed char tgt = -1;
                if (obj == NOUN_TXT) tgt = classes[cls].rel_cls;
                else if (obj == NOUN_THEMSELF) tgt = (signed char)cls;
                else if (obj < 0xFF00) tgt = local_class_of_tile(obj);
                if (classes[cls].tile == TILE_no1 && !vnt && !ont) {
                    if (tgt >= 0 && empty_trans_n < 4) empty_trans[empty_trans_n++] = tgt;
                    continue;
                }
                if (vnt || ont) {
                    if (tgt == (signed char)cls) cls_notransform[cls] |= 2;
                    else if (tgt >= 0 && cls_transform[cls] == tgt) cls_transform[cls] = -1;
                } else {
                    if (tgt == (signed char)cls) cls_notransform[cls] |= 1;
                    else if (tgt >= 0) cls_transform[cls] = tgt;
                }
            }
        } else if (verb == VERB_wont) {
            if (objkind == 1) {
                if (vnt) add_prop_ex(cls, (unsigned char)obj, 0, extra);  /* wo = wont n't */
                else block_prop(cls, (unsigned char)obj);
            }
        } else if (verb == VERB_giv) {
            if (objkind == 1) add_rel(cls, VERB_giv, obj, ont);
        } else {
            if (obj == 0xFF20) obj = 0xFF20 + (extra ? extra : 1);  /* encode dir in sentinel */
            add_rel(cls, vnt ? (verb | 0x80) : verb, obj, extra);
        }
    }
}

static void scan_sentences(void) {
    /* collect word-eligible units, then scan rows and columns */
    unsigned char *tlist = scratch + 1056; /* overlay above words[] (unit ids < 255) */
    unsigned int nt = 0;
    unsigned int i;
    unsigned char axis;
    for (i = 0; i < nunits; i++) {
        unsigned char c;
        if (u_cls[i] == DEAD_CLS) continue;
        c = u_cls[i];
        if ((classes[c].flags & TF_TEXT) || cls_wurd[c] || classes[c].tile == TILE_this) {
            if (nt < 256) tlist[nt++] = i;
        }
    }
    for (axis = 0; axis < 2; axis++) {
        unsigned int line, maxline = axis ? bw : bh;
        for (line = 0; line < maxline; line++) {
            unsigned int *run = (unsigned int *)(scratch + 1312); /* overlay */
            unsigned int rn = 0;
            unsigned int a, b;
            /* gather units on this line sorted by the other coord */
            for (a = 0; a < nt; a++) {
                unsigned int u = tlist[a];
                unsigned char c1 = axis ? u_x[u] : u_y[u];
                signed int c2;
                unsigned int pos;
                if (c1 != line) continue;
                c2 = axis ? u_y[u] : u_x[u];
                pos = rn;
                while (pos > 0 && ((signed int)(axis ? u_y[run[pos - 1]] : u_x[run[pos - 1]]) > c2 ||
                       ((signed int)(axis ? u_y[run[pos - 1]] : u_x[run[pos - 1]]) == c2 && classes[u_cls[run[pos - 1]]].layer > classes[u_cls[u]].layer))) {
                    if (pos < 128) { run[pos] = run[pos - 1]; }
                    pos--;
                }
                if (pos < 128 && rn < 128) { run[pos] = u; rn++; }
            }
            /* split into consecutive runs, one unit per cell (topmost) */
            a = 0;
            while (a < rn) {
                unsigned int seg = a;
                unsigned char sn = 0;
                signed int expect = axis ? u_y[run[a]] : u_x[run[a]];
                while (a < rn) {
                    unsigned int best = run[a];
                    signed int co = axis ? u_y[run[a]] : u_x[run[a]];
                    if (co != expect) break;
                    /* among stacked text in this cell keep the last (topmost by layer order) */
                    while (a < rn && (signed int)(axis ? u_y[run[a]] : u_x[run[a]]) == co) { best = run[a]; a++; }
                    if (sn < 128) run[seg + sn++] = best;  /* compact in place (seg+sn <= a) */
                    expect = co + 1;
                }
                if (sn >= 3 || (sn && sn < 3 && 0)) {
                    unsigned char s;
                    build_words(run + seg, sn);
                    for (s = 0; s < nwords; s++) {
                        signed int r = parse_from(s);
                        if (r > s) {
                            unsigned char k;
                            for (k = s; k < (unsigned char)r && k < nwords; k++)
                                u_dir[words[k].unit] |= U_ACTIVE;
                        }
                    }
                }
                (void)b;
            }
        }
    }
}

void parse_rules_b(void) {
    unsigned char c, i;
    unsigned int u;
    /* snapshot wurd-ness from the previous ruleset (original does the same) */
    for (c = 0; c < nclass; c++) {
        unsigned char w = (classes[c].flags & TF_TEXT) || classes[c].tile == TILE_this;
        if (!w && has_prop_c(c, PROP_wurd) && !has_anti_c(c, PROP_wurd)) w = 1;
        if (w && find_prop(c, PROP_wurd, 2) != 0xFF) w = 0;
        cls_wurd[c] = w;
    }
    for (u = 0; u < nunits; u++) u_dir[u] &= ~U_ACTIVE;
    for (c = 0; c < MAXCLASS; c++) {
        cls_nprops[c] = 0; cls_nrel[c] = 0;
        cls_transform[c] = -1; cls_notransform[c] = 0; cls_group[c] = 0;
    }
    ncondrules = 0; any_giv = 0; any_condrules = 0; empty_trans_n = 0;
    parse_mode = 0;
    scan_sentences();          /* pass 0: groups (FRENS/GROOP) */
    for (u = 0; u < nunits; u++) u_dir[u] &= ~U_ACTIVE;
    /* base rules (game/rules.lua clearRules) */
    {
        unsigned char has_selctr = 0;
        for (c = 0; c < nclass; c++) {
            unsigned int t = classes[c].tile;
            if (classes[c].flags & TF_TEXT) add_prop0(c, PROP_goawaypls);
            if (t == TILE_lvl) add_prop0(c, PROP_nogo);
            if (t == TILE_bordr) {
                add_prop0(c, PROP_nogo);
                add_prop0(c, PROP_tall);
                add_prop_ex(c, PROP_tall, 1, 0);
            }
            if (t == TILE_this) add_prop0(c, PROP_goawaypls);
            if (t == TILE_selctr || t == TILE_lin) has_selctr = 1;
        }
        if (has_selctr) {
            signed char sc = local_class_of_tile(TILE_selctr);
            if (sc >= 0) { add_prop_ex((unsigned char)sc, PROP_curse, 0, 0); add_prop_ex((unsigned char)sc, PROP_shy, 0, 0); }
        }
    }
    parse_mode = 1;
    scan_sentences();
    /* resolve anti/block precedence + flye */
    for (c = 0; c < nclass; c++) {
        for (i = 0; i < cls_nprops[c]; i++) {
            if (cls_props[c][i] == PROP_tall || cls_props[c][i] == PROP_flye) {
                if (cls_props_cnt[c][i] == PCNT_BLOCK) {
                    unsigned char j;
                    for (j = 0; j < cls_nprops[c]; j++)
                        if (j != i && cls_props[c][j] == cls_props[c][i] && cls_props_cnt[c][j] < PCNT_ANTI)
                            cls_props_cnt[c][j] = 0;
                }
                continue;
            }
            if (cls_props_cnt[c][i] == PCNT_BLOCK || cls_props_cnt[c][i] == PCNT_ANTI) {
                unsigned char j;
                for (j = 0; j < cls_nprops[c]; j++)
                    if (j != i && cls_props[c][j] == cls_props[c][i] && cls_props_cnt[c][j] < PCNT_ANTI)
                        cls_props_cnt[c][j] = 0; /* zero-count: treat as absent in has via <ANTI check next */
            }
        }
        /* remove zero-count entries by compacting */
        {
            unsigned char w = 0, r;
            for (r = 0; r < cls_nprops[c]; r++) {
                if (cls_props_cnt[c][r] == 0) continue;
                if (w != r) {
                    cls_props[c][w] = cls_props[c][r];
                    cls_props_cnt[c][w] = cls_props_cnt[c][r];
                    cls_props_extra[c][w] = cls_props_extra[c][r];
                }
                w++;
            }
            cls_nprops[c] = w;
        }
        classes[c].flye = (signed char)count_prop_c(c, PROP_flye) - (has_anti_c(c, PROP_flye) ? 1 : 0);
        classes[c].flye_mode = 0;
        if (has_prop_c(c, PROP_tall)) classes[c].flye_mode |= 1;
        if (has_anti_c(c, PROP_tall)) classes[c].flye_mode |= 2;
        if ((cls_notransform[c] & 1) && !(cls_notransform[c] & 2)) cls_notransform[c] |= 4;
    }
    /* RP: copy target class properties */
    for (c = 0; c < nclass; c++) {
        unsigned char r;
        for (r = 0; r < cls_nrel[c]; r++) {
            signed char tc;
            unsigned char pi;
            if (cls_rel_verb[c][r] != VERB_rp) continue;
            if (cls_rel_tgt[c][r] >= 0xFF00) continue;
            tc = local_class_of_tile(cls_rel_tgt[c][r]);
            if (tc < 0 || tc == (signed char)c) continue;
            {
                unsigned char exists = 0;
                unsigned int uu;
                for (uu = 0; uu < nunits; uu++) if (u_cls[uu] == (unsigned char)tc) { exists = 1; break; }
                if (!exists) continue;
            }
            for (pi = 0; pi < cls_nprops[tc]; pi++) {
                if (cls_props_cnt[tc][pi] >= PCNT_ANTI) continue;
                if (find_prop(c, cls_props[tc][pi], 2) != 0xFF) continue;
                add_prop_ex(c, cls_props[tc][pi], 0, cls_props_extra[tc][pi]);
            }
        }
    }
}

