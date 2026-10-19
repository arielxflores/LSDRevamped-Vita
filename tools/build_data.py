#!/usr/bin/env python3
"""Build the game data for the Vita port from LSD Revamped's repository.

usage: build_data.py [--lsdr DIR] [--out DIR] [--jobs N]

Clones figglewatts/LSDRevamped at the commit the port was made against (or uses the checkout given
with --lsdr), downloads the Git LFS files it needs from GitHub, converts the dreams, entities,
sounds, music, videos and menus, and packs them. The result goes to OUT/lsdr/ (default:
./lsdr-data/lsdr/): copy that folder to ux0:data/ on the Vita.

Needs Python 3.8+ with numpy and Pillow, git, ffmpeg (with ffprobe) and a C compiler (for the
LZ4 packer). About 1.5 GB of disk space and 20-60 minutes, depending on the machine.
"""
import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys
import time

LSDR_REPO = 'https://github.com/figglewatts/LSDRevamped.git'
LSDR_COMMIT = '8238ab75ae49d1886f746d17191904d81a54aa5c'
TOOLS = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def run(args, quiet=False):
    cmd = [sys.executable, os.path.join(TOOLS, args[0])] + args[1:]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        raise SystemExit('failed: %s' % ' '.join(args))
    if not quiet and r.stdout.strip():
        last = r.stdout.strip().splitlines()[-1]
        log('  ' + last[:160])
    return r.stdout


def check_requirements():
    missing = []
    for exe in ('ffmpeg', 'ffprobe'):
        if shutil.which(exe) is None:
            missing.append(exe)
    try:
        import lz4.block  # noqa: F401  (otherwise mkpak.py builds its packer with gcc)
    except ImportError:
        if shutil.which('cc') is None and shutil.which('gcc') is None:
            missing.append('a C compiler (gcc)')
    for mod in ('numpy', 'PIL', 'yaml'):
        try:
            __import__(mod)
        except ImportError:
            missing.append('python module %s' % {'PIL': 'Pillow', 'yaml': 'PyYAML'}.get(mod, mod))
    if not os.path.isfile(os.environ.get('LSDR_CJK_FONT', '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')):
        missing.append('the Noto Sans CJK font (package fonts-noto-cjk)')
    if missing:
        raise SystemExit('missing: ' + ', '.join(missing) + '\ninstall them with:  sudo apt install git '
                         'python3-numpy python3-pil python3-yaml ffmpeg gcc fonts-noto-cjk')


def download_archive(path):
    """without git: the commit's source archive from GitHub (LFS files are fetched separately by lfsget.py)"""
    import urllib.request
    import zipfile
    url = os.environ.get('LSDR_ARCHIVE_URL', 'https://codeload.github.com/figglewatts/LSDRevamped/zip/' + LSDR_COMMIT)
    tmp = path + '.zip'
    os.makedirs(os.path.dirname(path), exist_ok=True)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    log('downloading LSD Revamped into %s' % path)
    for attempt in range(1, 6):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(tmp, 'wb') as f:
                got = 0
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    got += len(chunk)
                    print('\r  %d MB' % (got >> 20), end='', flush=True)
            print()
            zipfile.ZipFile(tmp).testzip()
            break
        except Exception as e:  # (a dropped connection or a cut-off file: try again)
            print()
            if attempt == 5:
                raise SystemExit('download failed (%s): check the connection and run this again' % e)
            log('  download interrupted (%s), trying again (%d of 5)' % (e, attempt + 1))
            time.sleep(5)
    if os.path.isdir(path):
        shutil.rmtree(path)
    with zipfile.ZipFile(tmp) as z:
        top = z.namelist()[0].split('/')[0]
        z.extractall(os.path.dirname(path))
    os.replace(os.path.join(os.path.dirname(path), top), path)
    os.remove(tmp)
    with open(os.path.join(path, '.lsdr-commit'), 'w') as f:
        f.write(LSDR_COMMIT + '\n')


def checkout(path):
    """LSD Revamped at the pinned commit, LFS files left as pointers (lfsget.py fetches what is needed)"""
    if shutil.which('git') is None:
        marker = os.path.join(path, '.lsdr-commit')
        if not (os.path.isfile(marker) and open(marker).read().strip() == LSDR_COMMIT):
            download_archive(path)
        return os.path.join(path, 'LSDR')
    env = dict(os.environ, GIT_LFS_SKIP_SMUDGE='1')
    git = ['git', '-C', path]
    if not os.path.isdir(os.path.join(path, '.git')):
        log('downloading LSD Revamped (about 100 MB) into %s' % path)
        os.makedirs(path, exist_ok=True)
        subprocess.run(git + ['init', '--quiet'], check=True)
        subprocess.run(git + ['remote', 'add', 'origin', LSDR_REPO], check=True)
    head = subprocess.run(git + ['rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if head != LSDR_COMMIT or not os.path.isdir(os.path.join(path, 'LSDR', 'Assets')):
        subprocess.run(git + ['fetch', '--quiet', '--depth', '1', 'origin', LSDR_COMMIT], check=True, env=env)
        subprocess.run(git + ['checkout', '--quiet', '--force', LSDR_COMMIT], check=True, env=env)
    return os.path.join(path, 'LSDR')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--lsdr', default='LSDRevamped', help='LSD Revamped checkout (cloned here if missing)')
    ap.add_argument('--out', default='lsdr-data', help='where the work files and the result go')
    ap.add_argument('--jobs', type=int, default=max(1, min(4, (os.cpu_count() or 2) // 2)))
    a = ap.parse_args()
    check_requirements()
    t0 = time.time()
    proj = checkout(os.path.abspath(a.lsdr))
    out = os.path.abspath(a.out)
    data = os.path.join(out, 'data')
    cache = os.path.join(out, 'lfs-cache')
    os.makedirs(data, exist_ok=True)
    os.environ['LSDR_LFS_CACHE'] = cache

    log('journal')
    run(['journal.py', '--project', proj, '--out', data])
    dreams = []
    for st in range(14):
        found = glob.glob(os.path.join(proj, 'Assets/Resources/Original/STG%02d/STG%02d-*.asset' % (st, st)))
        if len(found) != 1:
            raise SystemExit('dream STG%02d not found in %s' % (st, proj))
        dreams.append((st, os.path.relpath(found[0], proj)))

    log('dreams: geometry and textures (%d at a time)' % a.jobs)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for st, asset in dreams:
            os.makedirs(os.path.join(data, 'stg%02d' % st), exist_ok=True)
        futs = {ex.submit(run, ['bake.py', '--project', proj, '--dream', asset, '--out',
                                os.path.join(data, 'stg%02d' % st), '--lfs-cache', cache], True): st
                for st, asset in dreams}
        for f in concurrent.futures.as_completed(futs):
            f.result()
            log('  STG%02d done' % futs[f])
    log('sound banks, songs and footsteps')
    run(['audiodata.py', '--project', proj, '--data', data])
    log('dreams: entities, models and scripts')
    for st, asset in dreams:  # (one at a time: they share the scripts' include folder)
        run(['scene.py', '--project', proj, '--dream', asset, '--data', data], True)
    log('the grey man')
    run(['greyman.py', '--project', proj, '--out', os.path.join(data, 'greyman.bin')])
    log('skies')
    run(['skydata.py', '--project', proj, '--lfs-cache', cache, '--data', data])
    log('special days')
    run(['specialdays.py', '--project', proj, '--data', data])
    log('Revamped soundtrack')
    run(['musicdata.py', '--project', proj, '--lfs-cache', cache, '--data', data])
    log('videos (ffmpeg: the longest step)')
    run(['videodata.py', '--project', proj, '--lfs-cache', cache, '--data', data])
    log('menus')
    run(['uibake.py', '--project', proj, '--lfs-cache', cache, '--data', data], True)

    dest = os.path.join(out, 'lsdr')
    os.makedirs(dest, exist_ok=True)
    for old in glob.glob(os.path.join(dest, '*.pak')):
        os.remove(old)
    log('packing')
    run(['mkpak.py', '--data', data, '--out', os.path.join(dest, 'dreams.pak')])
    run(['mkpak.py', '--data', data, '--movies', '--out', os.path.join(dest, 'movies.pak')])
    run(['mkpak.py', '--data', data, '--intro', '--out', os.path.join(dest, 'intro.pak')])
    run(['mkpak.py', '--data', data, '--music', '--out', os.path.join(dest, 'music.pak')])
    size = sum(os.path.getsize(p) for p in glob.glob(os.path.join(dest, '*.pak')))
    log('done in %.0f minutes: %s (%.0f MB) - copy that folder to ux0:data/ on the Vita' % (
        (time.time() - t0) / 60, dest, size / 1e6))


if __name__ == '__main__':
    main()
