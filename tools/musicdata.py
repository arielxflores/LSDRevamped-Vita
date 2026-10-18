#!/usr/bin/env python3
"""LSD Revamped's own soundtrack (the RevampedSongLibrary: RevampedSongList.asset, community songs
as Ogg Vorbis), for the game's "Use original soundtrack" setting turned off.

usage: musicdata.py --project <LSDRevamped/LSDR> [--lfs-cache DIR] --data <data dir>

Writes data/music/NNN.ogg (the files as they are: the Vita decodes Vorbis itself) and
data/music.bin ('LSDM' v1):
  u32 nsongs; per song: char name[64], char author[64], u32 sample rate, u32 channels, char file[16]
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lfsget     # noqa: E402
import unityyaml  # noqa: E402


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries',
                          'stream=sample_rate,channels,codec_name', '-of', 'default=nw=1', path],
                         capture_output=True, text=True).stdout
    d = dict(line.split('=', 1) for line in out.split() if '=' in line)
    return d.get('codec_name'), int(d.get('sample_rate', 0)), int(d.get('channels', 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--lfs-cache', default=os.environ.get('LSDR_LFS_CACHE', '.lfs-cache'))
    ap.add_argument('--data', required=True)
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    idx = os.path.join(a.data, 'guid_index_music.txt')
    unityyaml.build_guid_index(proj, idx)
    guids = unityyaml.GuidIndex(idx, proj)
    os.remove(idx)
    lib = unityyaml.load_single(os.path.join(proj, 'Assets/UnityEditor/Resources/Sound/Music/RevampedSongList.asset'))
    songs = []
    for ref in lib.get('Songs') or []:
        sp = guids.path(ref['guid'])
        s = unityyaml.load_single(sp)
        clip = s.get('Clip') or {}
        cp = guids.path(clip.get('guid', '')) if int(clip.get('fileID', 0) or 0) else None
        songs.append((str(s.get('Name') or ''), str(s.get('Author') or ''), cp))
    out_dir = os.path.join(a.data, 'music')
    os.makedirs(out_dir, exist_ok=True)

    def get(i):
        name, author, cp = songs[i]
        if not cp:
            return i, None
        rel = os.path.relpath(cp, os.path.dirname(proj))
        for attempt in range(5):
            try:
                data = lfsget.fetch(proj, a.lfs_cache, rel)
                break
            except Exception as e:  # (a dropped download: try again)
                if attempt == 4:
                    raise
                print('retrying %s (%s)' % (rel, e))
        fn = '%03d.ogg' % i
        path = os.path.join(out_dir, fn)
        if not os.path.exists(path) or os.path.getsize(path) != len(data):
            with open(path, 'wb') as f:
                f.write(data)
        return i, fn

    files = [None] * len(songs)
    with concurrent.futures.ThreadPoolExecutor(3) as ex:
        for i, fn in ex.map(get, range(len(songs))):
            files[i] = fn
    b = bytearray(b'LSDM' + struct.pack('<I', 1))
    entries = []
    for (name, author, _cp), fn in zip(songs, files):
        rate = ch = 0
        if fn:
            codec, rate, ch = probe(os.path.join(out_dir, fn))
            if codec != 'vorbis':
                print('skipping %s: %s' % (name, codec))
                fn = None
        entries.append((name, author, rate, ch, fn or ''))
    b += struct.pack('<I', len(entries))
    for name, author, rate, ch, fn in entries:
        b += name.encode('utf-8')[:63].ljust(64, b'\0') + author.encode('utf-8')[:63].ljust(64, b'\0')
        b += struct.pack('<II', rate, ch) + fn.encode().ljust(16, b'\0')
    with open(os.path.join(a.data, 'music.bin'), 'wb') as f:
        f.write(b)
    rates = {}
    for e in entries:
        rates[(e[2], e[3])] = rates.get((e[2], e[3]), 0) + 1
    total = sum(os.path.getsize(os.path.join(out_dir, e[4])) for e in entries if e[4])
    print('music.bin: %d songs (%d with a clip), %.1f MB, rates/channels %s' % (
        len(entries), sum(1 for e in entries if e[4]), total / 1e6, rates))


if __name__ == '__main__':
    main()
