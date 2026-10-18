#!/usr/bin/env python3
"""Audio data for the Vita build.

usage: audiodata.py --project <LSDRevamped/LSDR> --data <data dir>

Writes
  data/audio.bin           'LSDA' v1: the sound banks to load, the original soundtrack table
                           (OriginalSongLibrary: per dream, per SongStyle, the songs as SEQ + bank),
                           the link sound and the LBD footstep index (LBDFootstepIndex.asset)
  data/stgNN/footsteps.bin 'LSDF' v1: each LBD chunk's position and tile footstep bytes, and the
                           chunk every collision triangle of dream.bin came from (for the footstep
                           under the player)
  data/snd/*.VH|VB, data/stgNN/BG?.SEQ   the original sound data, copied

Sound clips are VAG samples of the SE bank (LSDR's SND/LoadedSamples/SE/SampleN.wav).
"""
import argparse
import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake         # noqa: E402
import lsdfmt       # noqa: E402
import unityyaml    # noqa: E402

BANKS = ['SE', 'STANDERD', 'AMBIENT', 'LOVELY', 'HUMAN', 'ELECTRO', 'ETHNOVA', 'CARTOON']
# SongStyle enum order and the DreamOriginalSongs field for each
STYLES = [('Standard', 'StandardSongs'), ('Ambient', 'AmbientSongs'), ('Lovely', 'LovelySongs'),
          ('Human', 'HumanSongs'), ('Electro', 'ElectroSongs'), ('Ethnova', 'EthnovaSongs'),
          ('Cartoon', 'CartoonSongs')]
BG = ['BGA', 'BGB', 'BGC', 'BGD', 'BGE']


def se_sample(guids, ref):
    """clip reference -> SE sample number (-1 = no clip)"""
    g = (ref or {}).get('guid')
    if not g or int((ref or {}).get('fileID', 0)) == 0:
        return -1
    p = guids.map.get(g, '')
    m = re.search(r'SND/LoadedSamples/SE/Sample(\d+)\.wav$', p)
    if not m:
        raise SystemExit('clip is not an SE sample: %s' % p)
    return int(m.group(1))


def song_entry(guids, ref):
    """SongAsset reference -> (stage, bg, bank); a silent song (no clip) as (-1 - stage, bg, bank), so the
    pause menu can still show its name"""
    sp = guids.path(ref['guid'])
    song = unityyaml.load_single(sp)
    clip = song.get('Clip') or {}
    if not clip.get('guid') or int(clip.get('fileID', 0)) == 0:
        m = re.search(r'Original/STG(\d\d)/LoadedTracks/(BG[A-E])/BG[A-E]-([A-Z]+)Song\.asset$', sp)
        if not m:
            raise SystemExit('unexpected silent song %s' % sp)
        return -1 - int(m.group(1)), BG.index(m.group(2)), BANKS.index(m.group(3))
    p = guids.map.get(clip['guid'], '')
    m = re.search(r'Original/STG(\d\d)/LoadedTracks/(BG[A-E])/BG[A-E]-([A-Z]+)\.wav$', p)
    if not m:
        raise SystemExit('unexpected song clip %s' % p)
    return int(m.group(1)), BG.index(m.group(2)), BANKS.index(m.group(3))


def read_dream_tris(path):
    d = open(path, 'rb').read()
    if struct.unpack_from('<I', d, 4)[0] != 4:
        raise SystemExit('%s: not a v4 dream.bin' % path)
    o = 8
    nenv = struct.unpack_from('<I', d, o)[0]
    o += 4 + nenv * 56
    nsp = struct.unpack_from('<I', d, o)[0]
    o += 4 + nsp * 20 + 12
    nb = struct.unpack_from('<I', d, o)[0]
    o += 4
    for _ in range(nb):
        _a, _g, nv, ni = struct.unpack_from('<IIII', d, o)
        o += 40 + nv * 24 + ni * 2 + (2 if ni % 2 else 0)
    ng = struct.unpack_from('<I', d, o)[0]
    o += 4 + ng * 24
    nw = struct.unpack_from('<I', d, o)[0]
    o += 4 + nw * 64
    ni = struct.unpack_from('<I', d, o)[0]
    o += 4
    for _ in range(ni):
        w, h = struct.unpack_from('<II', d, o)
        o += 8 + w * h * 4
    nt = struct.unpack_from('<I', d, o)[0]
    o += 4
    return [struct.unpack_from('<9f', d, o + i * 36) for i in range(nt)]


def footsteps(proj, guids, dream_asset, out_dir):
    """Replays bake.py's collision triangle walk to know each triangle's LBD chunk."""
    dream = unityyaml.load_single(dream_asset)
    prefab = os.path.join(proj, 'Assets', 'Resources', dream['DreamPrefabPath'] + '.prefab')
    docs = unityyaml.load(prefab)
    scene = bake.Scene(docs)
    col = {}
    chunks = []
    tri_cache = {}
    for fid, d in docs.items():
        if d.type != 'PrefabInstance':
            continue
        src = d.body.get('m_SourcePrefab', {}).get('guid')
        path = guids.path(src)
        if not path or not path.upper().endswith('.LBD'):
            continue
        inst = scene.instance_world(fid)
        settings = bake.importer_settings(path)
        collide = int(settings.get('Collision', 1)) == 1
        overrides = bake.load_overrides(guids, settings)
        lbd = lsdfmt.read_lbd(open(path, 'rb').read())
        ci = len(chunks)
        chunks.append(((inst[0][3], inst[1][3], inst[2][3]), [t.footstep for t in lbd.layout]))
        for tile_no, tile in enumerate(lbd.layout):
            if not tile.draw:
                continue
            x, y = tile_no // lbd.w, tile_no % lbd.h
            chain = [tile]
            cur, n = tile, 0
            while cur.extra >= 0 and n <= 1:
                cur = lbd.extra[cur.extra]
                chain.append(cur)
                n += 1
            for t in chain:
                key = (path, t.type)
                if key not in tri_cache:
                    tri_cache[key] = bake.tmd_object_tris(lbd.tiles[t.type], overrides)
                opaque, alpha, over = tri_cache[key]
                m = bake.mmul(inst, bake.mmul(bake.translate(x, -t.height, y), bake.ry(t.dir * 90)))
                for tris in [opaque, alpha] + over:
                    for tri in tris:
                        wt = tuple((bake.xform(m, v[0]), v[1], v[2]) for v in tri)
                        if collide:
                            pts = tuple(tuple(round(c, 5) for c in v[0]) for v in wt)
                            k = tuple(sorted(pts))
                            col[k] = (pts, ci)
    tris = list(col.values())
    ref = read_dream_tris(os.path.join(out_dir, 'dream.bin'))
    if len(ref) != len(tris):
        raise SystemExit('%s: %d triangles, dream.bin has %d' % (out_dir, len(tris), len(ref)))
    for (pts, _ci), r in zip(tris, ref):
        flat = [c for v in pts for c in v]
        if any(abs(a - b) > 1e-4 for a, b in zip(flat, r)):
            raise SystemExit('%s: triangle order differs from dream.bin' % out_dir)
    out = bytearray(b'LSDF' + struct.pack('<II', 1, len(chunks)))
    for pos, fs in chunks:
        fs = (fs + [0] * 400)[:400]
        out += struct.pack('<3fI', *pos, min(400, len(fs))) + bytes(fs)
    out += struct.pack('<I', len(tris))
    out += struct.pack('<%dH' % len(tris), *[ci for _p, ci in tris])
    if len(tris) % 2:
        out += b'\0\0'
    with open(os.path.join(out_dir, 'footsteps.bin'), 'wb') as f:
        f.write(out)
    return len(chunks), len(tris)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--skip-footsteps', action='store_true')
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    orig = os.path.join(proj, 'Assets', 'Resources', 'Original')
    idx = os.path.join(a.data, 'guid_index.txt')
    unityyaml.build_guid_index(proj, idx)
    guids = unityyaml.GuidIndex(idx, proj)

    # the sound data itself
    os.makedirs(os.path.join(a.data, 'snd'), exist_ok=True)
    for b in BANKS:
        for ext in ('VH', 'VB'):
            shutil.copyfile(os.path.join(orig, 'SND', '%s.%s' % (b, ext)), os.path.join(a.data, 'snd', '%s.%s' % (b, ext)))
    for st in range(14):
        for bg in BG:
            src = os.path.join(orig, 'STG%02d' % st, bg + '.SEQ')
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(a.data, 'stg%02d' % st, bg + '.SEQ'))

    # OriginalSongLibrary
    lib = unityyaml.load_single(os.path.join(orig, 'OriginalSongLibrary.asset'))
    music = []
    for dream in lib['OriginalDreams']:
        styles = []
        for _name, field in STYLES:
            lst = unityyaml.load_single(guids.path(dream[field]['guid']))
            styles.append([song_entry(guids, s) for s in lst.get('Songs') or []])
        music.append(styles)

    # DreamSystem.LinkSound and the journal's footstep index
    ds = unityyaml.load_single(os.path.join(proj, 'Assets/UnityEditor/Systems/DreamSystem.asset'))
    link = se_sample(guids, ds.get('LinkSound'))
    fi = unityyaml.load_single(os.path.join(orig, 'SND', 'LBDFootstepIndex.asset'))
    keys_hex = fi['Index']['_keys']
    keys = list(struct.unpack('<%di' % (len(keys_hex) // 8), bytes.fromhex(keys_hex)))
    vals = [(se_sample(guids, v.get('Sound')), float(v.get('Pitch', 1))) for v in fi['Index']['_values']]
    fallback = (se_sample(guids, fi['Fallback'].get('Sound')), float(fi['Fallback'].get('Pitch', 1)))

    out = bytearray(b'LSDA' + struct.pack('<I', 1))
    out += struct.pack('<I', len(BANKS))
    for b in BANKS:
        out += b.encode().ljust(16, b'\0')
    out += struct.pack('<I', len(music))
    for styles in music:
        for songs in styles:
            out += struct.pack('<I', len(songs))
            for s in songs:
                out += struct.pack('<iii', *s)
    out += struct.pack('<i', link)
    out += struct.pack('<if', *fallback)
    out += struct.pack('<I', len(keys))
    for k, (smp, pitch) in zip(keys, vals):
        out += struct.pack('<iif', k, smp, pitch)
    with open(os.path.join(a.data, 'audio.bin'), 'wb') as f:
        f.write(out)
    print('audio.bin: %d banks, %d dreams x %d styles (%s songs), link sound SE%d, %d footstep kinds' % (
        len(BANKS), len(music), len(STYLES), '/'.join(str(len(s)) for s in music[0]), link, len(keys)))

    if not a.skip_footsteps:
        assets = {}
        for st in range(14):
            d = os.path.join(orig, 'STG%02d' % st)
            for f in os.listdir(d):
                if f.startswith('STG%02d-' % st) and f.endswith('.asset'):
                    assets[st] = os.path.join(d, f)
        for st in sorted(assets):
            nc, nt = footsteps(proj, guids, assets[st], os.path.join(a.data, 'stg%02d' % st))
            print('stg%02d: %d chunks, %d triangles' % (st, nc, nt))
    os.remove(idx)


if __name__ == '__main__':
    main()
