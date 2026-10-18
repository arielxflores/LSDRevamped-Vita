#!/usr/bin/env python3
"""Bake the Grey Man (ETC/SYMSPY.MOM as placed by LSDR's Greyman.prefab) into one
static mesh in the Grey Man's own space, written in the models.bin format.

usage: greyman.py --project <LSDR> --out data/greyman.bin
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake        # noqa: E402
import mom as momlib  # noqa: E402

ROOT_ROT = (0.0, 0.70710677, -0.7071068, 0.0)   # SYMSPY instance rotation in Greyman.prefab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    data = open(os.path.join(a.project, 'Assets/Resources/Original/ETC/SYMSPY.MOM'), 'rb').read()
    m = momlib.read_mom(data, 0)
    root, _clips = momlib.import_mom(m)
    tmd = m['tmd']
    batches = {0: bake.Batch(0, 0), 2: bake.Batch(2, 2)}

    def walk(node, parent_m):
        mtx = bake.mmul(parent_m, bake.trs(node.pos, node.rot, node.scale))
        if node.mesh >= 0:
            opaque, alpha = bake.tmd_object_tris(tmd[node.mesh])[:2]
            for kind, tris in ((0, opaque), (2, alpha)):
                for tri in tris:
                    batches[kind].add_tri(tuple((bake.xform(mtx, v[0]), v[1], v[2]) for v in tri))
        for c in node.children:
            walk(c, mtx)

    walk(root, bake.trs((0.0, 0.0, 0.0), ROOT_ROT, (1.0, 1.0, 1.0)))
    used = [b for b in batches.values() if b.idx]
    out = bytearray(b'LSDM' + struct.pack('<II', 1, 1))
    out += b'greyman'.ljust(32, b'\0')
    out += struct.pack('<II', 1, len(used))
    for b in used:
        out += struct.pack('<III6f', b.alpha, len(b.verts), len(b.idx), *b.lo, *b.hi)
        for (p, uv, col) in b.verts:
            out += struct.pack('<5f4B', *p, *uv, *col)
        out += struct.pack('<%dH' % len(b.idx), *b.idx)
        if len(b.idx) % 2:
            out += b'\0\0'
    out += struct.pack('<I', 0)   # no clips
    with open(a.out, 'wb') as f:
        f.write(out)
    lo = [min(b.lo[i] for b in used) for i in range(3)]
    hi = [max(b.hi[i] for b in used) for i in range(3)]
    print('grey man: %d tris, bounds %s %s' % (sum(len(b.idx) // 3 for b in used), lo, hi))


if __name__ == '__main__':
    main()
