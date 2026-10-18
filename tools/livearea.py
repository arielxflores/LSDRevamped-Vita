#!/usr/bin/env python3
"""The VPK's LiveArea images, made from the artwork in art/src:

  icon.jpg        -> sce_sys/icon0.png                        128x128 (the bubble)
  background.png  -> sce_sys/livearea/contents/bg.png         840x500 (cropped to fit, not stretched)
  startup.png     -> sce_sys/livearea/contents/startup.png    280x158 (the logo on the same grid)

The Vita wants LiveArea images as 8-bit indexed PNGs, so each is reduced to a 256-colour palette.

usage: livearea.py --src art/src --out build/sce_sys_art
"""
import argparse
import os
import shutil

from PIL import Image

BG_W, BG_H = 840, 500
GATE_W, GATE_H = 280, 158


def indexed(im):
    return im.convert('RGB').quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)


def cover(im, w, h):
    """scale to fill w x h, cropping the overflow equally from both sides"""
    s = max(w / im.width, h / im.height)
    nw, nh = round(im.width * s), round(im.height * s)
    im = im.resize((nw, nh), Image.LANCZOS)
    x, y = (nw - w) // 2, (nh - h) // 2
    return im.crop((x, y, x + w, y + h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default='art/src')
    ap.add_argument('--out', required=True)
    ap.add_argument('--template', default='sce_sys/livearea/contents/template.xml')
    a = ap.parse_args()
    lc = os.path.join(a.out, 'sce_sys', 'livearea', 'contents')
    os.makedirs(lc, exist_ok=True)

    icon = Image.open(os.path.join(a.src, 'icon.jpg')).convert('RGB')
    icon = cover(icon, 128, 128)
    indexed(icon).save(os.path.join(a.out, 'sce_sys', 'icon0.png'), optimize=True)

    bg = cover(Image.open(os.path.join(a.src, 'background.png')).convert('RGB'), BG_W, BG_H)
    indexed(bg).save(os.path.join(lc, 'bg.png'), optimize=True)

    # the gate: the logo, scaled to fit with a margin, over the same grid at the same scale
    gate = bg.crop(((BG_W - GATE_W) // 2, (BG_H - GATE_H) // 2,
                    (BG_W - GATE_W) // 2 + GATE_W, (BG_H - GATE_H) // 2 + GATE_H)).convert('RGBA')
    logo = Image.open(os.path.join(a.src, 'startup.png')).convert('RGBA')
    s = min((GATE_W - 16) / logo.width, (GATE_H - 16) / logo.height)
    logo = logo.resize((round(logo.width * s), round(logo.height * s)), Image.LANCZOS)
    gate.alpha_composite(logo, ((GATE_W - logo.width) // 2, (GATE_H - logo.height) // 2))
    indexed(gate).save(os.path.join(lc, 'startup.png'), optimize=True)

    shutil.copyfile(a.template, os.path.join(lc, 'template.xml'))
    for p in ('sce_sys/icon0.png', 'sce_sys/livearea/contents/bg.png', 'sce_sys/livearea/contents/startup.png'):
        im = Image.open(os.path.join(a.out, p))
        print('%s: %dx%d %s, %d bytes' % (p, im.width, im.height, im.mode, os.path.getsize(os.path.join(a.out, p))))


if __name__ == '__main__':
    main()
