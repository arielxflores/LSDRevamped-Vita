#!/usr/bin/env python3
"""Write journal.bin: the dream journal LSD Revamped plays through.

usage: journal.py --project <LSDRevamped/LSDR> [--journal <asset>] --out <data dir>

  'LSDJ' u32 version
  u32 ndreams, per dream: char[16] data directory (stgNN), char[64] name,
                          u32 flags (1 linkable, 2 first day, 4 grey man, 8 environment effects),
                          i32 environment offset
  361 x i8: GraphSpawnMap (19x19, row = upper + 9, column = dynamic + 9), journal index or -1
"""
import argparse
import json
import os
import tempfile
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unityyaml       # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--journal', default='Assets/Resources/Original/OriginalDreams.asset')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    idx_file = os.path.join(tempfile.gettempdir(), 'lsd_guid_index_j%d.txt' % os.getpid())
    unityyaml.build_guid_index(proj, idx_file)
    guids = unityyaml.GuidIndex(idx_file, proj)
    os.remove(idx_file)

    j = unityyaml.load_single(os.path.join(proj, a.journal))
    dreams = []
    for ref in j['Dreams']:
        p = guids.path(ref['guid'])
        d = unityyaml.load_single(p)
        m = re.search(r'(STG\d\d)', p)
        flags = ((1 if int(d.get('Linkable', 0)) else 0) | (2 if int(d.get('FirstDay', 0)) else 0) |
                 (4 if int(d.get('GreyMan', 0)) else 0) | (8 if int(d.get('HasEnvironmentEffects', 1)) else 0))
        dreams.append(dict(dir=m.group(1).lower(), name=d.get('Name', ''), flags=flags,
                           env_offset=int(d.get('EnvironmentOffset', 0)), guid=ref['guid']))
    gidx = {d['guid']: i for i, d in enumerate(dreams)}

    sm = unityyaml.load_single(guids.path(j['GraphSpawnMap']['guid']))
    pool = [gidx.get((e.get('Dream') or {}).get('guid'), -1) for e in sm['Dreams']]
    raw = bytes.fromhex(sm['_graph'])
    vals = struct.unpack('<361i', raw)
    graph = [pool[v] if 0 <= v < len(pool) else -1 for v in vals]

    out = bytearray(b'LSDJ' + struct.pack('<I', 1))
    out += struct.pack('<I', len(dreams))
    for d in dreams:
        n = d['name'].encode('utf-8')[:63]
        out += d['dir'].encode()[:15].ljust(16, b'\0') + n.ljust(64, b'\0')
        out += struct.pack('<Ii', d['flags'], d['env_offset'])
    out += struct.pack('<361b', *graph)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'journal.bin'), 'wb') as f:
        f.write(out)
    rep = dict(name=j.get('Name'), dreams=[(d['dir'], d['name'], d['flags'], d['env_offset']) for d in dreams],
               graph_rows=[''.join('%x' % g if g >= 0 else '.' for g in graph[r * 19:(r + 1) * 19])
                           for r in range(18, -1, -1)])
    with open(os.path.join(a.out, 'journal.json'), 'w') as f:
        json.dump(rep, f, indent=1)
    print(json.dumps(rep, indent=1))


if __name__ == '__main__':
    main()
