/* Transforms + movement + end-of-turn - lives in ROM bank GD_MOVE_BANK. */
#include "engine.h"
#include <string.h>

/* ---------------- transforms ---------------- */

unsigned char do_transforms_b(void) {
    unsigned char changed = 0;
    unsigned int u;
    unsigned char r;
    unsigned char fastprops = !any_condrules && !any_giv;
    signed char no1_cls;
    /* No transform target, GONE/TXTIFY/THINGIFY or empty transform in
       the class-level rule set: the scan below cannot change anything.
       Conditional rules can grant those per unit, hence fastprops only. */
    if (fastprops && !any_transform_work) return 0;
    no1_cls = local_class_of_tile(TILE_no1);
    for (u = 0; u < nunits; u++) {
        unsigned char cls = u_cls[u];
        signed char tgt = -1;
        if (cls == DEAD_CLS) continue;
        if (fastprops ? (cls_hot3[cls] & CHF3_GONE) : has_prop_u((unsigned char)u, PROP_gone)) { destroy_unit_raw(u); changed = 1; continue; }
        if ((fastprops ? (cls_hot3[cls] & CHF3_NOTRANFORM) : has_prop_u((unsigned char)u, PROP_notranform)) || (cls_notransform[cls] & 4)) {
            /* still allow txtify/thingify? original: notranform negates tranz only; keep full block */
            continue;
        }
        tgt = cls_transform[cls];
        if (any_condrules) {
            for (r = 0; r < ncondrules; r++) {
                CondRule *cr = &condrules[r];
                unsigned char ok = 1, k;
                signed char t2 = -1;
                if (cr->verb != VERB_be || (cr->flags & (CF_OBJKIND | CF_VNT | CF_ONT))) continue;
                if ((cr->flags & CF_SNT) ? noun_matches_class(cr->subj, cls) : !noun_matches_class(cr->subj, cls)) continue;
                for (k = 0; k < cr->nconds && ok; k++) {
                    unsigned char res = eval_cond(u, cr->cond[k], cr->cnoun[k], cr->cextra[k], 0);
                    if (cr->cnt[k]) res = !res;
                    if (!res) ok = 0;
                }
                if (!ok) continue;
                if (cr->obj == NOUN_TXT) t2 = classes[cls].rel_cls;
                else if (cr->obj < 0xFF00) t2 = local_class_of_tile(cr->obj);
                if (t2 >= 0) tgt = t2;
            }
        }
        if ((fastprops ? (cls_hot3[cls] & CHF3_TXTIFY) : has_prop_u((unsigned char)u, PROP_txtify)) && classes[cls].rel_cls >= 0 && !(classes[cls].flags & TF_TEXT))
            tgt = classes[cls].rel_cls;
        if ((fastprops ? (cls_hot3[cls] & CHF3_THINGIFY) : has_prop_u((unsigned char)u, PROP_thingify)) && classes[cls].rel_cls >= 0 && (classes[cls].flags & TF_TEXT))
            tgt = classes[cls].rel_cls;
        if (tgt >= 0 && tgt != (signed char)cls) {
            if (tgt == no1_cls) { destroy_unit(u); }
            else u_cls[u] = (unsigned char)tgt;
            changed = 1;
        }
    }
    /* no1 (empty) transforms: spawn on empty cells */
    if (empty_trans_n) {
        unsigned char x, y, k, spawned = 0;
        for (y = 0; y < bh && spawned < 48; y++)
            for (x = 0; x < bw && spawned < 48; x++) {
                if (cell_first(x, y) == NIL_UNIT) {
                    for (k = 0; k < empty_trans_n; k++) {
                        if (spawn_unit((unsigned char)empty_trans[k], x, y, 1) != NIL_UNIT) spawned++;
                    }
                    changed = 1;
                }
            }
    }
    if (changed) build_rows();
    return changed;
}

/* ---------------- movement ---------------- */

static unsigned char dir_add(unsigned char dir, signed char steps) {
    signed int d = (dir - 1 + steps) & 7;
    return (unsigned char)(d + 1);
}
static void face_unit(unsigned int u, unsigned char dir) {
    if (has_prop_u(u, PROP_noturn)) return;
    u_dir[u] = (u_dir[u] & ~DIR_MASK) | (dir & DIR_MASK);
}

static unsigned char victim_of_nedkee(unsigned int mover, unsigned int other) {
    return (has_prop_u(mover, PROP_nedkee) && has_prop_u(other, PROP_fordor)) ||
           (has_prop_u(mover, PROP_fordor) && has_prop_u(other, PROP_nedkee));
}
static unsigned char rel_is(unsigned char cls, unsigned char verb, unsigned char other_cls) {
    unsigned char i;
    for (i = 0; i < cls_nrel[cls]; i++) {
        if (cls_rel_verb[cls][i] != verb) continue;
        if (noun_matches_class(cls_rel_tgt[cls][i], other_cls)) return 1;
    }
    return 0;
}
static unsigned char gomyway_blocks(unsigned int blocker, unsigned char movedir) {
    /* prevents entry from the front and front diagonals */
    unsigned char bd = u_dir[blocker] & DIR_MASK;
    unsigned char enter_from = dir_add(movedir, 4); /* direction from blocker to mover */
    return enter_from == bd || enter_from == dir_add(bd, 1) || enter_from == dir_add(bd, -1);
}

/* Set by execute_movers() for one top-level curse-reason mover.  Upstream
   canMove() gives curse movement its own destination/blocking rules
   (this is how the overworld selctr travels the lin/lvl path network). */
static unsigned char move_is_curse;

static unsigned char spec_vis_for_unit(unsigned int unit) {
    unsigned char s;
    for (s = 0; s < spec_count; s++)
        if (spec_unit[s] == unit) return spec_vis[s];
    return 0xFF;
}

static signed char try_move(unsigned int u, unsigned char dir, unsigned char depth);

static void compute_target(unsigned int u, unsigned char dir, signed int *nx, signed int *ny) {
    *nx = u_x[u] + dx8[dir];
    *ny = u_y[u] + dy8[dir];
    if (has_prop_u(u, PROP_goarnd)) {
        if (*nx < 0) *nx = bw - 1; else if (*nx >= bw) *nx = 0;
        if (*ny < 0) *ny = bh - 1; else if (*ny >= bh) *ny = 0;
    } else if (has_prop_u(u, PROP_mirrarnd)) {
        if (*nx < 0) { *nx = bw - 1; *ny = bh - 1 - *ny; }
        else if (*nx >= bw) { *nx = 0; *ny = bh - 1 - *ny; }
        if (*ny < 0) { *ny = bh - 1; *nx = bw - 1 - *nx; }
        else if (*ny >= bh) { *ny = 0; *nx = bw - 1 - *nx; }
    }
}

/* returns 1 moved, 0 blocked, -1 mover died (nedkee) */
static signed char try_move(unsigned int u, unsigned char dir, unsigned char depth) {
    signed int nx, ny;
    unsigned int i;
    unsigned char cls = u_cls[u];
    unsigned char liek_ok = 1;
    unsigned char is_curse = (move_is_curse && depth == 0 && has_prop_u(u, PROP_curse));
    unsigned char curse_ok = 0;
    unsigned char mover_nrel = cls_nrel[cls];
    if (depth > 24) return 0;
    if (has_prop_u(u, PROP_stukc)) return 0;
    compute_target(u, dir, &nx, &ny);
    if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) return 0;
    /* DIAG/ORTHO (upstream canMoveCore): a DIAG-only unit cannot make
       a cardinal step and an ORTHO-only unit cannot make a diagonal
       step, whether it is self-moving or being pushed.  Directions
       1,3,5,7 are cardinal; 2,4,6,8 are diagonal. */
    if (any_diagortho) {
        unsigned char is_diag = has_prop_u((unsigned char)u, PROP_diag);
        unsigned char is_ortho = has_prop_u((unsigned char)u, PROP_ortho);
        if (is_diag && !is_ortho && (dir & 1)) return 0;
        if (is_ortho && !is_diag && !(dir & 1)) return 0;
    }
    /* POOR TOLL: entering a portal cell exits the next same-class portal */
    {
        unsigned int portal = NIL_UNIT;
        for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx)) {
            if (i != u && has_prop_u(i, PROP_poortoll) && !(has_prop_u(u, PROP_thicc))) { portal = i; break; }
        }
        if (portal != NIL_UNIT && depth < 3) {
            unsigned int j, best = NIL_UNIT;
            unsigned int bestkey = 0xFFFFFFFFUL;
            unsigned int selfkey = (unsigned int)ny * 256 + nx;
            for (j = 0; j < nunits; j++) {
                unsigned int key;
                if (u_cls[j] != u_cls[portal] || j == portal) continue;
                key = (unsigned int)u_y[j] * 256 + u_x[j];
                if (key > selfkey && key < bestkey) { bestkey = key; best = j; }
            }
            if (best == NIL_UNIT) {
                for (j = 0; j < nunits; j++) {
                    unsigned int key;
                    if (u_cls[j] != u_cls[portal] || j == portal) continue;
                    key = (unsigned int)u_y[j] * 256 + u_x[j];
                    if (key < bestkey) { bestkey = key; best = j; }
                }
            }
            if (best != NIL_UNIT) {
                /* teleport: place mover on exit portal, then step out */
                u_x[u] = u_x[best]; u_y[u] = u_y[best];
                build_rows();
                return try_move(u, dir, depth + 2);
            }
        }
    }
    /* CURSE (upstream canMove reason "curse"): the destination must
       contain a path object -- an open lvl entrance or a lin (or, below,
       something the mover LIEKs).  A locked lvl refuses entry outright.
       Without this, lvl's base NOGO made every lvl cell except the one
       the selctr starts on impossible to (re-)enter. */
    if (is_curse) {
        for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx)) {
            unsigned char vis;
            if (i == u || !same_flye_u(i, u)) continue;
            vis = spec_vis_for_unit(i);
            if (vis != 0xFF) {
                if (classes[u_cls[i]].tile == TILE_lvl && vis == 1) return 0;
                if (vis == 0) curse_ok = 1;
            }
            if (classes[u_cls[i]].tile == TILE_lin) curse_ok = 1;
        }
    }
    /* LIEK: destination must contain something liked */
    if (mover_nrel) {
        unsigned char has_liek = 0, found = 0;
        unsigned char r;
        for (r = 0; r < mover_nrel; r++) {
            if (cls_rel_verb[cls][r] != VERB_liek) continue;
            has_liek = 1;
            for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx))
                if (noun_matches_class(cls_rel_tgt[cls][r], u_cls[i])) found = 1;
        }
        if (found) curse_ok = 1;
        if (has_liek && !found && !is_curse) return 0;
    }
    if (is_curse && !curse_ok) return 0;
    (void)liek_ok;
    /* BEHINU: swap with everything in target cell (same flye) */
    if (has_prop_u(u, PROP_behinu)) {
        unsigned char ox = u_x[u], oy = u_y[u];
        unsigned char swapped = 0;
        for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx)) {
            if (i == u || !same_flye_u(i, u)) continue;
            u_x[i] = ox; u_y[i] = oy; swapped = 1;
        }
        u_x[u] = (unsigned char)nx; u_y[u] = (unsigned char)ny;
        u_dir[u] |= U_MOVED;
        build_rows();
        (void)swapped;
        return 1;
    }
    /* blocker resolution loop (push chains restart the scan) */
    for (;;) {
        unsigned char pushed = 0;
        for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx)) {
            unsigned char bcls = u_cls[i];
            unsigned char b_nogo, b_goaway, b_moov;
            if (i == u || !same_flye_u(i, u)) continue;
            if (mover_nrel && rel_is(cls, VERB_ignor, bcls)) continue;
            if (victim_of_nedkee(u, i)) {
                destroy_unit(i);
                destroy_unit(u);
                build_rows();
                return -1;
            }
            if (mover_nrel && rel_is(cls, VERB_haet, bcls)) return 0;
            if (has_prop_u(i, PROP_gomyway) && gomyway_blocks(i, dir)) return 0;
            /* Cache the three blocker answers: the old form recomputed
               each of them up to three times for the same unit. */
            b_nogo = has_prop_u(i, PROP_nogo);
            b_goaway = has_prop_u(i, PROP_goawaypls);
            b_moov = mover_nrel && rel_is(cls, VERB_moov, bcls);
            if (!b_nogo && !b_goaway && !b_moov)
                continue;
            /* solid-ish: NO GO overrides GO AWAY PLS (but MOOV targets are
               not solid if they cannot be pushed, per upstream).  For a
               curse-reason mover, upstream suppresses the nogo/push-failure
               stop entirely (the eligibility check above already decided
               whether the cell may be entered). */
            if (b_nogo && !b_moov && !(is_curse && b_goaway)) {
                if (is_curse) continue;
                return 0;
            }
            if (b_goaway || b_moov) {
                signed char r;
                if (has_prop_u(i, PROP_shy)) {
                    if (b_moov && !b_nogo) continue;
                    if (is_curse) continue;
                    return 0;
                }
                r = try_move(i, dir, depth + 1);
                if (r == 1) { pushed = 1; break; }
                if (u_cls[i] == DEAD_CLS) { pushed = 1; break; }
                if (b_moov) continue;
                if (is_curse) continue;
                return 0;
            }
            if (!is_curse) return 0;
        }
        if (!pushed) break;
    }
    u_x[u] = (unsigned char)nx;
    u_y[u] = (unsigned char)ny;
    u_dir[u] |= U_MOVED;
    build_rows();
    return 1;
}

/* slide effects (icy / icyyyy / goooo / reflecc) after landing */
static void apply_slides(unsigned int u, unsigned char dir) {
    unsigned char guard = 0;
    while (guard++ < 48 && u_cls[u] != DEAD_CLS) {
        unsigned int i;
        unsigned char forced = 0;
        unsigned char fdir = dir;
        for (i = cell_first(u_x[u], u_y[u]); i != NIL_UNIT; i = cell_next(i, u_x[u])) {
            if (i == u || !same_flye_u(i, u)) continue;
            if (has_prop_u(i, PROP_icyyyy)) { forced = 1; fdir = dir; break; }
            if (has_prop_u(i, PROP_goooo)) { forced = 1; fdir = u_dir[i] & DIR_MASK; break; }
            if (has_prop_u(i, PROP_reflecc)) { forced = 1; fdir = dir_add(dir, 4); break; }
            if (has_prop_u(i, PROP_icy)) forced = 1;
        }
        if (!forced) break;
        if (try_move(u, fdir, 0) != 1) break;
        dir = fdir;
    }
}

static void pull_and_sidekik(unsigned int u, unsigned char ox, unsigned char oy, unsigned char dir) {
    unsigned int i;
    signed int bx, by;
    /* COME PLS: unit behind the vacated cell moves into it */
    bx = (signed int)ox - dx8[dir]; by = (signed int)oy - dy8[dir];
    if (bx >= 0 && by >= 0 && bx < bw && by < bh) {
        for (i = cell_first((unsigned char)bx, (unsigned char)by); i != NIL_UNIT; i = cell_next(i, (unsigned char)bx)) {
            if (i != u && same_flye_u(i, u) && has_prop_u(i, PROP_comepls) && !has_prop_u(i, PROP_shy)) {
                unsigned char px = u_x[i], py = u_y[i];
                if (try_move(i, dir, 0) == 1) pull_and_sidekik(i, px, py, dir);
                break;
            }
        }
    }
    /* SIDEKIK: neighbours perpendicular to the vacated cell copy the move;
       DIAGKIK: diagonal-behind neighbours copy it too */
    {
        unsigned char sides[4];
        unsigned char s;
        sides[0] = dir_add(dir, 2); sides[1] = dir_add(dir, -2);
        sides[2] = dir_add(dir, 3); sides[3] = dir_add(dir, -3);
        for (s = 0; s < 4; s++) {
            signed int sx = (signed int)ox + dx8[sides[s]];
            signed int sy = (signed int)oy + dy8[sides[s]];
            if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) continue;
            for (i = cell_first((unsigned char)sx, (unsigned char)sy); i != NIL_UNIT; i = cell_next(i, (unsigned char)sx)) {
                unsigned char want = (s < 2) ? PROP_sidekik : PROP_diagkik;
                if (i != u && same_flye_u(i, u) && has_prop_u(i, want) && !has_prop_u(i, PROP_shy)) {
                    unsigned char px = u_x[i], py = u_y[i];
                    if (try_move(i, dir, 0) == 1) pull_and_sidekik(i, px, py, dir);
                    break;
                }
            }
        }
    }
}

static signed char move_unit_full(unsigned int u, unsigned char dir, unsigned char is_mover) {
    unsigned char ox = u_x[u], oy = u_y[u];
    signed char r;
    /* glued groups move together */
    if (is_mover && has_prop_u(u, PROP_glued)) {
        unsigned int group[24];
        unsigned char gn = 0, gi;
        unsigned int stackq[24];
        unsigned char qh = 0, qt = 0;
        group[gn++] = u; stackq[qt++] = (unsigned char)u;
        while (qh < qt && gn < 24) {
            unsigned int cur = stackq[qh++];
            unsigned char d;
            for (d = 1; d <= 8; d += (d == 1 ? 2 : 1)) { /* 4-neighbours: 1,3,5,7 via loop below */
                if (d > 8) break;
            }
            for (d = 1; d <= 7; d += 2) {
                signed int nx = u_x[cur] + dx8[d], ny = u_y[cur] + dy8[d];
                unsigned int i;
                if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                for (i = cell_first((unsigned char)nx, (unsigned char)ny); i != NIL_UNIT; i = cell_next(i, (unsigned char)nx)) {
                    unsigned char seen = 0, k;
                    if (!has_prop_u(i, PROP_glued)) continue;
                    for (k = 0; k < gn; k++) if (group[k] == i) seen = 1;
                    if (!seen && gn < 24) { group[gn++] = i; stackq[qt++] = (unsigned char)i; }
                }
            }
        }
        if (gn > 1) {
            /* try to move every member; movers outside group get pushed */
            unsigned char ok = 1;
            /* first pass: attempt in an order that lets the front move first */
            for (gi = 0; gi < gn && ok; gi++) {
                /* skip if target cell is inside the group (it will move too) */
                signed char rr = try_move(group[gi], dir, 6);
                if (rr < 0) return rr;
                if (rr == 0) ok = 0;
            }
            return ok ? 1 : 0;
        }
    }
    r = try_move(u, dir, 0);
    if (r == 1) {
        pull_and_sidekik(u, ox, oy, dir);
        apply_slides(u, dir);
    }
    (void)is_mover;
    return r;
}

static unsigned char adjust_move_dir(unsigned int u, unsigned char dir) {
    unsigned char c;
    if (has_prop_u(u, PROP_munwalk)) dir = dir_add(dir, 4);
    c = count_prop_c(u_cls[u], PROP_diagstep);
    while (c--) dir = dir_add(dir, 1);
    c = count_prop_c(u_cls[u], PROP_sidestep);
    while (c--) dir = dir_add(dir, 2);
    /* DIAG/ORTHO restrictions are enforced in try_move(), after the
       munwalk/diagstep/sidestep adjustments above have produced the
       effective step direction. */
    return dir;
}

typedef struct { unsigned int unit; unsigned char dr; } Mover;  /* dr = dir | reason<<4 */
#define RS_U 1
#define RS_WALK 2
#define RS_SHIFT 3
#define RS_STALK 4
#define RS_SPOOP 5
#define RS_MOOVDIR 6
#define RS_COPKAT 7
#define RS_CURSE 8

static unsigned char cls_moved_mask[MAXCLASS];

static void execute_movers(Mover *mv, unsigned int n) {
    unsigned int k;
    for (k = 0; k < n; k++) {
        unsigned int u = mv[k].unit;
        unsigned char dir;
        signed char r;
        if (u >= nunits || u_cls[u] == DEAD_CLS) continue;
        if (has_prop_u(u, PROP_halfstep) && (turn_no & 1)) continue;
        dir = adjust_move_dir(u, (mv[k].dr & 15));
        move_is_curse = ((mv[k].dr >> 4) == RS_CURSE);
        /* knightstep / hopovr: two-step moves */
        face_unit(u, dir);
        if (has_prop_u(u, PROP_hopovr)) {
            signed int nx, ny;
            compute_target(u, dir, &nx, &ny);
            r = try_move(u, dir, 0);
            if (r == 0) {
                /* hop: temporarily test landing two cells away */
                unsigned char ox = u_x[u], oy = u_y[u];
                signed int lx = (signed int)ox + dx8[dir] * 2, ly = (signed int)oy + dy8[dir] * 2;
                if (lx >= 0 && ly >= 0 && lx < bw && ly < bh) {
                    u_x[u] = (unsigned char)(ox + dx8[dir]); u_y[u] = (unsigned char)(oy + dy8[dir]);
                    build_rows();
                    r = try_move(u, dir, 0);
                    if (r != 1) { u_x[u] = ox; u_y[u] = oy; build_rows(); r = 0; }
                }
            }
        } else {
            r = move_unit_full(u, dir, 1);
        }
        move_is_curse = 0;
        if (r == 1) {
            cls_moved_mask[u_cls[u]] |= (1 << ((dir - 1) & 7));
        } else {
            if (u_cls[u] != DEAD_CLS && has_prop_u(u, PROP_ouch) && (mv[k].dr >> 4) != RS_SPOOP) {
                destroy_unit(u);
                build_rows();
            } else if ((mv[k].dr >> 4) == RS_WALK && !has_prop_u(u, PROP_stubbn) && u_cls[u] != DEAD_CLS) {
                face_unit(u, dir_add(u_dir[u] & DIR_MASK, 4));
            }
        }
    }
}

static unsigned char stalk_dir(unsigned int u, unsigned int noun) {
    /* BFS distance field within a 21x21 window around the stalker */
    unsigned char *dist = scratch;                             /* 21*21 */
    unsigned int *queue = (unsigned int *)(scratch + 448);     /* 441*2 */
    signed int wx = (signed int)u_x[u] - 10, wy = (signed int)u_y[u] - 10;
    unsigned int qh = 0, qt = 0, i;
    unsigned char bestd = 0, bestdist = 255;
    signed int x, y;
    for (i = 0; i < 21 * 21; i++) dist[i] = 255;
    for (i = 0; i < nunits; i++) {
        signed int lx, ly;
        if (u_cls[i] == DEAD_CLS || !unit_matches_noun(i, noun)) continue;
        lx = (signed int)u_x[i] - wx; ly = (signed int)u_y[i] - wy;
        if (lx < 0 || ly < 0 || lx >= 21 || ly >= 21) continue;
        dist[ly * 21 + lx] = 0;
        queue[qt++] = (unsigned int)(ly * 21 + lx);
    }
    if (!qt) {
        /* greedy toward nearest target */
        signed int bd = 32000;
        signed char bdx = 0, bdy = 0;
        unsigned char d;
        for (i = 0; i < nunits; i++) {
            signed int ddx, ddy, dd;
            if (u_cls[i] == DEAD_CLS || !unit_matches_noun(i, noun)) continue;
            ddx = (signed int)u_x[i] - u_x[u]; ddy = (signed int)u_y[i] - u_y[u];
            dd = (ddx < 0 ? -ddx : ddx) + (ddy < 0 ? -ddy : ddy);
            if (dd < bd) { bd = dd; bdx = (signed char)(ddx > 0 ? 1 : ddx < 0 ? -1 : 0); bdy = (signed char)(ddy > 0 ? 1 : ddy < 0 ? -1 : 0); }
        }
        for (d = 1; d <= 8; d++) if (dx8[d] == bdx && dy8[d] == bdy) return d;
        return 0;
    }
    while (qh < qt) {
        unsigned int cur = queue[qh++];
        unsigned char cd = dist[cur];
        unsigned char d;
        x = (signed int)(cur % 21); y = (signed int)(cur / 21);
        for (d = 1; d <= 7; d += 2) {
            signed int nx = x + dx8[d], ny = y + dy8[d];
            unsigned int ni, j;
            unsigned char blocked = 0;
            if (nx < 0 || ny < 0 || nx >= 21 || ny >= 21) continue;
            ni = (unsigned int)(ny * 21 + nx);
            if (dist[ni] != 255) continue;
            for (j = cell_first((unsigned char)(wx + nx), (unsigned char)(wy + ny)); j != NIL_UNIT; j = cell_next(j, (unsigned char)(wx + nx))) {
                if (same_flye_u(j, u) && has_prop_u(j, PROP_nogo)) blocked = 1;
            }
            if (wx + nx < 0 || wy + ny < 0 || wx + nx >= bw || wy + ny >= bh) blocked = 1;
            if (blocked) { dist[ni] = 254; continue; }
            dist[ni] = cd + 1;
            queue[qt++] = ni;
        }
    }
    {
        unsigned char d;
        for (d = 1; d <= 8; d++) {
            signed int lx = 10 + dx8[d], ly = 10 + dy8[d];
            unsigned char dd = dist[ly * 21 + lx];
            if (dd < bestdist) { bestdist = dd; bestd = d; }
        }
    }
    return bestdist == 255 ? 0 : bestd;
}

void do_movement_b(unsigned char dir1, unsigned char dir2) {
    static Mover mv[192];
    unsigned int nmv = 0;
    unsigned int i, j;
    unsigned char c;
    unsigned char fastprops = !any_condrules && !any_giv;
    for (c = 0; c < MAXCLASS; c++) cls_moved_mask[c] = 0;
    for (i = 0; i < nunits; i++) u_dir[i] &= ~U_MOVED;
    /* collect movers */
    for (i = 0; i < nunits && nmv < 192; i++) {
        unsigned char cls = u_cls[i];
        unsigned char h1;
        if (cls == DEAD_CLS) continue;
        if (fastprops && !(cls_group[cls] & CLS_GROUP_MOVER)) continue;
        h1 = fastprops ? cls_hot1[cls] : 0;
        if (fastprops ? ((h1 & CHF_SLEP) && !(h1 & CHF_ICY))
                      : (has_prop_u((unsigned char)i, PROP_slep) && !has_prop_u((unsigned char)i, PROP_icy))) {
            /* slep units cannot self-move */
        } else {
            /* player-controlled */
            unsigned char pdir = 0;
            if (fastprops) {
                unsigned char h2 = cls_hot2[cls];
                /* YOU is the original's orthogonal-only control
                   property: a diagonal P1 request moves a class only
                   if it also has u/y'all/w/curse (CLS_GROUP_P1DIAG). */
                if ((h2 & CHF2_P1) && dir1 &&
                    ((dir1 & 1) || (cls_group[cls] & CLS_GROUP_P1DIAG))) pdir = dir1;
                else if ((h2 & CHF2_P2) && dir2) pdir = dir2;
                if ((cls_anti_hot[cls] & CHA_U) && dir1) pdir = dir_add(dir1, 4);
            } else {
                unsigned char p1diag = has_prop_u((unsigned char)i, PROP_u) ||
                                       has_prop_u((unsigned char)i, PROP_y_all) ||
                                       has_prop_u((unsigned char)i, PROP_w) ||
                                       has_prop_u((unsigned char)i, PROP_curse);
                if ((p1diag || has_prop_u((unsigned char)i, PROP_you)) && dir1 &&
                    ((dir1 & 1) || p1diag)) pdir = dir1;
                else if ((has_prop_u((unsigned char)i, PROP_utoo) || has_prop_u((unsigned char)i, PROP_utres)) && dir2) pdir = dir2;
                if (has_anti_c(cls, PROP_u) && dir1) pdir = dir_add(dir1, 4);
            }
            if (pdir) {
                unsigned char reason = (fastprops ? (cls_hot3[cls] & CHF3_CURSE)
                                                  : has_prop_u((unsigned char)i, PROP_curse)) ? RS_CURSE : RS_U;
                mv[nmv].unit = i; mv[nmv].dr = (pdir) | ((reason) << 4); nmv++; continue;
            }
            if (fastprops ? (h1 & CHF_WALK) : has_prop_u((unsigned char)i, PROP_walk)) { mv[nmv].unit = i; mv[nmv].dr = (u_dir[i] & DIR_MASK) | ((RS_WALK) << 4); nmv++; continue; }
            if (fastprops ? (cls_anti_hot[cls] & CHA_WALK) : has_anti_c(cls, PROP_walk)) { mv[nmv].unit = i; mv[nmv].dr = (dir_add(u_dir[i] & DIR_MASK, 4)) | ((RS_WALK) << 4); nmv++; continue; }
        }
        /* stalk / directional MOOV */
        if (any_rel_stalkmoov && cls_nrel[cls]) {
            unsigned char r;
            for (r = 0; r < cls_nrel[cls]; r++) {
                if (cls_rel_verb[cls][r] == VERB_stalk && !(fastprops ? (h1 & CHF_SLEP) : has_prop_u((unsigned char)i, PROP_slep))) {
                    unsigned char d = stalk_dir(i, cls_rel_tgt[cls][r]);
                    if (d) { mv[nmv].unit = i; mv[nmv].dr = (d) | ((RS_STALK) << 4); nmv++; }
                    break;
                }
                if (cls_rel_verb[cls][r] == VERB_moov && cls_rel_tgt[cls][r] >= 0xFF21 && cls_rel_tgt[cls][r] <= 0xFF28) {
                    mv[nmv].unit = i; mv[nmv].dr = ((unsigned char)(cls_rel_tgt[cls][r] - 0xFF20)) | ((RS_MOOVDIR) << 4); nmv++;
                    break;
                }
            }
        }
    }
    /* GO (shift): units sharing a cell with a GO unit move in its facing */
    for (i = 0; i < nunits && nmv < 186; i++) {
        if (fastprops && !(any_hot1 & CHF_GO)) break;
        if (u_cls[i] == DEAD_CLS || !(fastprops ? (cls_hot1[u_cls[i]] & CHF_GO) : has_prop_u((unsigned char)i, PROP_go))) continue;
        for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT && nmv < 192; j = cell_next(j, u_x[i])) {
            if (j == i || !same_flye_u(j, i)) continue;
            mv[nmv].unit = j; mv[nmv].dr = (u_dir[i] & DIR_MASK) | ((RS_SHIFT) << 4); nmv++;
        }
    }
    /* SPOOP: adjacent targets flee */
    if (any_rel_spoop)
    for (i = 0; i < nunits && nmv < 186; i++) {
        unsigned char r;
        if (u_cls[i] == DEAD_CLS) continue;
        for (r = 0; r < cls_nrel[u_cls[i]]; r++) {
            unsigned char d;
            if (cls_rel_verb[u_cls[i]][r] != VERB_spoop) continue;
            for (d = 1; d <= 8; d++) {
                signed int nx = u_x[i] + dx8[d], ny = u_y[i] + dy8[d];
                if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                for (j = cell_first((unsigned char)nx, (unsigned char)ny); j != NIL_UNIT && nmv < 192; j = cell_next(j, (unsigned char)nx)) {
                    if (unit_matches_noun(j, cls_rel_tgt[u_cls[i]][r]) && !(fastprops ? (cls_hot1[u_cls[j]] & CHF_SLEP) : has_prop_u((unsigned char)j, PROP_slep))) {
                        mv[nmv].unit = j; mv[nmv].dr = (d) | ((RS_SPOOP) << 4); nmv++;
                    }
                }
            }
        }
    }
    execute_movers(mv, nmv);
    /* COPKAT phase: copy class movements */
    nmv = 0;
    if (any_rel_copkat)
    for (i = 0; i < nunits && nmv < 186; i++) {
        unsigned char r;
        if (u_cls[i] == DEAD_CLS || (fastprops ? (cls_hot1[u_cls[i]] & CHF_SLEP) : has_prop_u((unsigned char)i, PROP_slep))) continue;
        for (r = 0; r < cls_nrel[u_cls[i]]; r++) {
            unsigned char d;
            if (cls_rel_verb[u_cls[i]][r] != VERB_copkat) continue;
            for (c = 0; c < nclass; c++) {
                if (!noun_matches_class(cls_rel_tgt[u_cls[i]][r], c)) continue;
                for (d = 1; d <= 8; d++)
                    if ((cls_moved_mask[c] & (1 << (d - 1))) && nmv < 192) {
                        mv[nmv].unit = i; mv[nmv].dr = (d) | ((RS_COPKAT) << 4); nmv++;
                    }
            }
        }
    }
    if (nmv) execute_movers(mv, nmv);
    /* YEET: throw co-located targets in the yeeter's facing */
    if (any_rel_yeet)
    for (i = 0; i < nunits; i++) {
        unsigned char r;
        if (u_cls[i] == DEAD_CLS) continue;
        for (r = 0; r < cls_nrel[u_cls[i]]; r++) {
            if (cls_rel_verb[u_cls[i]][r] != VERB_yeet) continue;
            for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
                if (j != i && unit_matches_noun(j, cls_rel_tgt[u_cls[i]][r]) && same_flye_u(j, i)) {
                    unsigned char guard = 0;
                    unsigned char d = u_dir[i] & DIR_MASK;
                    unsigned int victim = j;
                    j = cell_next(j, u_x[i]); /* note: re-fetch after moves below */
                    while (guard++ < 64 && try_move(victim, d, 0) == 1) { }
                    build_rows();
                    break;
                }
            }
        }
    }
    /* HAET SKYE / HAET FLOR: falling */
    if (!fastprops || (any_hot1 & (CHF_HAETSKYE | CHF_HAETFLOR)))
    for (i = 0; i < nunits; i++) {
        unsigned char guard = 0;
        if (u_cls[i] == DEAD_CLS) continue;
        if (fastprops ? (cls_hot1[u_cls[i]] & CHF_HAETSKYE) : has_prop_u((unsigned char)i, PROP_haetskye)) {
            while (guard++ < 64 && try_move(i, 3, 0) == 1) { }
        }
        guard = 0;
        if (fastprops ? (cls_hot1[u_cls[i]] & CHF_HAETFLOR) : has_prop_u((unsigned char)i, PROP_haetflor)) {
            while (guard++ < 64 && try_move(i, 7, 0) == 1) { }
        }
    }
    build_rows();
}

/* ---------------- end of turn ---------------- */

unsigned char end_of_turn_b(void) {
    unsigned int i, j;
    unsigned char result = 0;
    unsigned char fastprops = !any_condrules && !any_giv;
    signed int win_score = 0;
    /* pair interactions on shared cells */
    for (i = 0; i < nunits; i++) {
        if (u_cls[i] == DEAD_CLS) continue;
        /* With no possible pair interaction anywhere, a class with no
           single-unit end-of-turn property/relation (and no U-cell
           effect to receive) cannot do anything in this loop. */
        if (fastprops && !any_eot_pair &&
            !(cls_group[u_cls[i]] & CLS_GROUP_EOT)) continue;
        /* OUCH sharing a cell */
        if (fastprops ? (cls_hot1[u_cls[i]] & CHF_OUCH) : has_prop_u((unsigned char)i, PROP_ouch)) {
            unsigned char sharing = 0;
            for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i]))
                if (j != i && same_flye_u(j, i)) sharing = 1;
            if (sharing) { destroy_unit(i); build_rows(); continue; }
        }
        /* Skip the whole shared-cell pair scan when no NEDKEE/FORDOR,
           NOSWIM, HOTTE/FRIDGD, SAD, SNACC, VS or PAINT rule exists.
           Conditional/GIV properties bypass the summary, hence !fastprops. */
        if (!fastprops || any_eot_pair)
        for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
            if (j == i || u_cls[j] == DEAD_CLS || !same_flye_u(j, i)) continue;
            if (victim_of_nedkee(i, j)) {
                destroy_unit(i); destroy_unit(j); build_rows();
                break;
            }
            /* NOSWIM: everything on the cell is destroyed */
            if (has_prop_u(i, PROP_noswim) || has_prop_u(j, PROP_noswim)) {
                unsigned int k;
                for (k = cell_first(u_x[i], u_y[i]); k != NIL_UNIT; k = cell_next(k, u_x[i]))
                    if (same_flye_u(k, i)) destroy_unit(k);
                build_rows();
                break;
            }
            /* HOTTE destroys FRIDGD */
            if (has_prop_u(i, PROP_hotte) && has_prop_u(j, PROP_fridgd)) destroy_unit(j);
            if (has_prop_u(j, PROP_hotte) && has_prop_u(i, PROP_fridgd)) { destroy_unit(i); build_rows(); break; }
            /* :( kills U */
            if (has_prop_u(i, PROP_SAD) && unit_is_u(j)) destroy_unit(j);
            if (has_prop_u(j, PROP_SAD) && unit_is_u(i)) { destroy_unit(i); build_rows(); break; }
            /* SNACC / VS */
            if (rel_is(u_cls[i], VERB_snacc, u_cls[j])) destroy_unit(j);
            if (rel_is(u_cls[j], VERB_snacc, u_cls[i])) { destroy_unit(i); build_rows(); break; }
            if (rel_is(u_cls[i], VERB_vs, u_cls[j])) {
                if (u_dir[i] & U_MOVED) destroy_unit(j);
                else if (u_dir[j] & U_MOVED) { destroy_unit(i); build_rows(); break; }
            }
            /* PAINT */
            if (rel_is(u_cls[i], VERB_paint, u_cls[j])) {
                unsigned char k, found = 0;
                for (k = 0; k < paint_count; k++) if (paint_unit[k] == j) { paint_coord[k] = classes[u_cls[i]].mcoord; found = 1; }
                if (!found && paint_count < MAXPAINT) {
                    paint_unit[paint_count] = j;
                    paint_coord[paint_count++] = classes[u_cls[i]].mcoord;
                }
            }
        }
        if (u_cls[i] == DEAD_CLS) continue;
        /* single-unit cell effects for U */
        if ((!fastprops || any_u_cell_effects) && unit_is_u(i)) {
            for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
                if (!same_flye_u(j, i)) continue;
                if (has_prop_u(j, PROP_WIN)) win_score++;
                if (has_prop_u(j, PROP_UNWIN)) win_score--;
                if (has_prop_u(j, PROP_BONUS) && j != i) {
                    level_bonus_got = 1;
                    destroy_unit_raw(j);
                    build_rows();
                }
                if (has_prop_u(j, PROP_delet) && j != i) result = 2;
                if (has_prop_u(j, PROP_nxt) && j != i) result = 3;
                if (has_prop_u(j, PROP_tryagain) && j != i) result = 4;
            }
            if (has_prop_u(i, PROP_WIN)) win_score++;
        }
        /* NUEK: expanding blast (radius grows with time) */
        if (fastprops ? (cls_hot1[u_cls[i]] & CHF_NUEK) : has_prop_u((unsigned char)i, PROP_nuek)) {
            {
                unsigned char rad = (unsigned char)((turn_no / 2) & 7);
                signed int ox, oy;
                for (oy = -(signed int)rad; oy <= (signed int)rad; oy++)
                    for (ox = -(signed int)rad; ox <= (signed int)rad; ox++) {
                        signed int nx = u_x[i] + ox, ny = u_y[i] + oy;
                        unsigned int k;
                        if ((!ox && !oy) || nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                        for (k = cell_first((unsigned char)nx, (unsigned char)ny); k != NIL_UNIT; k = cell_next(k, (unsigned char)nx))
                            destroy_unit(k);
                    }
                build_rows();
            }
        }
        /* CREAT / MOAR / SPLIT / VISITFREN / HER-family */
        {
            unsigned char r;
            if (any_rel_creat)
            for (r = 0; r < cls_nrel[u_cls[i]]; r++) {
                signed char tc = -1;
                if (u_dir[i] & U_NEW) continue;
                if (cls_rel_verb[u_cls[i]][r] != VERB_creat) continue;
                if (cls_rel_tgt[u_cls[i]][r] == NOUN_TXT) tc = classes[u_cls[i]].rel_cls;
                else if (cls_rel_tgt[u_cls[i]][r] < 0xFF00) tc = local_class_of_tile(cls_rel_tgt[u_cls[i]][r]);
                if (tc >= 0) spawn_unit((unsigned char)tc, u_x[i], u_y[i], u_dir[i] & DIR_MASK);
            }
            if (!(u_dir[i] & U_NEW) && (fastprops ? (cls_hot2[u_cls[i]] & CHF2_MOAR) : has_prop_u((unsigned char)i, PROP_moar))) {
                unsigned char d;
                for (d = 1; d <= 8; d++) {
                    signed int nx = u_x[i] + dx8[d], ny = u_y[i] + dy8[d];
                    unsigned char free_cell = 1;
                    unsigned int k;
                    if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                    for (k = cell_first((unsigned char)nx, (unsigned char)ny); k != NIL_UNIT; k = cell_next(k, (unsigned char)nx))
                        if (same_flye_u(k, i) && has_prop_u(k, PROP_nogo)) free_cell = 0;
                    if (free_cell) spawn_unit(u_cls[i], (unsigned char)nx, (unsigned char)ny, u_dir[i] & DIR_MASK);
                }
                build_rows();
            }
            if (!(u_dir[i] & U_NEW) && (fastprops ? (cls_hot2[u_cls[i]] & CHF2_SPLIT) : has_prop_u((unsigned char)i, PROP_split))) {
                unsigned char d = u_dir[i] & DIR_MASK;
                unsigned char ld = dir_add(d, 2), rd = dir_add(d, -2);
                for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
                    signed int ax, ay;
                    if (j == i || !same_flye_u(j, i)) continue;
                    ax = u_x[i] + dx8[ld]; ay = u_y[i] + dy8[ld];
                    if (ax >= 0 && ay >= 0 && ax < bw && ay < bh)
                        spawn_unit(u_cls[j], (unsigned char)ax, (unsigned char)ay, u_dir[j] & DIR_MASK);
                    ax = u_x[i] + dx8[rd]; ay = u_y[i] + dy8[rd];
                    if (ax >= 0 && ay >= 0 && ax < bw && ay < bh)
                        spawn_unit(u_cls[j], (unsigned char)ax, (unsigned char)ay, u_dir[j] & DIR_MASK);
                }
                build_rows();
            }
            if (fastprops ? (cls_hot2[u_cls[i]] & CHF2_VISITFREN) : has_prop_u((unsigned char)i, PROP_visitfren)) {
                unsigned int best = NIL_UNIT, bestkey = 0xFFFFFFFFUL;
                unsigned int selfkey = (unsigned int)u_y[i] * 256 + u_x[i];
                unsigned int k;
                for (k = 0; k < nunits; k++) {
                    unsigned int key;
                    if (u_cls[k] != u_cls[i] || k == i) continue;
                    key = (unsigned int)u_y[k] * 256 + u_x[k];
                    if (key > selfkey && key < bestkey) { bestkey = key; best = k; }
                }
                if (best != NIL_UNIT) {
                    for (j = cell_first(u_x[i], u_y[i]); j != NIL_UNIT; j = cell_next(j, u_x[i])) {
                        if (j != i) { u_x[j] = u_x[best]; u_y[j] = u_y[best]; }
                    }
                    build_rows();
                }
            }
            if (fastprops ? (cls_hot2[u_cls[i]] & CHF2_RITHERE) : has_prop_u((unsigned char)i, PROP_rithere)) {
                u_x[i] = her_wx[u_cls[i]]; u_y[i] = her_wy[u_cls[i]];
                build_rows();
            } else if (fastprops ? (cls_hot2[u_cls[i]] & CHF2_HER) : has_prop_u((unsigned char)i, PROP_her)) {
                signed int nx = her_wx[u_cls[i]] + dx8[her_wdir[u_cls[i]] ? her_wdir[u_cls[i]] : 1];
                signed int ny = her_wy[u_cls[i]] + dy8[her_wdir[u_cls[i]] ? her_wdir[u_cls[i]] : 1];
                if (nx >= 0 && ny >= 0 && nx < bw && ny < bh) { u_x[i] = (unsigned char)nx; u_y[i] = (unsigned char)ny; build_rows(); }
            } else if (fastprops ? (cls_hot2[u_cls[i]] & CHF2_THR) : has_prop_u((unsigned char)i, PROP_thr)) {
                unsigned char d = her_wdir[u_cls[i]] ? her_wdir[u_cls[i]] : 1;
                unsigned char guard = 0;
                while (guard++ < 96 && try_move(i, d, 0) == 1) { }
            }
            if (fastprops ? (cls_hot3[u_cls[i]] & CHF3_UNDO) : has_prop_u((unsigned char)i, PROP_undo)) undo_prop_count++;
            /* SING */
            if (any_rel_sing)
            for (r = 0; r < cls_nrel[u_cls[i]]; r++) {
                static const unsigned int note_periods[7] = {254, 226, 214, 190, 170, 160, 143}; /* A B C D E F G */
                if (cls_rel_verb[u_cls[i]][r] == VERB_sing &&
                    cls_rel_tgt[u_cls[i]][r] >= 0xFF10 && cls_rel_tgt[u_cls[i]][r] <= 0xFF16) {
                    audio_sing(note_periods[cls_rel_tgt[u_cls[i]][r] - 0xFF10]);
                }
            }
        }
    }
    /* SOKO: all targets non-frenles */
    if (!result && any_rel_soko) {
        unsigned char c2, r;
        for (c2 = 0; c2 < nclass; c2++) {
            for (r = 0; r < cls_nrel[c2]; r++) {
                unsigned char all = 1, any = 0;
                if (cls_rel_verb[c2][r] != VERB_soko) continue;
                for (i = 0; i < nunits; i++) {
                    if (u_cls[i] == DEAD_CLS || !unit_matches_noun(i, cls_rel_tgt[c2][r])) continue;
                    any = 1;
                    if (eval_cond(i, COND_frenles, 0, 0, 0)) { all = 0; break; }
                }
                if (any && all) result = 1;
            }
        }
    }
    if (!result && win_score > 0) result = 1;
    if (!result && undo_prop_count) result = 6;
    return result;
}


void do_dir_rules_b(void) {
    unsigned int u;
    /* cls_dir_code (built by rebuild_hot_props) is this function's
       whole per-class desired/spin result.  The old scan called
       prop_dir_of() for every property of every unit, twice per turn. */
    if (!any_dir_work) return;
    for (u = 0; u < nunits; u++) {
        unsigned char c = u_cls[u];
        unsigned char i, code, desired;
        if (c == DEAD_CLS) continue;
        code = cls_dir_code[c];
        if (!code) continue;
        desired = code & 15;
        if (desired) {
            face_unit(u, desired);
        } else {
            unsigned char spin = (code >> 4) & 7;
            if (spin) face_unit(u, dir_add(u_dir[u] & DIR_MASK, (signed char)spin));
        }
        if (!(code & DIR_CODE_ANTI)) continue;
        /* Anti-direction properties: preserve the original ordered
           scan (later entries observe the facing left by earlier ones). */
        for (i = 0; i < cls_nprops[c]; i++) {
            unsigned char d;
            if (cls_props_cnt[c][i] != PCNT_ANTI) continue;
            d = prop_dir_of(cls_props[c][i]);
            if (d && (u_dir[u] & DIR_MASK) == d) face_unit(u, dir_add(d, 1));
        }
    }
}
