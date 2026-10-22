#!/usr/bin/env python3
"""Parse the upstream Bab Be U (Love2D) data files.

Implements the same dynamic tile creation as utils.lua:getTile()/addTile():
  * "txt_<name>" tiles are synthesised from <name> (meta text, recursively)
  * "<name>n't"  tiles are synthesised from <name> (nt flag, metasprite)
  * old_names aliases are honoured (e.g. txt_text -> txt_txt, txt_n't -> txt_nt)
Sprites are composited exactly like game/scene.lua:drawUnit(): each sprite
layer is tinted (multiplied) by its palette colour unless painted[i] is
false, in which case the sprite's own greyscale is used.  Overlay sprites
are drawn untinted on top.
"""
import json, glob, os, re, base64, zlib, copy, collections
from PIL import Image

class BabData:
    def __init__(self, root):
        self.root = root
        self.tiles = {}            # name -> tile dict (after addTile defaults)
        self.by_old_name = {}
        self.sprites = {}          # sprite name -> PIL RGBA (lazy paths)
        self.sprite_paths = {}
        self.palettes = {}         # name -> list[35] of (r,g,b) 0..255
        self._load_tiles()
        self._load_sprite_index()
        self._load_palettes()
        self._parse_values()

    # ---------------------------------------------------------------- tiles
    def _load_tiles(self):
        for f in sorted(glob.glob(os.path.join(self.root, 'assets/tiles/**/*.json'), recursive=True)):
            for e in json.load(open(f, encoding='utf-8')):
                self._add_tile(e)

    def _add_tile(self, tile):
        tile = dict(tile)
        tile.setdefault('types', ['object'])
        tile.setdefault('painted', [True])
        tile.setdefault('rotate', False)
        tile.setdefault('portal', False)
        tile.setdefault('wobble', False)
        tile.setdefault('sprite_transforms', {})
        tile.setdefault('features', {})
        tile.setdefault('tags', [])
        tile.setdefault('alias', [])
        tile.setdefault('old_names', [])
        tile.setdefault('nt', False)
        tile.setdefault('meta', 0)
        if 'is_text' not in tile:
            tile['is_text'] = tile['name'].startswith('txt_') or tile['name'].startswith('letter_')
        tile.setdefault('convertible', True)
        if 'layer' not in tile:
            tile['layer'] = 20 if tile['is_text'] else 1
        if 'txtname' not in tile:
            tile['txtname'] = tile['name'][4:] if (tile['is_text'] and tile['name'].startswith('txt_')) else tile['name']
            if tile['name'].startswith('letter_'):
                tile['txtname'] = tile['name'][7:]
        self.tiles[tile['name']] = tile
        for old in tile['old_names']:
            self.by_old_name[old] = tile['name']
        return tile

    def get_tile(self, name, old=True):
        """Mirror of utils.lua getTile().  Returns tile dict or None."""
        if name in self.tiles:
            return self.tiles[name]
        if old and name in self.by_old_name:
            return self.tiles[self.by_old_name[name]]
        if name.endswith("n't"):
            base = self.get_tile(name[:-3], old)
            if base is None: return None
            t = copy.deepcopy(base)
            t['name'] = base['name'] + "n't"
            t['sprite'] = base.get('metasprite') or base['sprite']
            t['nt'] = True
            t['old_names'] = []
            return self._add_tile(t)
        if name.startswith('txt_'):
            base = self.get_tile(name[4:], old)
            if base is None: return None
            t = copy.deepcopy(base)
            t['name'] = 'txt_' + base['name']
            t['sprite'] = base.get('metasprite') or base['sprite']
            t['types'] = ['object']
            t.pop('thingify', None)
            t['txtname'] = 'txt_' + base['txtname'] if base['is_text'] else 'txt_' + base['name']
            # utils.lua: tile.txtname = "txt_" .. tile.txtname  (of the copy)
            t['txtname'] = 'txt_' + base['txtname']
            t['is_text'] = True
            t['meta'] = base.get('meta', 0) + 1
            t['old_names'] = []
            if t['layer'] < 20: t['layer'] = 20
            return self._add_tile(t)
        return None

    # -------------------------------------------------------------- sprites
    def _load_sprite_index(self):
        base = os.path.join(self.root, 'assets/sprites')
        for p in glob.glob(os.path.join(base, '**/*.png'), recursive=True):
            rel = os.path.relpath(p, base)[:-4].replace(os.sep, '/')
            self.sprite_paths[rel] = p

    def sprite(self, name):
        if name not in self.sprites:
            p = self.sprite_paths.get(name)
            self.sprites[name] = Image.open(p).convert('RGBA') if p else None
        return self.sprites[name]

    def tile_sprite_name(self, sprite_name, tile, frame=None):
        """Mirror of getTileSprite(): wobble frames are preferred when asked."""
        tries = []
        if frame is not None and tile.get('wobble'):
            tries.append(f"{sprite_name}_{frame}")
        tries.append(sprite_name)
        for t in tries:
            if t in self.sprite_paths:
                return t
        return 'wat'

    def _load_palettes(self):
        for p in sorted(glob.glob(os.path.join(self.root, 'assets/palettes/*.png'))):
            name = os.path.basename(p)[:-4]
            im = Image.open(p).convert('RGBA')
            w, h = im.size
            px = list(im.getdata())
            self.palettes[name] = [(r, g, b) for (r, g, b, a) in px]  # index = x + y*w

    # --------------------------------------------------------------- values
    def _parse_values(self):
        src = open(os.path.join(self.root, 'values.lua'), encoding='utf-8', errors='ignore').read()
        # main_palette_for_colour = { blacc = {0, 4}, ... }
        m = re.search(r'main_palette_for_colour\s*=\s*\{(.*?)\n\}', src, re.S)
        self.colour_coord = {}
        if m:
            for name, x, y in re.findall(r'(\w+)\s*=\s*\{(\d+),\s*(\d+)\}', m.group(1)):
                self.colour_coord[name] = (int(x), int(y))
        # colour_for_palette[x][y] = "name"
        self.colour_for_coord = {}
        for x, y, name in re.findall(r'colour_for_palette\[(\d+)\]\[(\d+)\]\s*=\s*"(\w+)"', src):
            self.colour_for_coord[(int(x), int(y))] = name
        # anti_word_replacements = { a = "b", ["x"] = "y", }
        m = re.search(r'anti_word_replacements\s*=\s*\{(.*?)\n\}', src, re.S)
        self.anti_replacements = {}
        if m:
            self.anti_replacements = {}
            for mm in re.finditer(r'(?:\[?"?([A-Za-z0-9_...:)/\']+)"?\]?)\s*=\s*"([^"]+)"', m.group(1)):
                self.anti_replacements[mm.group(1)] = mm.group(2)
        m = re.search(r'anti_word_reverses\s*=\s*\{(.*?)\n\}', src, re.S)
        self.anti_reverses = set()
        if m:
            for mm in re.finditer(r'(?:\[?"?([A-Za-z0-9_...:)/\']+)"?\]?)\s*=\s*true', m.group(1)):
                self.anti_reverses.add(mm.group(1))

    # --------------------------------------------------------------- levels
    _unit_re = re.compile(r'\{tile="((?:[^"\\]|\\.)*)",x=(-?\d+),id=(\d+),y=(-?\d+),special=\{(.*?)\},dir=(\d+)\}')

    def parse_level(self, path):
        d = json.load(open(path, encoding='utf-8', errors='ignore'))
        raw = base64.b64decode(d['map'])
        if d.get('compression') == 'zlib':
            raw = zlib.decompress(raw)
        text = raw.decode('utf-8', errors='ignore')
        units = []
        for m in self._unit_re.finditer(text):
            name, x, uid, y, special, direction = m.groups()
            sp = {}
            if special:
                for kv in re.finditer(r'(\w+)=(?:"((?:[^"\\]|\\.)*)"|(-?\d+))', special):
                    sp[kv.group(1)] = kv.group(2) if kv.group(2) is not None else int(kv.group(3))
            units.append({'tile': name.replace('\\"', '"'), 'x': int(x), 'y': int(y),
                          'id': int(uid), 'dir': int(direction), 'special': sp})
        d['_units'] = units
        d['_path'] = path
        return d

    def all_levels(self):
        out = []
        for p in sorted(glob.glob(os.path.join(self.root, 'officialworlds/**/*.bab'), recursive=True)):
            out.append((os.path.relpath(p, os.path.join(self.root, 'officialworlds')), p))
        return out

# --------------------------------------------------------------------------
# Sprite compositing -> 32x32 arrays of colour-source ids.
# A colour source is a tuple:
#   ('pal', coord_index 0..34, grey 0..255)  tinted by level palette colour
#   ('fix', sms_byte)                        fixed SMS colour (overlays etc.)
# Source 0 = transparent.
# --------------------------------------------------------------------------

def sms_quantise(r, g, b):
    """RGB 0..255 -> SMS colour byte --bbggrr (2 bits per channel)."""
    def q(v): return min(3, (v * 4) // 256 if v < 255 else 3) if v < 32 else min(3, int(v / 85.0 + 0.5))
    # round to nearest of 0,85,170,255
    def q2(v): return max(0, min(3, int(v / 85.0 + 0.5)))
    return q2(r) | (q2(g) << 2) | (q2(b) << 4)

def sms_rgb(byte):
    return ((byte & 3) * 85, ((byte >> 2) & 3) * 85, ((byte >> 4) & 3) * 85)

class ShapeBuilder:
    def __init__(self, bab):
        self.bab = bab
        self.cache = {}

    def _layer_pixels(self, sprite_name, tile, frame):
        bab = self.bab
        name = bab.tile_sprite_name(sprite_name, tile, frame)
        im = bab.sprite(name)
        if im is None:
            im = bab.sprite('wat')
        if im.size != (32, 32):
            im = im.resize((32, 32), Image.NEAREST)
        return list(im.getdata())

    def composite(self, tile, frame=None, inactive=False):
        """Return (pixels, sources) where pixels is 32x32 list of source
        indices (0=transparent) and sources[i] describes source i+1."""
        key = (tile['name'], frame, inactive)
        if key in self.cache:
            return self.cache[key]
        bab = self.bab
        sprites = list(tile['sprite'])
        transforms = tile.get('sprite_transforms') or {}
        if inactive and 'inactive' in transforms:
            sprites = [transforms['inactive']]
        colors = tile.get('color') or [[0, 3]]
        painted = (tile.get('painted') or [True])
        sources = []
        src_index = {}
        def intern(src):
            if src not in src_index:
                src_index[src] = len(sources) + 1
                sources.append(src)
            return src_index[src]
        out = [0] * (32 * 32)
        layers = []
        for i, sname in enumerate(sprites):
            coord = colors[i] if i < len(colors) else colors[-1]
            is_painted = painted[i] if i < len(painted) else True
            layers.append((self._layer_pixels(sname, tile, frame), coord, is_painted))
        for ov in (tile.get('overlay') or []):
            layers.append((self._layer_pixels(ov, tile, None), None, False))
        default_pal = bab.palettes['default']
        for pixels, coord, is_painted in layers:
            for idx, (r, g, b, a) in enumerate(pixels):
                if a < 128:
                    continue
                if is_painted and coord is not None:
                    if len(coord) == 3:  # direct RGB tint (e.g. pride flags)
                        out[idx] = intern(('fix', sms_quantise(r * coord[0] // 255, g * coord[1] // 255, b * coord[2] // 255)))
                    elif r == g == b:
                        out[idx] = intern(('pal', coord[0] + coord[1] * 7, r))
                    else:
                        pr, pg, pb = default_pal[coord[0] + coord[1] * 7]
                        out[idx] = intern(('fix', sms_quantise(r * pr // 255, g * pg // 255, b * pb // 255)))
                else:
                    if r == g == b:
                        out[idx] = intern(('fix', sms_quantise(r, g, b)))
                    else:
                        out[idx] = intern(('fix', sms_quantise(r, g, b)))
        result = (out, sources)
        self.cache[key] = result
        return result

def downscale_16(pixels32, min_opaque=2):
    """32x32 source-index image -> 16x16 by 2x2 coverage.

    Taking a pixel when *any* source pixel was opaque dilates every
    stroke and turns the small word sprites into unreadable blobs.
    Objects use a 2-of-4 threshold; text uses 3-of-4 (supplied by the
    converter), preserving holes and separations between letters.
    The winning source is the most frequent opaque source in the block.
    """
    out = [0] * (16 * 16)
    for y in range(16):
        for x in range(16):
            block = [pixels32[(2 * y + dy) * 32 + 2 * x + dx] for dy in (0, 1) for dx in (0, 1)]
            opaque = [v for v in block if v]
            if len(opaque) >= min_opaque:
                out[y * 16 + x] = collections.Counter(opaque).most_common(1)[0][0]
    return out

def pack_shape(pixels16):
    """16x16 indices (<=15) -> 128 bytes, 2 pixels per byte."""
    out = bytearray()
    for i in range(0, 256, 2):
        out.append(((pixels16[i] & 15) << 4) | (pixels16[i + 1] & 15))
    return bytes(out)

def rotate_shape_cw(pixels16):
    out = [0] * 256
    for y in range(16):
        for x in range(16):
            out[x * 16 + (15 - y)] = pixels16[y * 16 + x]
    return out
