"""Readers for LSD: Dream Emulator data formats (LBD, TMD, TIX, TIM).

Ported from figglewatts/libLSD (MIT) and LSDR's LibLSDUnity.cs (MIT), keeping
the same conventions LSD Revamped uses so the Vita build looks the same.
"""
import struct

# ---------------------------------------------------------------- helpers


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def seek(self, p):
        self.p = p

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]
        self.p += 2
        return v

    def s16(self):
        v = struct.unpack_from('<h', self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]
        self.p += 4
        return v

    def s32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]
        self.p += 4
        return v


# ---------------------------------------------------------------- TMD

# Packet field layouts, keyed by identifier ((mode & ~2) << 8 | flag), exactly
# as libLSD's TMDPackets.cs reads them. Tokens:
#   uvc = u,v + CBA   uvt = u,v + TSB   uv = u,v + pad16
#   c = r,g,b,pad     n = normal idx    p = vertex idx   x16 = pad16
_TEX3 = ['uvc', 'uvt', 'uv']
_TEX4 = ['uvc', 'uvt', 'uv', 'uv']
PACKETS = {
    # triangles
    0x2101: ['c', 'p', 'p', 'p', 'x16'],
    0x2501: _TEX3 + ['c', 'p', 'p', 'p', 'x16'],
    0x3101: ['c', 'c', 'c', 'p', 'p', 'p', 'x16'],
    0x3501: _TEX3 + ['c', 'c', 'c', 'p', 'p', 'p', 'x16'],
    0x2000: ['c', 'n', 'p', 'p', 'p'],
    0x2004: ['c', 'c', 'c', 'n', 'p', 'p', 'p'],
    0x2400: _TEX3 + ['n', 'p', 'p', 'p'],
    0x3000: ['c', 'n', 'p', 'n', 'p', 'n', 'p'],
    0x3004: ['c', 'c', 'c', 'n', 'p', 'n', 'p', 'n', 'p'],
    0x3400: _TEX3 + ['n', 'p', 'n', 'p', 'n', 'p'],
    # quads
    0x2901: ['c', 'p', 'p', 'p', 'p'],
    0x2D01: _TEX4 + ['c', 'p', 'p', 'p', 'p'],
    0x3901: ['c', 'c', 'c', 'c', 'p', 'p', 'p', 'p'],
    0x3D01: _TEX4 + ['c', 'c', 'c', 'c', 'p', 'p', 'p', 'p'],
    0x2800: ['c', 'n', 'p', 'p', 'p', 'p', 'x16'],
    0x2804: ['c', 'c', 'c', 'c', 'n', 'p', 'p', 'p', 'p', 'x16'],
    0x2C00: _TEX4 + ['n', 'p', 'p', 'p', 'p', 'x16'],
    0x3800: ['c', 'n', 'p', 'n', 'p', 'n', 'p', 'n', 'p'],
    0x3804: ['c', 'c', 'c', 'c', 'n', 'p', 'n', 'p', 'n', 'p', 'n', 'p'],
    0x3C00: _TEX4 + ['n', 'p', 'n', 'p', 'n', 'p', 'n', 'p'],
    # lines (not drawn)
    0x4001: ['c', 'p', 'p'],
    0x5001: ['c', 'c', 'p', 'p'],
}

OPT_ALPHA = 2
OPT_TEXTURED = 4
OPT_QUAD = 8
FLAG_DOUBLE_SIDED = 2
TYPE_POLYGON = 1


class Prim:
    __slots__ = ('mode', 'flag', 'ptype', 'options', 'uvs', 'tsb', 'cba',
                 'colors', 'verts', 'normals')


def read_packet(r):
    """Read one primitive packet. Returns Prim, or None for unknown types
    (libLSD also skips those; the packet length then comes from ilen)."""
    start = r.p
    olen = r.u8()
    ilen = r.u8()
    flag = r.u8()
    mode = r.u8()
    ident = ((mode & ~2) << 8) | flag
    layout = PACKETS.get(ident)
    prim = Prim()
    prim.mode, prim.flag = mode, flag
    prim.ptype = (mode >> 5) & 7
    prim.options = mode & 0x1F
    prim.uvs, prim.colors, prim.verts, prim.normals = [], [], [], []
    prim.tsb = prim.cba = 0
    if layout is None:
        # unknown: skip by ilen (words), like the PSX does
        r.seek(start + 4 + ilen * 4)
        return None
    for tok in layout:
        if tok == 'uvc':
            u, v = r.u8(), r.u8()
            prim.cba = r.u16()
            prim.uvs.append((u, v))
        elif tok == 'uvt':
            u, v = r.u8(), r.u8()
            prim.tsb = r.u16()
            prim.uvs.append((u, v))
        elif tok == 'uv':
            u, v = r.u8(), r.u8()
            r.u16()
            prim.uvs.append((u, v))
        elif tok == 'c':
            cr, cg, cb = r.u8(), r.u8(), r.u8()
            r.u8()
            prim.colors.append((cr / 255.0, cg / 255.0, cb / 255.0))
        elif tok == 'n':
            prim.normals.append(r.u16())
        elif tok == 'p':
            prim.verts.append(r.u16())
        elif tok == 'x16':
            r.u16()
    return prim


class TMDObject:
    __slots__ = ('verts', 'prims', 'scale')


def read_tmd(data, base):
    r = Reader(data, base)
    magic = r.u32()
    flags = r.u32()
    nobj = r.u32()
    if magic != 0x41:
        raise ValueError('bad TMD magic %x at %x' % (magic, base))
    fixp = flags & 1
    top = r.p
    objs = []
    for _ in range(nobj):
        vaddr, nv, naddr, nn, paddr, nprim, scale = struct.unpack_from('<IIIIIIi', data, r.p)
        r.p += 28
        o = TMDObject()
        o.scale = scale
        pr = Reader(data, paddr if fixp else paddr + top)
        o.prims = [read_packet(pr) for _ in range(nprim)]
        vr = Reader(data, vaddr if fixp else vaddr + top)
        verts = []
        for _ in range(nv):
            x, y, z, _pad = struct.unpack_from('<hhhh', data, vr.p)
            vr.p += 8
            verts.append((x, y, z))
        o.verts = verts
        objs.append(o)
    return objs


# ---------------------------------------------------------------- LBD


class LBDTile:
    __slots__ = ('draw', 'solid', 'type', 'footstep', 'dir', 'height', 'extra')


def _read_tile(r, addr_off, extra_top):
    t = LBDTile()
    t.draw = r.u8() == 1
    t.solid = r.u8() == 1
    t.type = r.u16()
    t.footstep = r.u8()
    t.dir = r.u8()
    t.height = r.s16()
    raw = r.u32()
    t.extra = -1 if raw == 0 else ((raw + addr_off) - extra_top) // 0xC
    return t


class LBD:
    pass


def read_lbd(data):
    r = Reader(data)
    lbd = LBD()
    lbd.version = r.u16()
    lbd.has_mml = r.u16() == 1
    addr_off = r.u32()
    tmd_off = r.u32()
    r.u32()  # tmd length
    lbd.mml_off = r.u32()
    lbd.mml_len = r.u32()
    r.u16()
    n_extra = r.u16()
    w = r.u16()
    h = r.u16()
    lbd.w, lbd.h = w, h
    extra_top = 0x20 + w * h * 0xC
    # file order is y-major (outer y, inner x); libLSD stores it at
    # Underlying[x*H + y] and LSDR's importer enumerates Underlying in order,
    # deriving (x = idx / W, y = idx % H). Keep that exact mapping.
    tiles_xy = {}
    for y in range(h):
        for x in range(w):
            tiles_xy[(x, y)] = _read_tile(r, addr_off, extra_top)
    lbd.layout = [tiles_xy[(idx // h, idx % h)] for idx in range(w * h)]
    lbd.extra = [_read_tile(r, addr_off, extra_top) for _ in range(n_extra)]
    lbd.tiles = read_tmd(data, tmd_off + addr_off)
    return lbd


# ---------------------------------------------------------------- TIM / TIX


def _c16(v):
    """PS1 15-bit colour -> (r,g,b,a) 0..255, with libLSD's alpha rules."""
    r5, g5, b5 = v & 31, (v >> 5) & 31, (v >> 10) & 31
    stp = (v >> 15) & 1
    black = r5 == 0 and g5 == 0 and b5 == 0
    a5 = 31
    if stp:
        if not black:
            a5 = max(r5, g5, b5)
    elif black:
        a5 = 0

    def e(c):
        return int(round(c / 31.0 * 255.0))
    return (e(r5), e(g5), e(b5), e(a5))


def read_tim(data, pos):
    """Returns dict(x, y, w, h, pixels=[(r,g,b,a)] row-major top-down)."""
    r = Reader(data, pos)
    tid = r.u32()
    flags = r.u32()
    if tid & 0xFF != 0x10:
        raise ValueError('bad TIM magic')
    mode = flags & 7
    clut = None
    if (flags >> 3) & 1:
        clen = r.u32()
        r.u16(); r.u16(); r.u16(); r.u16()
        n = 16 if mode == 0 else 256
        ncl = (clen - 12) // (n * 2)
        clut = [[_c16(r.u16()) for _ in range(n)] for _ in range(ncl)]
    r.u32()  # pixel data length
    px = r.u16()
    py = r.u16()
    pw = r.u16()
    ph = r.u16()
    words = struct.unpack_from('<%dH' % (pw * ph), data, r.p)
    pixels = []
    if mode == 0:
        w = pw * 4
        pal = clut[0]
        for d in words:
            for j in range(4):
                pixels.append(pal[(d >> (4 * j)) & 0xF])
    elif mode == 1:
        w = pw * 2
        pal = clut[0]
        for d in words:
            pixels.append(pal[d & 0xFF])
            pixels.append(pal[(d >> 8) & 0xFF])
    elif mode == 2:
        w = pw
        pixels = [_c16(d) for d in words]
    elif mode == 3:
        w = (pw // 3) * 2
        for i in range(0, len(words), 3):
            d1, d2, d3 = words[i], words[i + 1], words[i + 2]
            pixels.append((d1 & 0xFF, (d1 >> 8) & 0xFF, d2 & 0xFF, 255))
            pixels.append(((d2 >> 8) & 0xFF, d3 & 0xFF, (d3 >> 8) & 0xFF, 255))
    else:
        raise ValueError('mixed TIM mode unsupported')
    return dict(x=px, y=py, w=w, h=ph, mode=mode, pixels=pixels)


def read_tix(data):
    r = Reader(data)
    nch = r.u32()
    offs = [r.u32() for _ in range(nch)]
    tims = []
    for off in offs:
        cr = Reader(data, off)
        n = cr.u32()
        toffs = [cr.u32() for _ in range(n)]
        for t in toffs:
            tims.append(read_tim(data, off + t))
    return tims


# LSDR's virtual VRAM (LibLSDUnity.VRAM_WIDTH/HEIGHT). We keep the same
# coordinate maths but store a 2048-wide texture; nothing LSD draws reaches
# past x=1536 in this layout.
VRAM_W_LSDR = 2056
VRAM_H = 512
ATLAS_W = 2048


def build_vram_atlas(tims):
    """Bottom-up RGBA atlas (row 0 = Unity's bottom row = GL's v=0)."""
    W, H = ATLAS_W, VRAM_H
    buf = bytearray(b'\xff' * (W * H * 4))
    for t in tims:
        ax = (t['x'] - 320) * 2
        ay = 512 - t['y'] - t['h']        # Unity bottom-up placement
        w, h = t['w'], t['h']
        px = t['pixels']
        for row in range(h):
            # TimDataToColors(flip=true): source row (h-1-k) goes to dest row ay+k
            src = h - 1 - row
            dy = ay + row
            if dy < 0 or dy >= H:
                continue
            for col in range(w):
                dx = ax + col
                if dx < 0 or dx >= W:
                    continue
                c = px[src * w + col]
                o = (dy * W + dx) * 4
                buf[o:o + 4] = bytes(c)
    return bytes(buf)


def prim_uv(prim, i):
    """LSDR's VRAM-atlas UV for vertex i of a textured primitive."""
    page = prim.tsb & 0x1F
    tpx = page % 16 * 128 - 640
    tpy = 256 if page < 16 else 0
    u, v = prim.uvs[i]
    vx = tpx + u
    vy = tpy + (256 - v)
    # same texel centre as LSDR, expressed for our 2048-wide atlas
    return ((vx + 0.5) / ATLAS_W, (vy - 0.5) / VRAM_H)


def prim_uv_lsdr(prim, i):
    """The UV exactly as LibLSDUnity computes it (2056-wide VRAM, float32), for UV material overrides."""
    import numpy as np
    page = prim.tsb & 0x1F
    tpx = page % 16 * 128 - 640
    tpy = 256 if page < 16 else 0
    u, v = prim.uvs[i]
    vx = tpx + u
    vy = tpy + (256 - v)
    return (np.float32((vx + 0.5) / VRAM_W_LSDR), np.float32((vy - 0.5) / VRAM_H))
