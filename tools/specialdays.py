#!/usr/bin/env python3
"""Special days (DreamJournal.SpecialDays) for the Vita build.

usage: specialdays.py --project <LSDRevamped/LSDR> [--journal <asset>] --data <data dir> [--preview DIR]

Writes data/specialdays.bin ('LSDS' v1):
  u32 ndays; per day: u32 day number, u32 always, i32 dynamic, i32 upper (graph contribution),
                      u32 nchoices, nchoices x (u32 kind (0 text, 1 video), u32 index)
  u32 nvideos; per video: char[16] movie name (movies/NAME.mpg)
  u32 ntexts
and data/special/textNN.bin for each text screen: u32 width, u32 height, width*height bytes of
coverage (bottom row first, as GL textures go).

A special day is either one day (TextSpecialDay / VideoSpecialDay) or a RandomSpecialDay whose
choices are such days. LSDR shows a text day with a Unity UI Text (built-in Arial, best fit 10-40,
centred, white on black) in a 1280x720 reference canvas inset by 100/60 units; the Japanese text
falls back to a system CJK font. Here each text screen is drawn once, at the Vita's 960x544, with
Noto Sans CJK JP.
"""
import argparse
import os
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unityyaml  # noqa: E402

W, H = 960, 544
SCALE = W / 1280.0                    # CanvasScaler: scale with screen size, match width
RECT_W = (1280 - 200) * SCALE         # the Text's rect (sizeDelta -200, -120)
RECT_H = (H / SCALE - 120) * SCALE
LINE = 1.15                           # Arial's line height per unit of font size
FONT = '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'  # (Ubuntu/Debian package fonts-noto-cjk)


def render_text(text):
    lines = text.replace('\r', '').split('\n')
    best = None
    for size in range(40, 9, -1):     # best fit: the largest size whose wrapped text fits
        px = size * SCALE
        font = ImageFont.truetype(FONT, max(1, round(px)), index=0)
        if len(lines) * LINE * px > RECT_H:
            continue
        if any(font.getlength(l) > RECT_W for l in lines):
            continue
        best = (px, font)
        break
    if best is None:
        px = 10 * SCALE
        best = (px, ImageFont.truetype(FONT, round(px), index=0))
    px, font = best
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    lh = LINE * px
    top = (H - lh * len(lines)) / 2
    asc, desc = font.getmetrics()
    for i, l in enumerate(lines):
        if not l:
            continue
        x = (W - font.getlength(l)) / 2
        # the glyphs sit in the middle of their line box
        y = top + i * lh + (lh - (asc + desc)) / 2
        d.text((x, y), l, font=font, fill=255)
    return im, px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--journal', default='Assets/Resources/Original/OriginalDreams.asset')
    ap.add_argument('--data', required=True)
    ap.add_argument('--preview')
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    idx = os.path.join(a.data, 'guid_index_sd.txt')
    unityyaml.build_guid_index(proj, idx)
    guids = unityyaml.GuidIndex(idx, proj)
    os.remove(idx)

    def asset(ref):
        p = guids.path(ref['guid'])
        b = unityyaml.load_single(p)
        return p, b, guids.name(b['m_Script']['guid'])

    j = unityyaml.load_single(os.path.join(proj, a.journal))
    sd = j.get('SpecialDays') or {}
    keys = struct.unpack('<%di' % (len(sd['_keys']) // 8), bytes.fromhex(sd['_keys'])) if sd.get('_keys') else ()
    vals = sd.get('_values') or []
    videos, video_index = [], {}
    texts, text_index = [], {}

    def leaf(ref):
        p, b, script = asset(ref)
        if script == 'TextSpecialDay.cs':
            t = b.get('TextJa') or ''
            if not t.strip():
                t = b.get('TextEn') or ''
            if p not in text_index:
                text_index[p] = len(texts)
                texts.append((os.path.basename(p), t))
            return 0, text_index[p]
        if script == 'VideoSpecialDay.cs':
            clip = guids.map.get(b['VideoClip']['guid'], '')
            m = re.search(r'/([A-Z0-9]+)\.STR\[0\]\.webm$', clip)
            if not m:
                raise SystemExit('unexpected video clip %s in %s' % (clip, p))
            if m.group(1) not in video_index:
                video_index[m.group(1)] = len(videos)
                videos.append(m.group(1))
            return 1, video_index[m.group(1)]
        raise SystemExit('unsupported special day %s (%s)' % (p, script))

    days = []
    for day, ref in sorted(zip(keys, vals)):
        p, b, script = asset(ref)
        v = (b.get('Contribution') or {}).get('_vector') or {}
        contrib = (int(float(v.get('x', 0))), int(float(v.get('y', 0))))
        if script == 'RandomSpecialDay.cs':
            choices = [leaf(c) for c in b.get('SpecialDayChoices') or []]
        else:
            choices = [leaf(ref)]
        days.append((day, int(b.get('Always', 0)), contrib, choices))

    out = bytearray(b'LSDS' + struct.pack('<I', 1))
    out += struct.pack('<I', len(days))
    for day, always, (dyn, up), choices in days:
        out += struct.pack('<IIiiI', day, always, dyn, up, len(choices))
        for kind, i in choices:
            out += struct.pack('<II', kind, i)
    out += struct.pack('<I', len(videos))
    for v in videos:
        out += v.encode().ljust(16, b'\0')
    out += struct.pack('<I', len(texts))
    sizes = []
    os.makedirs(os.path.join(a.data, 'special'), exist_ok=True)
    for i, (name, t) in enumerate(texts):
        im, px = render_text(t)
        sizes.append(px)
        with open(os.path.join(a.data, 'special', 'text%02d.bin' % i), 'wb') as f:
            f.write(struct.pack('<II', W, H) + im.transpose(Image.FLIP_TOP_BOTTOM).tobytes())
        if a.preview:
            os.makedirs(a.preview, exist_ok=True)
            im.save(os.path.join(a.preview, os.path.splitext(name)[0] + '.png'))
    with open(os.path.join(a.data, 'specialdays.bin'), 'wb') as f:
        f.write(out)
    print('specialdays.bin: %d special days, %d videos, %d text screens (font %.1f-%.1f px)' % (
        len(days), len(videos), len(texts), min(sizes) if sizes else 0, max(sizes) if sizes else 0))
    print('days:', ' '.join('%d%s' % (d, '*' if al else '') for d, al, _c, _ch in days))


if __name__ == '__main__':
    main()
