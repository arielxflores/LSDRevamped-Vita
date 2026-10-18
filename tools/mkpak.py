#!/usr/bin/env python3
"""Pack the baked data into the archives the Vita reads from ux0:data/lsdr.

usage: mkpak.py --data <data dir> --out dreams.pak

The packs are kept under ~24 MB each: dreams.pak, then dreams2.pak, dreams3.pak... as needed.

  'LSDP' u32 version=2, u32 count, u32 reserved
  count x { char name[48]; u32 offset; u32 packed size; u32 size; u32 crc32 }
  entry data: packed size bit 31 set = stored as it is; bit 30 = LZ4: u32 nblocks, u32 block size
  (unpacked, 256 KB), u32 packed size of each block, then the blocks (each an independent LZ4 block,
  so the game can unpack the first ones while the rest is still being read); the crc32 is of the
  entry data as stored. (Version 1, still written for the movie packs, had single LZ4 blocks or zlib
  streams and the crc32 of the unpacked data; the game reads both.)

--movies packs data/movies/*.mpg but the openings (stored: MPEG does not compress) into movies1.pak,
movies2.pak... (OUT names the base: --out build/movies.pak) instead; --intro the openings
(movies/OPENING*.mpg, for the intro) into intro1.pak...; --music the Revamped soundtrack
(data/music/*.ogg) into music1.pak...
"""
import argparse
import concurrent.futures
import ctypes
import os
import struct
import subprocess
import zlib

STORED = 0x80000000  # packed size flags
LZ4 = 0x40000000

FILES = ('dream.bin', 'entities.bin', 'models.bin', 'footsteps.bin', 'envfx.bin', 'tex_A.bin', 'tex_B.bin', 'tex_C.bin', 'tex_D.bin',
         'BGA.SEQ', 'BGB.SEQ', 'BGC.SEQ', 'BGD.SEQ', 'BGE.SEQ')


LIMIT = 24_000_000  # bytes per pack file


_LZ4 = None


def lz4_compress(raw):
    """LZ4 HC block at the maximum level: the python lz4 module when it is installed (same output),
    otherwise tools/c/liblz4pack.so (lz4 1.9.4, built from tools/c/lz4pack.c)"""
    global _LZ4
    if _LZ4 is None:
        try:
            import lz4.block
            _LZ4 = lz4.block
        except ImportError:
            pass
    if _LZ4 is not None and hasattr(_LZ4, 'compress'):
        packed = _LZ4.compress(raw, mode='high_compression', compression=12, store_size=False)
        if _LZ4.decompress(packed, uncompressed_size=len(raw)) != raw:
            raise SystemExit('lz4 round trip failed')
        return packed
    if _LZ4 is None:
        so = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'c', 'liblz4pack.so')
        if not os.path.exists(so):
            d = os.path.dirname(so)
            subprocess.run(['gcc', '-O2', '-shared', '-fPIC', '-I' + os.path.join(d, '..', '..', 'src'), '-o', so,
                            os.path.join(d, 'lz4pack.c'), os.path.join(d, 'lz4hc.c'),
                            os.path.join(d, '..', '..', 'src', 'lz4.c')], check=True)
        _LZ4 = ctypes.CDLL(so)
    cap = _LZ4.lz4bound(len(raw))
    out = ctypes.create_string_buffer(cap)
    n = _LZ4.lz4pack(raw, len(raw), out, cap)
    if n <= 0:
        raise SystemExit('lz4 failed')
    packed = out.raw[:n]
    back = ctypes.create_string_buffer(max(1, len(raw)))
    if _LZ4.lz4unpack(packed, n, back, len(raw)) != len(raw) or back.raw[:len(raw)] != raw:
        raise SystemExit('lz4 round trip failed')
    return packed


BLOCK = 256 * 1024  # unpacked bytes per LZ4 block


def pack_one(path):
    """(raw, packed, flag): LZ4 blocks (fast to unpack on the Vita), or stored when that does not help"""
    raw = open(path, 'rb').read()
    blocks = [lz4_compress(raw[i:i + BLOCK]) for i in range(0, len(raw), BLOCK)]
    z = struct.pack('<II', len(blocks), BLOCK) + struct.pack('<%dI' % len(blocks), *(len(b) for b in blocks))
    z += b''.join(blocks)
    if not raw or len(z) >= len(raw):
        return raw, raw, STORED
    return raw, z, LZ4


def store_one(path):
    raw = open(path, 'rb').read()
    return raw, raw, STORED


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--movies', action='store_true')
    ap.add_argument('--intro', action='store_true')
    ap.add_argument('--music', action='store_true')
    a = ap.parse_args()
    if a.movies or a.intro or a.music:
        # several packs of at most ~24 MB (moviesN.pak / introN.pak / musicN.pak, N = 1, 2, ...): files that
        # size are easier to move around. Stored: MPEG and Vorbis do not compress.
        if a.music:
            names = ['music/' + f for f in sorted(os.listdir(os.path.join(a.data, 'music'))) if f.endswith('.ogg')]
        else:
            names = ['movies/' + f for f in sorted(os.listdir(os.path.join(a.data, 'movies'))) if f.endswith('.mpg')]
            # (the intro's openings go in packs of their own, so the in-game videos' packs stay as they were)
            names = [n for n in names if n.startswith('movies/OPENING') == a.intro]
        groups, cur, size = [], [], 0
        for n in names:
            sz = os.path.getsize(os.path.join(a.data, n))
            if cur and size + sz > LIMIT:
                groups.append(cur)
                cur, size = [], 0
            cur.append(n)
            size += sz
        if cur:
            groups.append(cur)
        base = a.out[:-4] if a.out.endswith('.pak') else a.out
        for i, g in enumerate(groups):
            a.out = '%s%d.pak' % (base, i + 1)
            write(a, g, [store_one(os.path.join(a.data, n)) for n in g], version=1)
        return
    names = ['journal.bin', 'greyman.bin', 'audio.bin', 'sky.bin', 'specialdays.bin']
    if os.path.exists(os.path.join(a.data, 'music.bin')):
        names.append('music.bin')  # (the Revamped soundtrack's list; its songs are in musicN.pak)
    names += ['snd/' + f for f in sorted(os.listdir(os.path.join(a.data, 'snd')))]
    names += ['special/' + f for f in sorted(os.listdir(os.path.join(a.data, 'special')))]
    names += ['ui.bin'] + ['ui/' + f for f in sorted(os.listdir(os.path.join(a.data, 'ui')))]
    sd = os.path.join(a.data, 'scripts')
    for dp, _dn, fn in sorted(os.walk(sd)):
        for f in sorted(fn):
            if f.endswith('.lua'):
                names.append(os.path.relpath(os.path.join(dp, f), a.data).replace(os.sep, '/'))
    for d in sorted(os.listdir(a.data)):
        if d.startswith('stg') and os.path.isdir(os.path.join(a.data, d)):
            names += ['%s/%s' % (d, f) for f in FILES if os.path.exists(os.path.join(a.data, d, f))]
    with concurrent.futures.ProcessPoolExecutor() as ex:
        packed = list(ex.map(pack_one, [os.path.join(a.data, n) for n in names]))
    # at most ~24 MB per pack: OUT, then OUT2, OUT3... (dreams.pak, dreams2.pak, ...)
    groups, cur, size = [], [], 0
    for n, pk in zip(names, packed):
        if cur and size + len(pk[1]) > LIMIT:
            groups.append(cur)
            cur, size = [], 0
        cur.append((n, pk))
        size += len(pk[1])
    groups.append(cur)
    base = a.out[:-4] if a.out.endswith('.pak') else a.out
    for i, g in enumerate(groups):
        a.out = base + '.pak' if i == 0 else '%s%d.pak' % (base, i + 1)
        write(a, [n for n, _ in g], [pk for _, pk in g])


def write(a, names, packed, version=2):
    hdr = 16 + 64 * len(names)
    off = hdr
    table = bytearray()
    for n, (raw, z, flag) in zip(names, packed):
        nb = n.encode()
        assert len(nb) < 48
        assert version == 2 or flag == STORED
        table += nb.ljust(48, b'\0') + struct.pack('<IIII', off, len(z) | flag, len(raw), zlib.crc32(z) & 0xFFFFFFFF)
        off += len(z)
    with open(a.out, 'wb') as f:
        f.write(b'LSDP' + struct.pack('<III', version, len(names), 0))
        f.write(table)
        for raw, z, _f in packed:
            f.write(z)
    total = sum(len(r) for r, _z, _f in packed)
    print('%d files, %.1f MB -> %.1f MB' % (len(names), total / 1e6, os.path.getsize(a.out) / 1e6))


if __name__ == '__main__':
    main()
