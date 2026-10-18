#!/usr/bin/env python3
"""Makes the Windows data builder: LSDRevamped-Vita-DataBuilder-win64.zip, a folder with
"Build LSD data.bat", an embedded Python with numpy, Pillow, PyYAML and lz4, ffmpeg, the Noto Sans CJK font
and the data tools. It runs on Linux and downloads the Windows parts.

usage: windows/make_bundle.py [--font FILE] [--out DIR]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PYTHON = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
FFMPEG = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl.zip'
WHEELS = ['numpy', 'pillow', 'lz4', 'pyyaml']
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NAME = 'LSDRevamped-Vita-DataBuilder'


def get(url, dest):
    if not os.path.exists(dest):
        print('downloading', url, flush=True)
        with urllib.request.urlopen(url, timeout=600) as r, open(dest + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(dest + '.part', dest)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font', default='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'windows'))
    a = ap.parse_args()
    dl = os.path.join(a.out, 'downloads')
    stage = os.path.join(a.out, NAME)
    os.makedirs(dl, exist_ok=True)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    py = os.path.join(stage, 'python')
    with zipfile.ZipFile(get(PYTHON, os.path.join(dl, os.path.basename(PYTHON)))) as z:
        z.extractall(py)
    pth = glob.glob(os.path.join(py, 'python3*._pth'))[0]
    with open(pth, 'w', newline='\r\n') as f:
        f.write('python312.zip\n.\nLib\\site-packages\n..\\tools\nimport site\n')
    shutil.copy(os.path.join(HERE, 'sitecustomize.py'), py)
    whl = os.path.join(dl, 'wheels')
    subprocess.run([sys.executable, '-m', 'pip', 'download', '-q', '--only-binary=:all:', '--platform', 'win_amd64',
                    '--python-version', '3.12', '--implementation', 'cp', '-d', whl] + WHEELS, check=True)
    site = os.path.join(py, 'Lib', 'site-packages')
    for w in glob.glob(os.path.join(whl, '*.whl')):
        with zipfile.ZipFile(w) as z:
            z.extractall(site)

    ff = os.path.join(stage, 'ffmpeg')
    os.makedirs(ff)
    with zipfile.ZipFile(get(FFMPEG, os.path.join(dl, os.path.basename(FFMPEG)))) as z:
        for n in z.namelist():
            if n.endswith(('bin/ffmpeg.exe', 'bin/ffprobe.exe', 'LICENSE.txt')):
                with z.open(n) as s, open(os.path.join(ff, os.path.basename(n)), 'wb') as d:
                    shutil.copyfileobj(s, d)
    with open(os.path.join(ff, 'SOURCE.txt'), 'w', newline='\r\n') as f:
        f.write('FFmpeg (LGPL build) from %s\nSource code: https://github.com/BtbN/FFmpeg-Builds and '
                'https://ffmpeg.org/download.html\n' % FFMPEG)

    fd = os.path.join(stage, 'font')
    os.makedirs(fd)
    shutil.copy(a.font, fd)
    shutil.copy(os.path.join(HERE, 'OFL.txt'), fd)

    td = os.path.join(stage, 'tools')
    os.makedirs(td)
    for p in glob.glob(os.path.join(ROOT, 'tools', '*.py')):
        if os.path.basename(p) != 'livearea.py':
            shutil.copy(p, td)
    shutil.copy(os.path.join(HERE, 'Build LSD data.bat'), stage)
    shutil.copy(os.path.join(HERE, 'README.txt'), stage)

    zp = os.path.join(a.out, NAME + '-win64.zip')
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dp, _dn, fn in os.walk(stage):
            for f in sorted(fn):
                p = os.path.join(dp, f)
                z.write(p, os.path.relpath(p, a.out))
    print('%s: %.0f MB' % (zp, os.path.getsize(zp) / 1e6))


if __name__ == '__main__':
    main()
