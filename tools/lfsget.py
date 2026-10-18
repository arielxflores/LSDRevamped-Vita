#!/usr/bin/env python3
"""Fetch the real contents of Git LFS files in the LSDRevamped checkout (whose LFS files are
only pointers) from GitHub's LFS media server, at the checkout's commit, checking each
file's SHA-256 against its pointer. Downloads are cached.

usage: lfsget.py --project <LSDRevamped checkout> --cache <dir> PATH...   (paths relative to the checkout)
       as a module: fetch(project, cache, relpath) -> bytes
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request

REPO = 'figglewatts/LSDRevamped'


def commit_of(project):
    """the checkout's root and commit (a downloaded archive has a .lsdr-commit file instead of .git)"""
    d = os.path.abspath(project)
    while True:
        marker = os.path.join(d, '.lsdr-commit')
        if os.path.isfile(marker):
            return d, open(marker).read().strip()
        if os.path.dirname(d) == d:
            break
        d = os.path.dirname(d)
    root = subprocess.run(['git', '-C', project, 'rev-parse', '--show-toplevel'], capture_output=True, text=True).stdout.strip()
    sha = subprocess.run(['git', '-C', project, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return root, sha


def fetch(project, cache, rel):
    root, sha = commit_of(project)
    path = os.path.join(root, rel)
    ptr = open(path, 'rb').read()
    m = re.match(rb'version https://git-lfs.github.com/spec/v1\noid sha256:([0-9a-f]{64})\nsize (\d+)', ptr)
    if not m:
        return ptr  # a normal file
    oid, size = m.group(1).decode(), int(m.group(2))
    os.makedirs(cache, exist_ok=True)
    cp = os.path.join(cache, oid)
    if os.path.exists(cp):
        data = open(cp, 'rb').read()
        if hashlib.sha256(data).hexdigest() == oid:
            return data
    url = 'https://media.githubusercontent.com/media/%s/%s/%s' % (REPO, sha, urllib.parse.quote(rel.replace(os.sep, '/')))
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    if len(data) != size or hashlib.sha256(data).hexdigest() != oid:
        raise SystemExit('LFS download of %s does not match its pointer' % rel)
    with open(cp, 'wb') as f:
        f.write(data)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('paths', nargs='+')
    a = ap.parse_args()
    for p in a.paths:
        d = fetch(a.project, a.cache, p)
        print('%s: %d bytes' % (p, len(d)))


if __name__ == '__main__':
    main()
