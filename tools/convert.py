#!/usr/bin/env python3
"""Bab Be U -> Sega Master System data converter.

Reads the upstream Love2D game (tiles JSON, sprite PNGs, palette PNGs,
.bab levels, XM music, WAV sfx, TTF font) and emits banked binary data +
generated C headers for the SMS engine.  See CONVERSION.md (generated)
for the per-level / per-word conversion report.
"""
import os, sys, json, re, struct, collections, math, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from babdata import BabData, ShapeBuilder, downscale_16, pack_shape, rotate_shape_cw, sms_quantise, sms_rgb
from PIL import Image, ImageFont, ImageDraw

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/work/bab-be-u')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gen')
os.makedirs(OUT, exist_ok=True)

# Engine limits (must match src/engine.h)
MAXW, MAXH = 128, 128
MAXUNITS = 255
MAXCLASS = 36
VRAM_CLASS_BUDGET = 415   # tiles 1..415 (font space reused while playing; 416..447 composite cache)
FONT_TILE0 = 1
CLASS_TILE0 = 1

SPECIAL_NOUNS = {'txt': 0xFF01, 'every1': 0xFF02, 'every2': 0xFF03,
                 'frens': 0xFF04, 'groop': 0xFF05, 'themself': 0xFF06,
                 'mous': 0xFF07, 'every1and2': 0xFF08}

TYPE_BITS = {'object': 1, 'property': 2, 'verb': 4, 'verb_class': 8,
             'verb_property': 16, 'verb_unit': 32, 'cond_prefix': 64,
             'cond_infix': 128, 'and': 256, 'not': 512, 'letter': 1024,
             'ellipsis': 2048, 'direction': 4096, 'group': 8192,
             'verb_direction': 16384, 'note': 32768}
# extra upstream type names folded into the mask above
TYPE_ALIAS = {'cond_infix_verb': 'cond_infix', 'cond_infix_verb_plus': 'cond_infix',
              'cond_infix_dir': 'cond_infix', 'cond_compare': 'cond_infix',
              'class_prefix': 'cond_prefix', 'gang_prefix': 'cond_prefix',
              'gang': 'object', 'parenthesis': 'letter', 'verb_all': 'verb',
              'verb_sing': 'verb_unit', 'ditto': 'object'}

bab = BabData(ROOT)
builder = ShapeBuilder(bab)
report = {'levels': [], 'notes': []}

# --------------------------------------------------------------------------
# 1. Parse all levels, resolve tiles (creates dynamic txt_/n't tiles)
# --------------------------------------------------------------------------
raw_levels = []          # (relpath_noext, data)
level_index_by_rel = {}
for rel, path in bab.all_levels():
    try:
        d = bab.parse_level(path)
    except Exception as e:
        report['notes'].append(f'level {rel}: parse failed: {e}')
        continue
    relne = rel[:-4]  # strip .bab
    level_index_by_rel[relne] = len(raw_levels)
    raw_levels.append((relne, d))

def resolve_name(name):
    t = bab.get_tile(name)
    if t: return t['name']
    # A few upstream levels reference tiles whose JSON definitions were
    # removed upstream but whose sprites remain (larry, tot, reflecr, zip).
    # Reconstruct a minimal definition from the sprite, as getTile() would
    # have found the (now missing) JSON entry.
    if name.endswith("n't"): return None
    base = name[4:] if name.startswith('txt_') else name
    spr = ('txt/' + base) if name.startswith('txt_') else base
    low = {k.lower(): k for k in bab.sprite_paths}
    use = spr if spr in bab.sprite_paths else low.get(spr.lower(), base if base in bab.sprite_paths else 'wat')
    report['notes'].append(f'tile "{name}": no JSON definition upstream; reconstructed from sprite "{use}"')
    t = bab._add_tile({'name': name, 'sprite': [use], 'types': ['object'], 'color': [[0, 3]]})
    return t['name']

for relne, d in raw_levels:
    for u in d['_units']:
        u['_tile'] = resolve_name(u['tile'])

# Registry: every tile known after resolution, plus txt_ variants and
# referents of everything placed, to a fixpoint.
def referent_name(tile):
    """Fullname of the class an object-word tile refers to (or special)."""
    name = tile['name']
    if tile['is_text'] and name.startswith('txt_'):
        return name[4:]
    return None

changed = True
while changed:
    changed = False
    for t in list(bab.tiles.values()):
        ref = referent_name(t)
        if ref and ref not in SPECIAL_NOUNS and bab.get_tile(ref) is None:
            pass  # unresolved referent stays unresolved (reported later)
        # ensure txt_ variant exists for placed/referent tiles
    # create txt_ variants for all current non-text tiles that are used
    break

used_names = set()
for relne, d in raw_levels:
    for u in d['_units']:
        if u['_tile']:
            used_names.add(u['_tile'])
queue = list(used_names)
while queue:
    n = queue.pop()
    t = bab.tiles.get(n)
    if not t: continue
    if t['is_text'] and 'object' in t['types'] and 'letter' not in t['types']:
        ref = referent_name(t)
        if ref and ref not in SPECIAL_NOUNS:
            rn = resolve_name(ref)
            if rn and rn not in used_names:
                used_names.add(rn); queue.append(rn)
    if not t['is_text']:
        tn = resolve_name('txt_' + n)
        if tn and tn not in used_names:
            used_names.add(tn); queue.append(tn)

registry = sorted(used_names)
tile_id = {n: i for i, n in enumerate(registry)}
report['notes'].append(f'registered {len(registry)} tiles (upstream defined {len(bab.tiles)})')

# --------------------------------------------------------------------------
# 2. Vocabulary: properties, verbs, conditions
# --------------------------------------------------------------------------
def word_name(tile):
    """The word a text tile contributes to a sentence (unit.textname)."""
    if 'letter' in tile['types']:
        return tile['name'][7:]
    return tile['name'][4:] if tile['name'].startswith('txt_') else tile['name']

props, verbs, conds = {}, {}, {}
for n in registry:
    t = bab.tiles[n]
    if t.get('nt'): continue
    wn = word_name(t)
    if t['is_text'] and 'property' in t['types']:
        props.setdefault(wn, len(props))
    if t['is_text'] and 'verb' in t['types']:
        verbs.setdefault('be' if wn in ('is',) else wn, len(verbs))
    if t['is_text'] and ('cond_prefix' in t['types'] or 'cond_infix' in t['types'] or
                          'class_prefix' in t['types']):
        conds.setdefault(wn, len(conds))
# direction words are properties in upstream (txt_up etc. have 'property')
prop_names = sorted(props, key=lambda k: props[k])
verb_names = sorted(verbs, key=lambda k: verbs[k])
cond_names = sorted(conds, key=lambda k: conds[k])
assert len(prop_names) < 128 and len(verb_names) < 128 and len(cond_names) < 128, \
    (len(prop_names), len(verb_names), len(cond_names))

# --------------------------------------------------------------------------
# 3. Shapes for every registered tile
# --------------------------------------------------------------------------
# variants: base, rot (90 CW), dim shape (inactive sprite_transform), wob2, wob3
shape_blobs = []       # packed 128B each
shape_index = {}       # (packed, sources-tuple) -> id
tile_sources = {}      # tile name -> [sources]
tile_shapes = {}       # tile name -> dict(variant -> shape id or -1)

def merge_sources(variants):
    """variants: list of (pixels16, sources). Union sources, cap 15."""
    allsrc = []
    for px, srcs in variants:
        for s in srcs:
            if s not in allsrc: allsrc.append(s)
    if len(allsrc) > 15:
        # keep 15 most frequent, remap rest to nearest grey same-coord / first
        freq = collections.Counter()
        for px, srcs in variants:
            for v in px:
                if v: freq[srcs[v - 1]] += 1
        keep = [s for s, _ in freq.most_common(15)]
        remap = {}
        for s in allsrc:
            if s in keep: continue
            best = keep[0]; bestd = 1e9
            for k in keep:
                if s[0] == k[0]:
                    d = abs(s[-1] - k[-1]) + (0 if s[1] == k[1] else 1000)
                    if d < bestd: bestd, best = d, k
            remap[s] = best
        allsrc = keep
    else:
        remap = {}
    index = {s: i + 1 for i, s in enumerate(allsrc)}
    out = []
    for px, srcs in variants:
        out.append([0 if v == 0 else index[remap.get(srcs[v - 1], srcs[v - 1])] for v in px])
    return out, allsrc

for n in registry:
    t = bab.tiles[n]
    variants = {}
    # Preserve letter counters/separations in text; use ordinary 50%
    # coverage for objects so narrow poles/outlines are less fragile.
    min_opaque = 3 if t['is_text'] else 2
    px, srcs = builder.composite(t)
    variants['base'] = (downscale_16(px, min_opaque), srcs)
    if t['rotate']:
        variants['rot'] = (rotate_shape_cw(variants['base'][0]), srcs)
    if t['is_text'] and (t.get('sprite_transforms') or {}).get('inactive'):
        px2, srcs2 = builder.composite(t, inactive=True)
        variants['dim'] = (downscale_16(px2, min_opaque), srcs2)
    if t.get('wobble'):
        for fr, key in ((2, 'wob2'), (3, 'wob3')):
            px3, srcs3 = builder.composite(t, frame=fr)
            variants[key] = (downscale_16(px3, min_opaque), srcs3)
    keys = list(variants)
    merged, allsrc = merge_sources([variants[k] for k in keys])
    tile_sources[n] = allsrc
    tile_shapes[n] = {}
    for k, px16 in zip(keys, merged):
        packed = pack_shape(px16)
        skey = (packed, tuple(allsrc))
        if skey not in shape_index:
            shape_index[skey] = len(shape_blobs)
            shape_blobs.append(packed)
        tile_shapes[n][k] = shape_index[skey]

# --------------------------------------------------------------------------
# 3b. Overworld lvl/lin render variants
# --------------------------------------------------------------------------
# Upstream drawUnit() does not use the plain lvl/lin sprite for every
# unit: lvl uses lvl_hidden/lvl_locked/lvl_won according to its special
# visibility (and save state), open lvl units get a number/dots/letter/
# other icon drawn inside the square, and hidden lin units use the
# outline-only lin_hidden sprite.  The first port ignored those specials,
# so every world-map node/path was drawn with the bright "open" artwork.
# Pre-render those variants as extra shapes whose colour-source indices
# line up with the lvl/lin class colour tables; the level blob records
# the shape id beside each entrance special.
lvl_shape_variants = {}      # normalised special key -> (normal, won) shape ids
SHAPE_LIN_HIDDEN = 0xFFFF

def _render_coord(tile_name):
    mc = (bab.tiles[tile_name].get('color') or [[0, 3]])[0]
    return mc[0] + mc[1] * 7 if len(mc) == 2 else 21

def _ensure_render_source(tile_name, grey):
    src = ('pal', _render_coord(tile_name), grey)
    srcs = tile_sources[tile_name]
    if src not in srcs:
        srcs.append(src)
    return srcs.index(src) + 1

def _grey_source_index(tile_name, grey):
    coord = _render_coord(tile_name)
    best, bestd = 1, 10 ** 9
    for i, s in enumerate(tile_sources[tile_name]):
        if s[0] == 'pal' and s[1] == coord:
            d = abs(s[2] - grey)
            if d < bestd:
                best, bestd = i + 1, d
    return best

def _sprite_indices(sprite_name, tile_name, w, h):
    im = bab.sprite(sprite_name) or bab.sprite('wat')
    if im is None:
        return [0] * (w * h)
    if im.size != (w, h):
        im = im.resize((w, h), Image.NEAREST)
    out = []
    for r, g, b, a in im.getdata():
        if a < 128:
            out.append(0)
        else:
            out.append(_grey_source_index(tile_name, (r + g + b) // 3))
    return out

def _append_render_shape(tile_name, pixels32, min_opaque):
    packed = pack_shape(downscale_16(pixels32, min_opaque))
    skey = (packed, tuple(tile_sources[tile_name]))
    if skey not in shape_index:
        shape_index[skey] = len(shape_blobs)
        shape_blobs.append(packed)
    return shape_index[skey]

if 'lvl' in tile_sources and 'lin' in tile_sources:
    for grey in (255, 205, 175, 153, 130, 0):
        _ensure_render_source('lvl', grey)
    for grey in (255, 205):
        _ensure_render_source('lin', grey)

    _lvl_bases = {
        'open': _sprite_indices('lvl', 'lvl', 32, 32),
        'locked': _sprite_indices('lvl_locked', 'lvl', 32, 32),
        'hidden': _sprite_indices('lvl_hidden', 'lvl', 32, 32),
        'won': _sprite_indices('lvl_won', 'lvl', 32, 32),
    }

    def _overlay_lvl_icon(base, sp):
        out = list(base)
        style = sp.get('iconstyle') or 'number'
        number = sp.get('number')
        try:
            number = int(number)
        except Exception:
            number = 1
        def blit(icon, iw, ih, ox, oy):
            for yy in range(ih):
                for xx in range(iw):
                    v = icon[yy * iw + xx]
                    if v:
                        out[(oy + yy) * 32 + ox + xx] = v
        if style == 'dots':
            blit(_sprite_indices(f'levelicon_dots_{max(1, min(9, number))}', 'lvl', 24, 24), 24, 24, 4, 4)
        elif style == 'letter':
            letter = 'abcdefghijklmnopqrstuvwxyz'[max(1, min(26, number)) - 1]
            blit(_sprite_indices(f'letter_{letter}', 'lvl', 24, 24), 24, 24, 4, 4)
        elif style == 'other':
            blit(_sprite_indices(sp.get('iconname') or 'wat', 'lvl', 24, 24), 24, 24, 4, 4)
        else:  # number (the upstream default), always two digits
            num = f'{max(0, min(99, number)):02d}'
            blit(_sprite_indices(f'levelicon_{num[0]}', 'lvl', 12, 24), 12, 24, 4, 4)
            blit(_sprite_indices(f'levelicon_{num[1]}', 'lvl', 12, 24), 12, 24, 16, 4)
        return out

    def _lvl_variant_key(sp):
        vis = sp.get('visibility') or 'locked'
        if vis != 'open':
            return (vis,)
        return (vis, sp.get('iconstyle') or 'number', sp.get('number'),
                sp.get('iconname') or '')

    for _relne, _d in raw_levels:
        for _u in _d['_units']:
            if _u.get('_tile') != 'lvl':
                continue
            _sp = _u.get('special') or {}
            _key = _lvl_variant_key(_sp)
            if _key in lvl_shape_variants:
                continue
            _vis = _key[0]
            if _vis == 'hidden':
                _sid = _append_render_shape('lvl', _lvl_bases['hidden'], 2)
                lvl_shape_variants[_key] = (_sid, _sid)
            elif _vis == 'open':
                _normal = _append_render_shape('lvl', _overlay_lvl_icon(_lvl_bases['open'], _sp), 2)
                _won = _append_render_shape('lvl', _overlay_lvl_icon(_lvl_bases['won'], _sp), 2)
                lvl_shape_variants[_key] = (_normal, _won)
            else:  # locked, including upstream's nil visibility
                _sid = _append_render_shape('lvl', _lvl_bases['locked'], 2)
                lvl_shape_variants[_key] = (_sid, _sid)

    SHAPE_LIN_HIDDEN = _append_render_shape(
        'lin', _sprite_indices('lin_hidden', 'lin', 32, 32), 2)

def lvl_variant_shapes(sp):
    vis = (sp or {}).get('visibility') or 'locked'
    if vis != 'open':
        return lvl_shape_variants.get((vis,), (0xFFFF, 0xFFFF))
    key = (vis, (sp or {}).get('iconstyle') or 'number',
           (sp or {}).get('number'), (sp or {}).get('iconname') or '')
    return lvl_shape_variants.get(key, (0xFFFF, 0xFFFF))

print(f'tiles={len(registry)} shapes={len(shape_blobs)} props={len(prop_names)} verbs={len(verb_names)} conds={len(cond_names)}')

# --------------------------------------------------------------------------
# 4. Bank allocator
# --------------------------------------------------------------------------
BANK_SIZE = 16384
banks = []             # list[bytearray], bank numbers start at 2
def alloc(data, align=1, whole=False):
    """Append data to banks; return (bank_number, offset)."""
    if whole and len(data) > BANK_SIZE:
        raise ValueError('blob too big for one bank')
    if not banks: banks.append(bytearray())
    cur = banks[-1]
    pad = (-len(cur)) % align
    if len(cur) + pad + len(data) > BANK_SIZE:
        banks.append(bytearray()); cur = banks[-1]; pad = 0
        if len(data) > BANK_SIZE: raise ValueError('blob > bank')
    cur += b'\x00' * pad
    off = len(cur)
    cur += data
    return (len(banks) - 1 + 2, off)

def banked_ptr(data, align=1):
    return alloc(data, align)

def alloc_span(data):
    """Allocate a blob spanning consecutive whole banks starting at offset 0
    of a fresh bank.  Returns (base_bank, 0); the engine addresses byte i
    as bank=base+(i>>14), off=i&0x3FFF."""
    if banks and len(banks[-1]) > 0:
        banks.append(bytearray())
    if not banks:
        banks.append(bytearray())
    base = len(banks) - 1 + 2
    pos = 0
    while pos < len(data):
        chunk = data[pos:pos + BANK_SIZE]
        banks[-1] += chunk
        pos += len(chunk)
        if pos < len(data):
            banks.append(bytearray())
    return (base, 0)

# shapes first (128B aligned blocks)
shape_dir = []
for packed in shape_blobs:
    shape_dir.append(alloc(packed, align=128))
shape_dir_blob = b''.join(struct.pack('<BH', b, o) for b, o in shape_dir)
SHAPEDIR = alloc(shape_dir_blob, align=2)

# colour-source lists per tile
color_blob = bytearray()
color_off = {}
for n in registry:
    srcs = tile_sources[n]
    color_off[n] = len(color_blob)
    color_blob.append(len(srcs))
    mc = (bab.tiles[n].get('color') or [[0, 3]])[0]
    color_blob.append(mc[0] + mc[1] * 7 if len(mc) == 2 else 255)
    for s in srcs:
        if s[0] == 'pal':
            color_blob += bytes([0, s[1], s[2]])
        else:
            color_blob += bytes([1, s[1], 0])
COLORS = alloc_span(bytes(color_blob))

# letter strings
letter_strings = {}
letter_piece_list = []
letter_piece_id = {}
for n in registry:
    t = bab.tiles[n]
    if 'letter' in t['types']:
        letter_strings[n] = n[7:]  # textname; special dir-based renames done in engine
        letter_piece_id[n] = len(letter_piece_list)
        letter_piece_list.append(n[7:])

# global tile table (22 bytes/entry)
def type_mask(tile):
    m = 0
    for ty in tile['types']:
        m |= TYPE_BITS.get(TYPE_ALIAS.get(ty, ty), 0)
    return m

def noun_for(tile):
    ref = referent_name(tile)
    if ref is None: return 0xFFFF
    if ref.endswith("n't"): ref = ref[:-3]
    if ref in SPECIAL_NOUNS: return SPECIAL_NOUNS[ref]
    rn = resolve_name(ref)
    return tile_id[rn] if rn in tile_id else 0xFFFF

tile_table = bytearray()
for n in registry:
    t = bab.tiles[n]
    sh = tile_shapes[n]
    wn = word_name(t)
    ref, ref2 = 0xFFFF, 0xFFFF
    if t['is_text']:
        if 'object' in t['types'] and 'letter' not in t['types']:
            ref = noun_for(t)
        if 'property' in t['types'] and wn in props:
            ref = props[wn] if ref == 0xFFFF else ref
            ref2 = props[wn]
        if 'verb' in t['types']:
            ref = verbs['be' if wn == 'is' else wn] if (('be' if wn == 'is' else wn) in verbs) else ref
        if ('cond_prefix' in t['types'] or 'cond_infix' in t['types'] or 'class_prefix' in t['types']) and wn in conds:
            if 'property' in t['types']:
                ref2 = (ref2 & 0xFF) | (conds[wn] << 8) if ref2 != 0xFFFF else conds[wn]
            else:
                ref = conds[wn]
    flags = 0
    if t['is_text']: flags |= 1
    if t['rotate']: flags |= 2
    if t.get('nt'): flags |= 4
    if t.get('wobble'): flags |= 8
    if 'rot' in sh: flags |= 16
    if t.get('portal'): flags |= 32
    txt_variant = tile_id.get('txt_' + n, 0xFFFF) if not t['is_text'] else 0xFFFF
    if t['is_text'] and n[4:] in tile_id:
        txt_variant = tile_id[n[4:]]  # for text tiles: the base object (thingify target)
    tile_table += struct.pack('<HHHHBBHHHI',
        sh.get('base', 0xFFFF), sh.get('rot', 0xFFFF), sh.get('wob2', 0xFFFF), sh.get('wob3', 0xFFFF),
        max(0, min(255, t['layer'])), flags, ref if ref is not None else 0xFFFF,
        ref2 if ref2 is not None else 0xFFFF, txt_variant, 0)
    # replace trailing I with color offset (u32 not needed; patch below)
# rebuild with colour offsets properly: entry = 4*H + B B H H H H = 20 bytes
tile_table = bytearray()
for n in registry:
    t = bab.tiles[n]
    sh = tile_shapes[n]
    wn = word_name(t)
    ref, ref2 = 0xFFFF, 0xFFFF
    if t['is_text']:
        if 'property' in t['types'] and wn in props: ref2 = props[wn]
        if ('cond_prefix' in t['types'] or 'cond_infix' in t['types'] or 'class_prefix' in t['types']) and wn in conds:
            ref2 = (ref2 & 0xFF) | (conds[wn] << 8)
        if 'verb' in t['types']:
            ref = verbs.get('be' if wn in ('is', "ben't", "isn't") else wn, 0xFFFF)
        elif 'object' in t['types'] and 'letter' not in t['types']:
            ref = noun_for(t)
        elif ('cond_prefix' in t['types'] or 'cond_infix' in t['types']) and 'property' not in t['types']:
            ref = conds.get(wn, 0xFFFF)
        if 'letter' in t['types']:
            ref = letter_piece_id.get(n, 0xFFFF)
    flags = 0
    if t['is_text']: flags |= 1
    if t['rotate']: flags |= 2
    if t.get('nt') or n.endswith("n't"): flags |= 4
    if t.get('wobble'): flags |= 8
    if 'rot' in sh: flags |= 16
    if t.get('portal'): flags |= 32
    if 'dim' in sh: flags |= 64
    rel = 0xFFFF
    if not t['is_text']:
        rel = tile_id.get(resolve_name('txt_' + n) or '', 0xFFFF)
    elif n.startswith('txt_') and n[4:] in tile_id:
        rel = tile_id[n[4:]]
    tile_table += struct.pack('<HHHHBBHHHHH',
        sh.get('base', 0xFFFF), sh.get('rot', 0xFFFF), sh.get('wob2', 0xFFFF),
        sh.get('dim', sh.get('wob3', 0xFFFF)), max(0, min(255, t['layer'])), flags,
        type_mask(t), ref, ref2, rel, color_off[n])
TILETABLE = alloc_span(bytes(tile_table))
TILE_ENTRY_SIZE = 20

# spelling dictionary: string -> (kind, ref)
# kind: 0 noun(tile id/special) 1 prop 2 verb 3 cond 4 and 5 not 6 ellipsis
dict_entries = {}
for n in registry:
    t = bab.tiles[n]
    if not t['is_text'] or t.get('nt') or 'letter' in t['types']: continue
    wn = word_name(t)
    cands = []
    if 'object' in t['types']:
        cands.append((0, noun_for(t)))
    if 'property' in t['types'] and wn in props:
        cands.append((1, props[wn]))
    if 'verb' in t['types']:
        cands.append((2, verbs.get('be' if wn == 'is' else wn, 0xFFFF)))
    if ('cond_prefix' in t['types'] or 'cond_infix' in t['types']) and wn in conds:
        cands.append((3, conds[wn]))
    if 'and' in t['types']: cands.append((4, 0))
    if 'not' in t['types']: cands.append((5, 0))
    if 'ellipsis' in t['types']: cands.append((6, 0))
    if cands:
        dict_entries.setdefault(wn, cands[0])
        for a in t.get('alias') or []:
            dict_entries.setdefault(a, cands[0])
dict_strings = bytearray()
dict_table = bytearray()
for s in sorted(dict_entries):
    kind, ref = dict_entries[s]
    if ref == 0xFFFF and kind == 0: continue
    dict_table += struct.pack('<HBHB', len(dict_strings), len(s.encode('utf-8', 'ignore')), ref, kind)
    dict_strings += s.encode('utf-8', 'ignore') + b'\x00'
DICT_STR = alloc(bytes(dict_strings))
DICT_TAB = alloc(bytes(dict_table), align=2)
NUM_DICT = len(dict_table) // 6

# --------------------------------------------------------------------------
# 5. Music (XM) + SFX (WAV) conversion
# --------------------------------------------------------------------------
import xm2psg, wav2sfx
music_ids = {}
music_blobs = []
bgm_dir = os.path.join(ROOT, 'assets/audio/bgm')
available_bgm = {}
for f in os.listdir(bgm_dir):
    available_bgm[os.path.splitext(f)[0]] = os.path.join(bgm_dir, f)
def music_id_for(name):
    if name in music_ids: return music_ids[name]
    path = available_bgm.get(name)
    if path and path.endswith('.xm'):
        try:
            data = xm2psg.convert(path)
            ptr = alloc_span(data)
            mid = len(music_blobs) + 1
            music_blobs.append((name, ptr, len(data)))
            music_ids[name] = mid
            return mid
        except Exception as e:
            report['notes'].append(f'music {name}: XM conversion failed: {e}')
    elif path:
        report['notes'].append(f'music "{name}" is recorded audio ({os.path.basename(path)}); no PSG conversion possible')
    else:
        report['notes'].append(f'music "{name}" not found in upstream assets')
    music_ids[name] = 0
    return 0

SFX_NAMES = ['move', 'rule', 'undo', 'bacc', 'win', 'unwin', 'break', 'unlock',
             'sink', 'hotte', 'snacc', 'bonus', 'bup', 'clicc', 'anti clicc',
             'fail', 'gone', 'honk1', 'thicc', 'unthicc', 'timestop', 'time resume']
sfx_ptrs = []
for nm in SFX_NAMES:
    for ext in ('.wav', '.ogg'):
        p = os.path.join(ROOT, 'assets/audio/sfx', nm + ext)
        if os.path.exists(p) and ext == '.wav':
            try:
                data = wav2sfx.convert(p)
                sfx_ptrs.append((nm, alloc(data), len(data)))
                break
            except Exception as e:
                report['notes'].append(f'sfx {nm}: conversion failed: {e}')
                sfx_ptrs.append((nm, (0, 0), 0)); break
    else:
        sfx_ptrs.append((nm, (0, 0), 0))

# --------------------------------------------------------------------------
# 6. Levels
# --------------------------------------------------------------------------
def resolve_level_ref(cur_rel, ref):
    if not ref: return 0xFFFF
    world = cur_rel.split('/')[0]
    cand = world + '/' + ref
    if cand in level_index_by_rel: return level_index_by_rel[cand]
    if ref in level_index_by_rel: return level_index_by_rel[ref]
    return 0xFFFF

def level_letter_words(d):
    """Words spelled by letter runs in the initial layout (rows+cols)."""
    grid = {}
    for u in d['_units']:
        if u['_tile'] and 'letter' in bab.tiles[u['_tile']]['types']:
            grid[(u['x'], u['y'])] = u['_tile'][7:]
    words = set()
    if not grid: return words
    maxx = max(x for x, y in grid); maxy = max(y for x, y in grid)
    for y in range(maxy + 1):
        s = ''
        for x in range(maxx + 2):
            if (x, y) in grid: s += grid[(x, y)]
            else:
                if s: words.add(s)
                s = ''
    for x in range(maxx + 1):
        s = ''
        for y in range(maxy + 2):
            if (x, y) in grid: s += grid[(x, y)]
            else:
                if s: words.add(s)
                s = ''
    return words

level_blobs = []   # per level: dict with ptr or exclusion reason code
for li, (relne, d) in enumerate(raw_levels):
    info = {'rel': relne, 'name': d.get('name', relne), 'reason': 0, 'flags': 0}
    units = [u for u in d['_units'] if u['_tile']]
    unresolved = [u['tile'] for u in d['_units'] if not u['_tile']]
    w, h = d.get('width', 0), d.get('height', 0)
    # class closure
    classes = []
    def add_class(name):
        if name and name in tile_id and name not in classes: classes.append(name)
    for u in units: add_class(u['_tile'])
    for n in list(classes):
        t = bab.tiles[n]
        if t['is_text'] and 'object' in t['types'] and 'letter' not in t['types']:
            ref = referent_name(t)
            if ref and ref not in SPECIAL_NOUNS:
                add_class(resolve_name(ref))
    words_in_level = {word_name(bab.tiles[n]) for n in classes if bab.tiles[n]['is_text']}
    if 'txtify' in words_in_level or 'thingify' in words_in_level or 'wurd' in words_in_level:
        for n in list(classes):
            t = bab.tiles[n]
            if not t['is_text']: add_class(resolve_name('txt_' + n))
            elif n.startswith('txt_'): add_class(resolve_name(n[4:]))
    for wtxt in level_letter_words(d):
        ent = dict_entries.get(wtxt)
        if ent and ent[0] == 0 and ent[1] < 0xFF00:
            add_class(registry[ent[1]])
    info['classes'] = classes
    info['w'], info['h'], info['nunits'] = w, h, len(units)
    vram = sum(4 * (1 + (1 if bab.tiles[n]['rotate'] else 0) + (1 if bab.tiles[n]['is_text'] else 0)) for n in classes)
    info['vram'] = vram
    if unresolved: info['reason'] = 4; info['unresolved'] = unresolved[:4]
    elif w > MAXW or h > MAXH: info['reason'] = 1
    elif len(units) > MAXUNITS: info['reason'] = 2
    elif len(classes) > MAXCLASS or vram > VRAM_CLASS_BUDGET: info['reason'] = 3
    if any('letter' in bab.tiles[n]['types'] for n in classes): info['flags'] |= 4
    if d.get('is_overworld'): info['flags'] |= 1
    level_blobs.append(info)
    if info['reason']:
        continue
    # ---- per-source SMS colours (full-precision tint of level palette)
    pal_name = d.get('palette', 'default')
    pal = bab.palettes.get(pal_name) or bab.palettes['default']
    def src_sms(s, dim=False):
        if s[0] == 'pal':
            r, g, b = pal[s[1]]
            r, g, b = r * s[2] // 255, g * s[2] // 255, b * s[2] // 255
        else:
            r, g, b = sms_rgb(s[1])
        if dim:
            br, bg, bb = pal[1]
            r = int(0.67 * (br * 0.5) + 0.33 * r); g = int(0.67 * (bg * 0.5) + 0.33 * g); b = int(0.67 * (bb * 0.5) + 0.33 * b)
        return sms_quantise(r, g, b)
    bg_sms = sms_quantise(*pal[1])
    # ---- assemble level blob (v2: engine bakes tiles/palette slots at load)
    cls_index = {n: i for i, n in enumerate(classes)}
    blob = bytearray()
    parent = resolve_level_ref(relne, d.get('parent_level', ''))
    nxt = resolve_level_ref(relne, d.get('next_level', ''))
    blob += struct.pack('<BBBBHHHBB', w, h, len(classes), 0, len(units), parent, nxt,
                        music_id_for(d.get('music', 'bab be go')), d.get('puffs_to_clear', 0) & 0xFF)
    blob.append(bg_sms)
    for n in classes: blob += struct.pack('<H', tile_id[n])
    for n in classes:
        blob += bytes(src_sms(s) for s in tile_sources[n])
        if bab.tiles[n]['is_text']:
            blob += bytes(src_sms(s, True) for s in tile_sources[n])
    for u in units:
        dir_byte = u['dir'] & 0xFF
        # Bit 0x10 is free in the engine's u_dir byte (DIR_MASK is the low
        # nibble); use it to mark hidden lin units for the renderer.
        if u['_tile'] == 'lin' and (u.get('special') or {}).get('visibility') == 'hidden':
            dir_byte |= 0x10
        blob += struct.pack('<BBBB', cls_index[u['_tile']], u['x'] & 0xFF, u['y'] & 0xFF, dir_byte)
    specials = []
    for i, u in enumerate(units):
        sp = u.get('special') or {}
        if sp.get('level'):
            # Upstream getUnitSprite() treats a nil lvl visibility as locked.
            vis = {'open': 0, 'locked': 1, 'hidden': 2}.get(sp.get('visibility'), 1)
            shape_normal, shape_won = lvl_variant_shapes(sp)
            specials.append(struct.pack('<HBHBHH', i, 1, resolve_level_ref(relne, sp['level']),
                                        vis, shape_normal, shape_won))
        if sp.get('customletter'):
            st = str(sp['customletter']).encode('utf-8', 'ignore')[:6]
            specials.append(struct.pack('<HBB', i, 2, len(st)) + st)
    blob += struct.pack('<B', len(specials))
    for spb in specials: blob += spb
    info['ptr'] = alloc(bytes(blob), align=2)
    info['blob_size'] = len(blob)


# Title pseudo-level (rendered with the real level loader; units are the
# actual upstream bab/flog/txt tiles on gras, default palette)
if True:
    classes = ['gras', 'bab', 'roc', 'flog', 'txt_bab', 'txt_be', 'txt_u']
    cls_index = {n: i for i, n in enumerate(classes)}
    units = []
    for x in range(1, 15): units.append(('gras', x, 8, 1))
    units += [('bab', 4, 8, 1), ('roc', 7, 8, 1), ('flog', 11, 8, 1),
              ('txt_bab', 5, 3, 1), ('txt_be', 7, 3, 1), ('txt_u', 9, 3, 1)]
    pal = bab.palettes['default']
    def src_sms(s, dim=False):
        if s[0] == 'pal':
            r, g, b = pal[s[1]]
            r, g, b = r * s[2] // 255, g * s[2] // 255, b * s[2] // 255
        else:
            r, g, b = sms_rgb(s[1])
        if dim:
            br, bg, bb = pal[1]
            r = int(0.67 * (br * 0.5) + 0.33 * r); g = int(0.67 * (bg * 0.5) + 0.33 * g); b = int(0.67 * (bb * 0.5) + 0.33 * b)
        return sms_quantise(r, g, b)
    blob = bytearray()
    blob += struct.pack('<BBBBHHHBB', 16, 12, len(classes), 0, len(units), 0xFFFF, 0xFFFF,
                        music_id_for('bab be go'), 0)
    blob.append(sms_quantise(*pal[1]))
    for n in classes: blob += struct.pack('<H', tile_id[n])
    for n in classes:
        blob += bytes(src_sms(s) for s in tile_sources[n])
        if bab.tiles[n]['is_text']:
            blob += bytes(src_sms(s, True) for s in tile_sources[n])
    for (n, x, y, dd) in units:
        blob += struct.pack('<BBBB', cls_index[n], x, y, dd)
    blob += struct.pack('<B', 0)
    TITLE = alloc(bytes(blob), align=2)

# level directory + names
names_blob = bytearray()
def intern_str(s):
    off = len(names_blob)
    names_blob.extend(s.encode('utf-8', 'ignore')[:40] + b'\x00')
    return off
# menu order: main campaign world first, then solo levels, minigames, gadgets
world_order = {'bab be u': 0, 'solo levels': 1, 'minigames': 2, 'gadgets': 3}
order = sorted(range(len(raw_levels)),
               key=lambda i: (world_order.get(raw_levels[i][0].split('/')[0], 9), raw_levels[i][0].lower()))
dir_blob = bytearray()
menu_index_of_level = {}
for mi, li in enumerate(order):
    menu_index_of_level[li] = mi
for li in order:
    info = level_blobs[li]
    bank, off = info.get('ptr', (0, 0))
    dir_blob += struct.pack('<BHHHHBBH', bank, off, intern_str(info['name']),
                            intern_str(info['rel'].split('/')[0]), li,
                            info['reason'], info['flags'], len(info.get('classes', [])))
LEVELDIR = alloc(bytes(dir_blob), align=2)
NAMES = alloc_span(bytes(names_blob))
MUSICDIR = alloc(b''.join(struct.pack('<BH', ptr[0], ptr[1]) for _, ptr, _ in music_blobs), align=2)
SFXDIR = alloc(b''.join(struct.pack('<BH', ptr[0], ptr[1]) for _, ptr, _ in sfx_ptrs), align=2)

# --------------------------------------------------------------------------
# 7. Font from the upstream TTF (8bitoperator), 8x8 1bpp -> SMS 4bpp tiles
# --------------------------------------------------------------------------
FONT_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,-'!?:;()&%+*#@/<>[]"
font_path = os.path.join(ROOT, 'assets/fonts/8bitoperator.ttf')
font_tiles = bytearray()
try:
    font = ImageFont.truetype(font_path, 8)
except Exception:
    font = ImageFont.load_default()
for ch in FONT_CHARS:
    im = Image.new('L', (8, 8), 0)
    dr = ImageDraw.Draw(im)
    try:
        bbox = dr.textbbox((0, 0), ch, font=font)
        dr.text((-(bbox[0]), -(bbox[1])), ch, fill=255, font=font)
    except Exception:
        pass
    px = list(im.getdata())
    for y in range(8):  # SMS planar 4bpp: 4 bytes per row, we use plane0 only
        bits = 0
        for x in range(8):
            if px[y * 8 + x] > 100: bits |= (0x80 >> x)
        font_tiles += bytes([bits, 0, 0, 0])
with open(os.path.join(OUT, 'font.h'), 'w') as f:
    f.write('/* generated from upstream assets/fonts/8bitoperator.ttf */\n')
    f.write(f'#define FONT_CHARS {json.dumps(FONT_CHARS)}\n')
    f.write('const unsigned char font_tiles[] = {')
    f.write(','.join(str(b) for b in font_tiles))
    f.write('};\n')

# --------------------------------------------------------------------------
# 8. Generated C header + bank asm
# --------------------------------------------------------------------------
SPECIAL_ENUM = {':(':'SAD', ':)':'WIN', ':o':'BONUS', 'un:)':'UNWIN', "n'tify":'NTIFY', "n'tifyyy":'NTIFY3'}
def enum_name(kind, name):
    if name in SPECIAL_ENUM: return f'{kind}_{SPECIAL_ENUM[name]}'
    s = re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_')
    if not s or s[0].isdigit(): s = 'X' + s
    return f'{kind}_{s}'

anti_prop = []
for p in prop_names:
    anti_prop.append(props.get(bab.anti_replacements.get(p, ''), -1))
anti_cond = []
for c in cond_names:
    anti_cond.append(conds.get(bab.anti_replacements.get(c, ''), -1))
anti_verb = []
for v in verb_names:
    anti_verb.append(verbs.get(bab.anti_replacements.get(v, ''), -1))
cond_reverse = [1 if c in bab.anti_reverses else 0 for c in cond_names]
# colour name per palette coord (index = x+y*7): cond id of colour or -1
colour_of_coord = []
for y in range(5):
    for x in range(7):
        nm = bab.colour_for_coord.get((x, y))
        colour_of_coord.append(conds.get(nm, -1) if nm else -1)

# Data banks use numbers 2..len(banks)+1.  The next two banks hold the
# banked parser and movement code; these values must agree with banks.mk.
PARSER_BANK = len(banks) + 2
MOVE_BANK = len(banks) + 3

with open(os.path.join(OUT, 'gamedata.h'), 'w') as f:
    f.write('/* generated by tools/convert.py - do not edit */\n')
    f.write(f'#define GD_PARSER_BANK {PARSER_BANK}\n#define GD_MOVE_BANK {MOVE_BANK}\n')
    f.write(f'#define NUM_TILES {len(registry)}\n#define TILE_ENTRY_SIZE {TILE_ENTRY_SIZE}\n')
    f.write(f'#define NUM_LEVELS {len(raw_levels)}\n#define NUM_DICT {NUM_DICT}\n')
    f.write(f'#define NUM_MUSIC {len(music_blobs)}\n#define NUM_SFX {len(sfx_ptrs)}\n')
    for name, (b, o) in [('SHAPEDIR', SHAPEDIR), ('COLORS', COLORS), ('TILETABLE', TILETABLE),
                         ('DICT_STR', DICT_STR), ('DICT_TAB', DICT_TAB), ('LEVELDIR', LEVELDIR),
                         ('NAMES', NAMES), ('MUSICDIR', MUSICDIR), ('SFXDIR', SFXDIR), ('TITLE', TITLE)]:
        f.write(f'#define GD_{name}_BANK {b}\n#define GD_{name}_OFF {o}\n')
    f.write(f'#define FIRST_LEVEL_MENU {menu_index_of_level.get(level_index_by_rel.get("bab be u/mapp", 0), 0)}\n')
    f.write('#define LEVELDIR_ENTRY 13\n')
    for i, p in enumerate(prop_names): f.write(f'#define {enum_name("PROP", p)} {i} /* {p} */\n')
    f.write(f'#define NUM_PROPS {len(prop_names)}\n')
    for i, v in enumerate(verb_names): f.write(f'#define {enum_name("VERB", v)} {i} /* {v} */\n')
    f.write(f'#define NUM_VERBS {len(verb_names)}\n')
    for i, c in enumerate(cond_names): f.write(f'#define {enum_name("COND", c)} {i} /* {c} */\n')
    f.write(f'#define NUM_CONDS {len(cond_names)}\n')
    for i, (nm, _, _) in enumerate(sfx_ptrs): f.write(f'#define {enum_name("SFX", nm)} {i}\n')
    # special tile ids the engine needs
    for tn in ('lvl', 'selctr', 'bordr', 'lin', 'no1', 'mous', 'this', 'bab', 'txt_bab', 'txt_be', 'txt_u', 'flog',
               'txt_direction', 'txt_spin', 'letter_colon', 'letter_parenthesis', 'letter_h', 'letter_i',
               'letter_n', 'letter_z', 'letter_m', 'letter_w', 'letter_6', 'letter_9', 'letter_no', 'letter_>',
               'letter_a', 'letter_b', 'letter_c', 'letter_d', 'letter_e', 'letter_f', 'letter_g', 'prop', 'wut'):
        if tn in tile_id: f.write(f'#define {enum_name("TILE", tn)} {tile_id[tn]}\n')
    f.write(f'#define SHAPE_LIN_HIDDEN {SHAPE_LIN_HIDDEN}\n')
    f.write('extern const signed char anti_prop_tab[];\nextern const signed char anti_cond_tab[];\n')
    f.write('extern const signed char anti_verb_tab[];\nextern const unsigned char cond_reverse_tab[];\n')
    f.write('extern const signed char colour_of_coord_tab[];\n')
    f.write('extern const unsigned int level_menu_to_id[];\n')
    f.write(f'#define NUM_LETTER_PIECES {len(letter_piece_list)}\n')
    f.write('extern const char * const letter_pieces[];\n')

with open(os.path.join(OUT, 'gamedata.c'), 'w') as f:
    f.write('#include "gamedata.h"\n')
    f.write('const signed char anti_prop_tab[] = {' + ','.join(map(str, anti_prop)) + '};\n')
    f.write('const signed char anti_cond_tab[] = {' + ','.join(map(str, anti_cond)) + '};\n')
    f.write('const signed char anti_verb_tab[] = {' + ','.join(map(str, anti_verb)) + '};\n')
    f.write('const unsigned char cond_reverse_tab[] = {' + ','.join(map(str, cond_reverse)) + '};\n')
    f.write('const signed char colour_of_coord_tab[] = {' + ','.join(map(str, colour_of_coord)) + '};\n')
    f.write('const unsigned int level_menu_to_id[] = {' + ','.join(map(str, order)) + '};\n')
    f.write('const char * const letter_pieces[] = {' + ','.join(json.dumps(x) for x in letter_piece_list) + '};\n')

# comment header with vocabulary for the report
with open(os.path.join(OUT, 'vocab.json'), 'w') as f:
    json.dump({'props': prop_names, 'verbs': verb_names, 'conds': cond_names,
               'tiles': registry, 'music': [m[0] for m in music_blobs],
               'sfx': [s[0] for s in sfx_ptrs]}, f, indent=1)
with open(os.path.join(OUT, 'levels.json'), 'w') as f:
    json.dump(level_blobs, f, indent=1, default=str)
with open(os.path.join(OUT, 'notes.json'), 'w') as f:
    json.dump(report['notes'], f, indent=1)

# Remove bank images from earlier, larger conversions; otherwise stale
# files can be mistaken for current generated data.
for stale in glob.glob(os.path.join(OUT, 'bank*.bin')):
    os.remove(stale)

for i, b in enumerate(banks):
    b += b'\x00' * (BANK_SIZE - len(b))
    with open(os.path.join(OUT, f'bank{i + 2:02d}.bin'), 'wb') as f:
        f.write(b)
with open(os.path.join(OUT, 'banks.s'), 'w') as f:
    for i in range(len(banks)):
        f.write(f'\t.module bank{i + 2}\n\t.area _BANK{i + 2}\n\t.incbin "gen/bank{i + 2:02d}.bin"\n')
with open(os.path.join(OUT, 'banks.mk'), 'w') as f:
    f.write('BANKS := ' + ' '.join(str(i + 2) for i in range(len(banks))) + '\n')
    f.write('BANKFLAGS := ' + ' '.join(f'-Wl-b_BANK{i + 2}=0x{i + 2:x}8000' for i in range(len(banks))) + '\n')
    f.write(f'CODEBANKFLAGS := -Wl-b_BANKP=0x{PARSER_BANK:x}8000 -Wl-b_BANKM=0x{MOVE_BANK:x}8000\n')

ok = sum(1 for l in level_blobs if not l['reason'])
print(f'levels ok={ok}/{len(level_blobs)} banks={len(banks)} rom~{(len(banks) + 2) * 16}KB')
print('excluded reasons:', collections.Counter(l['reason'] for l in level_blobs))
