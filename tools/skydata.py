#!/usr/bin/env python3
"""Sky effects data (DreamEnvironmentEffects: sun, sunburst, clouds, stars).

usage: skydata.py --project <LSDRevamped/LSDR> --lfs-cache <dir> --data <data dir>

Writes
  data/sky.bin            'LSDK' v1: the sun, cloud and star textures (RGBA, rows bottom-up as Unity
                          samples them), fetched from LSDR's Git LFS storage (tools/lfsget.py)
  data/stgNN/envfx.bin    'LSDX' v1: per environment of the dream (same order as dream.bin's), the
                          DreamEnvironment fields for the effects
"""
import argparse
import glob
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lfsget       # noqa: E402
import unityyaml    # noqa: E402

TEXTURES = [  # (name, path in the LSDRevamped repo)
    ('sun', 'LSDR/Assets/UnityEditor/Resources/Textures/Sky/sun.png'),       # Sun.prefab SunTexture, SunburstRay.mat
    ('clouds', 'LSDR/Assets/UnityEditor/Resources/Textures/Sky/clouds1.png'),  # Cloud.mat
    ('stars', 'LSDR/Assets/Resources/Original/ETC/stars.png'),               # Stars.mat
]


def col(e, k, default):
    c = e.get(k)
    if not isinstance(c, dict):
        return list(default)
    return [float(c.get(x, d)) for x, d in zip('rgba', default)]


def env_fx(e):
    """DreamEnvironment fields, with the class's initial values for missing ones"""
    f = lambda k, d: float(e.get(k, d))  # noqa: E731
    return (col(e, 'SunColor', (1, 0, 0, 1)) + [f('SunChance', 1), f('SunHeight', 2), f('SunSize', 1)] +
            col(e, 'SecondSunColor', (0, 0, 1, 1)) + [f('SecondSunSize', 1), f('SecondSunChance', 0.5),
                                                       f('SecondSunOffset', -0.15), f('SecondSunHeight', 2)] +
            col(e, 'SunBurstColor', (1, 0, 0, 1)) + [f('SunBurstSize', 1), f('SunBurstChance', 0.5)] +
            col(e, 'CloudsColor', (0, 0, 0, 0)) + [int(e.get('NumberOfClouds', 6)), f('CloudsChance', 0.5)] +
            col(e, 'StarsColor', (0, 0, 0, 0)) + [int(e.get('NumberOfStars', 3)), f('StarsChance', 0)])


FMT = '<4f3f4f4f4f2f4fif4fif'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--lfs-cache', required=True)
    ap.add_argument('--data', required=True)
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    checkout = os.path.dirname(proj)

    out = bytearray(b'LSDK' + struct.pack('<II', 1, len(TEXTURES)))
    for name, rel in TEXTURES:
        im = Image.open(io.BytesIO(lfsget.fetch(checkout, a.lfs_cache, rel))).convert('RGBA')
        im = im.transpose(Image.FLIP_TOP_BOTTOM)  # Unity's v = 0 is the bottom row
        out += name.encode().ljust(16, b'\0') + struct.pack('<II', im.width, im.height) + im.tobytes()
        print('%s: %dx%d' % (name, im.width, im.height))
    with open(os.path.join(a.data, 'sky.bin'), 'wb') as f:
        f.write(out)

    idx = os.path.join(a.data, 'guid_index.txt')
    unityyaml.build_guid_index(proj, idx)
    guids = unityyaml.GuidIndex(idx, proj)
    for st in range(14):
        asset = glob.glob(os.path.join(proj, 'Assets/Resources/Original/STG%02d/STG%02d-*.asset' % (st, st)))[0]
        dream = unityyaml.load_single(asset)
        envs = []
        seq_guid = (dream.get('Environments') or {}).get('guid')
        if seq_guid:
            seq = unityyaml.load_single(guids.path(seq_guid))
            for e in seq.get('Environments', []):
                envs.append(env_fx(unityyaml.load_single(guids.path(e['guid']))))
        if not envs:
            envs.append(env_fx({}))
        o = bytearray(b'LSDX' + struct.pack('<II', 1, len(envs)))
        for v in envs:
            o += struct.pack(FMT, *v)
        with open(os.path.join(a.data, 'stg%02d' % st, 'envfx.bin'), 'wb') as f:
            f.write(o)
        print('stg%02d: %d environments, effects %s' % (st, len(envs), 'on' if dream.get('HasEnvironmentEffects') else 'off'))
    os.remove(idx)


if __name__ == '__main__':
    main()
