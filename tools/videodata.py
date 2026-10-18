#!/usr/bin/env python3
"""The original game's videos (LSDR's FILM/*.webm and ETC/ENDING), converted for the Vita.

usage: videodata.py --project <LSDRevamped/LSDR> [--lfs-cache DIR] --data <data dir>

LSDR's clips are VP8/Vorbis WebM (320x240, 15 fps, 37800 Hz). The Vita build plays MPEG-1 video
with MP2 audio in an MPEG program stream (pl_mpeg), so each one becomes data/movies/NAME.mpg:
the same frames at 15 fps (an unofficial MPEG-1 frame rate code, which src/pl_mpeg.h knows) and
the sound resampled to 44100 Hz. NAME is the clip's file name before '.STR' (EVENT1, SPDAY01A, ENDING,
OPENINGA...). Clips converted before are left alone.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lfsget  # noqa: E402

DIRS = ('Assets/Resources/Original/FILM', 'Assets/Resources/Original/ETC')


def convert(job):
    src, dst = job
    tmp = dst + '.part'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-c:v', 'mpeg1video', '-strict', 'unofficial', '-r', '15',
                    '-b:v', '900k', '-maxrate', '2500k', '-bufsize', '2000k', '-g', '15', '-c:a', 'mp2', '-ar', '44100',
                    '-b:a', '128k', '-f', 'mpeg', tmp], check=True)
    os.replace(tmp, dst)
    return dst, os.path.getsize(dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--lfs-cache', default=os.environ.get('LSDR_LFS_CACHE', '.lfs-cache'))
    ap.add_argument('--data', required=True)
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    out = os.path.join(a.data, 'movies')
    os.makedirs(out, exist_ok=True)
    jobs = []
    for d in DIRS:
        for f in sorted(os.listdir(os.path.join(proj, d))):
            if not f.endswith('.webm'):
                continue
            name = f.split('.STR')[0]
            if d.endswith('ETC') and name != 'ENDING' and not name.startswith('OPENING'):
                continue  # (ETC: the ending, and the intro scene's openings A-F)
            rel = os.path.relpath(os.path.join(proj, d, f), os.path.dirname(proj))
            lfsget.fetch(proj, a.lfs_cache, rel)
            import hashlib
            data = lfsget.fetch(proj, a.lfs_cache, rel)
            src = os.path.join(a.lfs_cache, hashlib.sha256(data).hexdigest())
            dst = os.path.join(out, name + '.mpg')
            if os.path.exists(dst):
                continue  # (converted before: keep it as it is, so its pack does not change)
            jobs.append((src, dst))
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        total = 0
        for dst, size in ex.map(convert, jobs):
            total += size
            print('%s: %.1f MB' % (os.path.basename(dst), size / 1e6))
    print('%d videos, %.1f MB' % (len(jobs), total / 1e6))


if __name__ == '__main__':
    main()
