#!/usr/bin/env python3
"""LSD Revamped's title screen (titlescreen.unity) and pause menu (PauseCanvas.prefab), laid out the way
Unity's UI does it, for the Vita.

usage: uibake.py --project <LSDRevamped/LSDR> [--lfs-cache DIR] --data <data dir> [--preview DIR]

Unity lays its UI out at run time (RectTransforms, layout groups, layout elements, text preferred
sizes, the canvas scaler). This tool does the same once, for the Vita's 960x544 screen, and writes
data/ui.bin ('LSDU' v1):

  u32 nviews; per view: char[24] name, u32 first, u32 count       (elements, in drawing order)
  u32 nelems; per element (64 bytes):
      u32 kind (0 rectangle/image, 1 text), i32 view, i32 tex (-1 = plain colour), i32 font,
      f32 x0, y0, x1, y1 (pixels, y down), f32 r, g, b, a, i32 tag (TAG_*), i32 text (string),
      u32 align (Unity TextAnchor), f32 shadow_dx, shadow_dy, shadow_a
  u32 nbuttons; per button: i32 graphic (element), i32 up, down, left, right (buttons),
      i32 action (ACT_*), i32 view, f32 normal[4], highlighted[4], pressed[4], disabled[4], f32 multiplier
  u32 nstrings; per string: u32 length, bytes (UTF-8)
  u32 nfonts; per font: f32 pixel size, f32 ascent, f32 line height, u32 nglyphs, atlas w, h,
      glyphs (u32 codepoint, f32 advance, i16 x, y, w, h (in the atlas), i16 bx, by (offset from
      the pen on the baseline)), atlas bytes (8-bit coverage)
  u32 ntex; per texture: u32 w, h, RGBA bytes (bottom row first)

Texts are drawn with glyph atlases baked here from the game's own fonts (Roboto, Fredoka One),
so dynamic strings (the day number, the journal name) use the same path as the static ones.
"""
import argparse
import copy
import io
import re
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lfsget     # noqa: E402
import unityyaml  # noqa: E402

SCREEN_W, SCREEN_H = 960, 544

# Unity UI script GUIDs (com.unity.ugui)
G_IMAGE = 'fe87c0e1cc204ed48ad3b37840f39efc'
G_RAWIMAGE = '1344c3c82d62a2a41a3576d8abb8e3ea'
G_TEXT = '5f7201a12d95ffc409449d95f23cf332'
G_BUTTON = '4e29b1a8efbd4b44bb3f3716e73f07ff'
G_SCALER = '0cd44c1031e13a943bb63640046fad76'
G_HLAYOUT = '30649d3a9faa99c48a7b1166b86bf2a0'
G_VLAYOUT = '59f8146938fff824cb5fd77236b75775'
G_GRID = '8a8695521f0d02e499659fee002a26c2'
G_LAYOUTELEM = '306cc8c2b49d7114eaa3623786fc2126'
G_FITTER = '3245ec927659c4140ac4f8d17403cc18'
G_SHADOW = 'cfabb0440166ab443bba8876756fdfa9'
G_TOGGLE = 'b52f93f5115b8fd40a43ef8d9dce91f2'    # Torii.UI.Toggle (UnityEngine.UI.Toggle)
G_SLIDER = '23886dbc19368c34c9faa6904ab10ee0'    # Torii.UI.Slider
G_DROPDOWN = '766fdd57afc05cf4691fec5319b8cdff'  # Torii.UI.Dropdown
G_TABVIEW = 'd2f7a82ff29e5294594e4ececaf56611'
G_HANDLE_DISPLAY = '29a549fde2bc7f445bdfc539bb72314b'
G_VIDEO_SETTINGS = 'a0a500e62d4944b4ab2949f320e5fa05'
G_GAMEPLAY_SETTINGS = '00df089e069e0f142b7d49c739132fff'
G_AUDIO_SETTINGS = 'aff542bdeef525e4e8d5c30772b3895f'
G_CONTROL_SETTINGS = 'c1731b07828cdff4199ae77cd1473a92'
G_KOFI = 'f93adf351b374f14bef26fb6cd36bafe'
G_SDK = '422bec2e82e141c48703e64dd4835e79'
G_CREDITS_SECTION = '43d895b0767847ca829433ce22159ec8'
G_SELECTME = '40f476b341cd56646831b54634e3a29b'
G_INPUTFIELD = 'd199490a83bb2b844b9695cbf13b01ef'  # UnityEngine.UI.InputField
G_MASK = '31a19414c41e5ae4aae2af33fee712f6'
G_SCROLLRECT = '1aa08ab6e0800fa44ae55d278d1423e3'
G_SCROLLBAR = '2a4db7a114972834c8e4117be1d82ba3'
SELECTABLES = (G_BUTTON, G_TOGGLE, G_SLIDER, G_DROPDOWN, G_INPUTFIELD)
# old prefabs reference UnityEngine.UI.dll's classes by fileID
G_UI_DLL = 'f70555f144d8491a825f0804e09c671c'
UI_DLL = {708705254: G_TEXT, -765806418: G_IMAGE, -98529514: G_RAWIMAGE, 1392445389: G_BUTTON,
          1297475563: G_VLAYOUT, -405508275: G_HLAYOUT, -2095666955: G_GRID, 1679637790: G_LAYOUTELEM,
          1741964061: G_FITTER}

# element tags: what the game fills in or changes at run time
TAGS = {'DayText': 1, 'SequenceText': 2, 'BuildNum': 3, 'YearCount': 4, 'GraphImage': 5, 'SongName': 6,
        'ArtistName': 7}
# (src/ui.h UI_TAG_*) the scheme creator's submit button text, the rebind rows' action texts (+row),
# the binding choice modal's title, cancel text and buttons (+i), the waiting-for-input modal's texts
TAG_SUBMIT_TEXT, TAG_ROW0, TAG_BIND_TITLE, TAG_BIND_CANCEL, TAG_BIND_BUTTON0 = 10, 20, 28, 29, 30
TAG_WAIT_TITLE, TAG_WAIT_TEXT, TAG_WAIT_CANCEL = 33, 34, 35
REBIND_ROWS = 7     # RebindableActions: Move, Interact, Run, LookUp, LookDown, NextProfile, Strafe
BIND_CHOICES = 2    # the most bindings an action has on the Vita (Move: the left stick, the d-pad)
# (src/ui.h UI_ACT_*) a row's rebind, reset and clear buttons; a choice modal's binding buttons
ACT_ROW_REBIND, ACT_ROW_RESET, ACT_ROW_CLEAR, ACT_BIND_CHOICE = 64, 72, 80, 88
# groups: 0 a tab's panel, 1 scrolling content, 2 a panel shown or hidden by the game (src/ui.h UI_PANEL_*)
PANELS = {'SchemeDisplayView': 1, 'SchemeCreateView': 2}
# button actions
ACTIONS = {'StartButton': 1, 'FlashbackButton': 2, 'GraphButton': 3, 'OptionsButton': 4, 'ExitButton': 5,
           'LeftButton': 6, 'RightButton': 7, 'BackButton': 8, 'ResumeButton': 9, 'QuitDreamButton': 10,
           'QuitGameButton': 5, 'SkipButton': 11}


def vec(d, k, default=(0.0, 0.0)):
    v = d.get(k) or {}
    return [float(v.get('x', default[0])), float(v.get('y', default[1]))]


def col(d, k='m_Color'):
    c = d.get(k) or {}
    return [float(c.get(ch, 1)) for ch in 'rgba']


class Node:
    def __init__(self, name, active):
        self.name, self.active = name, active
        self.parent, self.children = None, []
        self.amin = self.amax = self.pos = self.size = [0.0, 0.0]
        self.pivot = [0.5, 0.5]
        self.scale = [1.0, 1.0]
        self.rot = 0.0  # degrees about Z (Unity: counter-clockwise)
        self.comp = {}

    def path(self):
        return (self.parent.path() + '/' if self.parent else '') + self.name


class Scene:
    def __init__(self, path, guids):
        self.docs = unityyaml.load_resolved(path, guids)
        self.guids = guids
        self.nodes = {}
        gos = {fid: d for fid, d in self.docs.items() if d.type == 'GameObject'}
        tf_of = {}
        for fid, d in gos.items():
            n = Node(d.body.get('m_Name', ''), int(d.body.get('m_IsActive', 1)) == 1)
            self.nodes[fid] = n
            for c in d.body.get('m_Component', []):
                cd = self.docs.get(c['component']['fileID'])
                if cd is None:
                    continue
                if cd.type in ('RectTransform', 'Transform'):
                    tf_of[fid] = c['component']['fileID']
                    b = cd.body
                    n.amin, n.amax = vec(b, 'm_AnchorMin'), vec(b, 'm_AnchorMax')
                    n.pos, n.size = vec(b, 'm_AnchoredPosition'), vec(b, 'm_SizeDelta')
                    n.pivot = vec(b, 'm_Pivot', (0.5, 0.5))
                    s = b.get('m_LocalScale') or {}
                    n.scale = [float(s.get('x', 1)), float(s.get('y', 1))]
                    q = b.get('m_LocalRotation') or {}
                    n.rot = math.degrees(2 * math.atan2(float(q.get('z', 0)), float(q.get('w', 1))))
                elif cd.type == 'MonoBehaviour':
                    g = (cd.body.get('m_Script') or {}).get('guid')
                    if g == G_UI_DLL:
                        g = UI_DLL.get(int((cd.body.get('m_Script') or {}).get('fileID', 0)), g)
                    if int(cd.body.get('m_Enabled', 1)) != 1:
                        continue
                    n.comp[g] = cd.body
                    n.comp.setdefault('fid:' + g, c['component']['fileID'])
                elif cd.type == 'CanvasGroup':
                    n.comp['canvasgroup'] = cd.body
        self.node_of_tf = {tf: self.nodes[g] for g, tf in tf_of.items()}
        self.node_of_comp = {}
        for g, d in gos.items():
            for c in d.body.get('m_Component', []):
                self.node_of_comp[c['component']['fileID']] = self.nodes[g]
        for g, tf in tf_of.items():
            b = self.docs[tf].body
            n = self.nodes[g]
            for ch in b.get('m_Children', []):
                cn = self.node_of_tf.get(ch['fileID'])
                if cn:
                    cn.parent = n
                    n.children.append(cn)
            n.children.sort(key=lambda c: c.order if hasattr(c, 'order') else 0)
        for g, tf in tf_of.items():
            self.nodes[g].order = int(self.docs[tf].body.get('m_RootOrder', 0))
        for n in self.nodes.values():
            n.children.sort(key=lambda c: c.order)

    def find(self, name, under=None):
        for n in self.nodes.values():
            if n.name == name and (under is None or self.is_under(n, under)):
                return n
        raise KeyError(name)

    @staticmethod
    def is_under(n, a):
        while n:
            if n is a:
                return True
            n = n.parent
        return False


# ------------------------------------------------------------------ text metrics


class FontInfo:
    def __init__(self, path, data):
        self.path = path
        self.data = data
        self.cache = {}

    def at(self, px):
        px = max(1, int(round(px)))
        if px not in self.cache:
            self.cache[px] = ImageFont.truetype(io.BytesIO(self.data), px)
        return self.cache[px]

    def metrics(self, size):
        """Unity's Font: ascent and line height for a font size (canvas units)"""
        f = self.at(100)
        asc, desc = f.getmetrics()
        return asc / 100.0 * size, (asc + desc) / 100.0 * size

    def width(self, text, size):
        text = plain(text)
        f = self.at(100)
        return max((f.getlength(line) for line in text.split('\n')), default=0) / 100.0 * size

    def wrap(self, text, size, width):
        """Unity Text with Horizontal Overflow: Wrap: lines broken at spaces to fit the width"""
        f = self.at(100)
        k = size / 100.0
        out = []
        for para in text.split('\n'):
            line = ''
            for word in para.split(' '):
                cand = word if not line else line + ' ' + word
                if line and f.getlength(plain(cand)) * k > width + 0.5:
                    out.append(line)
                    line = word
                else:
                    line = cand
                while f.getlength(plain(line)) * k > width + 0.5 and len(line) > 1 and '<' not in line:
                    # (a word longer than the line)
                    cut = len(line) - 1
                    while cut > 1 and f.getlength(line[:cut]) * k > width + 0.5:
                        cut -= 1
                    out.append(line[:cut])
                    line = line[cut:]
            out.append(line)
        return '\n'.join(out)


RICH_TAG = re.compile(r'</?(?:b|i)>')


def plain(text):
    """a rich text's characters (Unity Text's <b> and <i> tags take no room; the runtime draws them)"""
    return RICH_TAG.sub('', text)


# ------------------------------------------------------------------ layout (UnityEngine.UI)


class Layout:
    def __init__(self, scene, fonts, canvas_size):
        self.s = scene
        self.fonts = fonts
        self.canvas_size = canvas_size
        self.root = None
        self.inputs = {}  # (node, axis) -> (min, preferred, flexible) of its layout group

    def rect(self, n):
        """(x, y, w, h) in canvas units, origin bottom-left"""
        if n.parent is None or n is self.root:
            return (0.0, 0.0, self.canvas_size[0], self.canvas_size[1])
        px, py, pw, ph = self.rect(n.parent)
        out = []
        for a, (p0, ps) in enumerate(((px, pw), (py, ph))):
            amin = p0 + n.amin[a] * ps
            amax = p0 + n.amax[a] * ps
            size = amax - amin + n.size[a]
            pivot_pos = amin + (amax - amin) * n.pivot[a] + n.pos[a]
            out.append((pivot_pos - n.pivot[a] * size, size))
        return (out[0][0], out[1][0], out[0][1], out[1][1])

    def children(self, n):
        out = []
        for c in n.children:
            if not c.active:
                continue
            le = c.comp.get(G_LAYOUTELEM)
            if le and int(le.get('m_IgnoreLayout', 0)):
                continue
            out.append(c)
        return out

    def group(self, n):
        for g in (G_HLAYOUT, G_VLAYOUT, G_GRID):
            if g in n.comp:
                return g, n.comp[g]
        return None, None

    # ILayoutElement properties: (min, preferred, flexible), priority-resolved like LayoutUtility
    def props(self, n, axis):
        cands = []  # (priority, min, pref, flex)
        g, lg = self.group(n)
        if g:
            cands.append((0,) + self.inputs.get((id(n), axis), (0.0, 0.0, -1.0)))
        if G_TEXT in n.comp:
            cands.append((0,) + self.text_props(n, axis))
        if G_IMAGE in n.comp:
            cands.append((0, 0.0, self.image_pref(n, axis), -1.0))
        le = n.comp.get(G_LAYOUTELEM)
        if le:
            k = 'Width' if axis == 0 else 'Height'
            cands.append((int(le.get('m_LayoutPriority', 1)), float(le.get('m_Min' + k, -1)),
                          float(le.get('m_Preferred' + k, -1)), float(le.get('m_Flexible' + k, -1))))
        res = []
        for i, default in ((1, 0.0), (2, 0.0), (3, 0.0)):
            best, bp = default, -10**9
            for c in cands:
                v = c[i]
                if v < 0:
                    continue
                if c[0] > bp:
                    best, bp = v, c[0]
                elif c[0] == bp:
                    best = max(best, v)
            res.append(best)
        mn, pref, flex = res
        return mn, max(mn, pref), flex

    def image_pref(self, n, axis):
        im = n.comp[G_IMAGE]
        sp = (im.get('m_Sprite') or {}).get('guid')
        if not sp or int((im.get('m_Sprite') or {}).get('fileID', 0)) == 0:
            return 0.0
        path = self.s.guids.path(sp)
        try:
            w, h = Image.open(io.BytesIO(load_asset(path))).size
        except Exception:
            return 0.0
        return float(w if axis == 0 else h)

    def text_props(self, n, axis):
        t = n.comp[G_TEXT]
        fd = t['m_FontData']
        font = self.fonts.get((fd['m_Font'] or {}).get('guid'))
        size = float(fd.get('m_FontSize', 14))
        text = str(t.get('m_Text') or '')
        if font is None or not text:
            return (0.0, 0.0, -1.0)
        if axis == 0:
            return (0.0, font.width(text, size), -1.0)
        if int(fd.get('m_HorizontalOverflow', 0)) == 0:
            text = font.wrap(text, size, self.rect(n)[2])
        _asc, lh = font.metrics(size)
        return (0.0, lh * float(fd.get('m_LineSpacing', 1)) * (text.count('\n') + 1), -1.0)

    def child_sizes(self, c, axis, control, force_expand):
        if not control:
            mn = pref = c.size[axis]
            flex = 0.0
        else:
            mn, pref, flex = self.props(c, axis)
        if force_expand:
            flex = max(flex, 1.0)
        return mn, pref, flex

    def calc(self, n, axis):
        for c in n.children:
            if c.active:
                self.calc(c, axis)
        g, lg = self.group(n)
        if not g:
            return
        kids = self.children(n)
        pad = lg.get('m_Padding') or {}
        hpad = float(pad.get('m_Left', 0)) + float(pad.get('m_Right', 0))
        vpad = float(pad.get('m_Top', 0)) + float(pad.get('m_Bottom', 0))
        padding = hpad if axis == 0 else vpad
        if g == G_GRID:
            cell = vec(lg, 'm_CellSize', (100, 100))
            sp = vec(lg, 'm_Spacing')
            constraint, count = int(lg.get('m_Constraint', 0)), int(lg.get('m_ConstraintCount', 2))
            if axis == 0:
                cols = count if constraint == 1 else (int(math.ceil(len(kids) / float(count))) if constraint == 2
                                                      else int(math.ceil(math.sqrt(len(kids)))))
                v = padding + (cell[0] + sp[0]) * cols - sp[0]
                self.inputs[(id(n), 0)] = (v, v, -1.0)
            else:
                if constraint == 1:
                    rows = int(math.ceil(len(kids) / float(count) - 0.001))
                elif constraint == 2:
                    rows = count
                else:
                    w = self.rect(n)[2]
                    cx = max(1, int(math.floor((w - hpad + sp[0] + 0.001) / (cell[0] + sp[0]))))
                    rows = int(math.ceil(len(kids) / float(cx)))
                v = padding + (cell[1] + sp[1]) * rows - sp[1]
                self.inputs[(id(n), 1)] = (v, v, -1.0)
            return
        vertical = g == G_VLAYOUT
        control = int(lg.get('m_ChildControlWidth' if axis == 0 else 'm_ChildControlHeight', 1))
        expand = int(lg.get('m_ChildForceExpandWidth' if axis == 0 else 'm_ChildForceExpandHeight', 1))
        spacing = float(lg.get('m_Spacing', 0))
        along_other = vertical ^ (axis == 1)
        tmin = tpref = padding
        tflex = 0.0
        for c in kids:
            mn, pref, flex = self.child_sizes(c, axis, control, expand)
            if along_other:
                tmin = max(mn + padding, tmin)
                tpref = max(pref + padding, tpref)
                tflex = max(flex, tflex)
            else:
                tmin += mn + spacing
                tpref += pref + spacing
                tflex += flex
        if not along_other and kids:
            tmin -= spacing
            tpref -= spacing
        tpref = max(tmin, tpref)
        self.inputs[(id(n), axis)] = (tmin, tpref, tflex)

    @staticmethod
    def set_child(c, axis, pos, size=None):
        """RectTransform.SetInsetAndSizeFromParentEdge, as LayoutGroup.SetChildAlongAxis does it"""
        if size is None:
            size = c.size[axis]
        c.amin = list(c.amin)
        c.amax = list(c.amax)
        c.size = list(c.size)
        c.pos = list(c.pos)
        if axis == 0:
            c.amin[0] = c.amax[0] = 0.0
            c.size[0] = size
            c.pos[0] = pos + size * c.pivot[0] * c.scale[0]
        else:
            c.amin[1] = c.amax[1] = 1.0
            c.size[1] = size
            c.pos[1] = -pos - size * (1 - c.pivot[1]) * c.scale[1]

    def place(self, n, axis):
        g, lg = self.group(n)
        if g:
            self.place_group(n, axis, g, lg)
        fitter = n.comp.get(G_FITTER)
        if fitter:
            fit = int(fitter.get('m_HorizontalFit' if axis == 0 else 'm_VerticalFit', 0))
            if fit:
                mn, pref, _f = self.props(n, axis)
                # SetSizeWithCurrentAnchors
                n.size = list(n.size)
                pw = self.rect(n.parent)[2 + axis]
                n.size[axis] = (mn if fit == 1 else pref) - (n.amax[axis] - n.amin[axis]) * pw
        for c in n.children:
            if c.active:
                self.place(c, axis)

    def place_group(self, n, axis, g, lg):
        kids = self.children(n)
        r = self.rect(n)
        size = r[2 + axis]
        pad = lg.get('m_Padding') or {}
        left, right = float(pad.get('m_Left', 0)), float(pad.get('m_Right', 0))
        top, bottom = float(pad.get('m_Top', 0)), float(pad.get('m_Bottom', 0))
        padding = (left + right) if axis == 0 else (top + bottom)
        align = int(lg.get('m_ChildAlignment', 0))
        align_on_axis = (align % 3) * 0.5 if axis == 0 else (align // 3) * 0.5

        def start_offset(required_wo_padding):
            required = required_wo_padding + padding
            surplus = size - required
            return (left if axis == 0 else top) + surplus * align_on_axis

        if g == G_GRID:
            cell = vec(lg, 'm_CellSize', (100, 100))
            sp = vec(lg, 'm_Spacing')
            constraint, count = int(lg.get('m_Constraint', 0)), int(lg.get('m_ConstraintCount', 2))
            w, h = r[2], r[3]
            if constraint == 1:
                cx = count
                cy = int(math.ceil(len(kids) / float(count) - 0.001))
            elif constraint == 2:
                cy = count
                cx = int(math.ceil(len(kids) / float(count) - 0.001))
            else:
                cx = max(1, int(math.floor((w - left - right + sp[0] + 0.001) / (cell[0] + sp[0]))))
                cy = max(1, int(math.floor((h - top - bottom + sp[1] + 0.001) / (cell[1] + sp[1]))))
            corner, start_axis = int(lg.get('m_StartCorner', 0)), int(lg.get('m_StartAxis', 0))
            if start_axis == 0:
                cpp = cx
                acx, acy = max(1, min(cx, len(kids))), max(1, min(cy, int(math.ceil(len(kids) / float(cpp)))))
            else:
                cpp = cy
                acy, acx = max(1, min(cy, len(kids))), max(1, min(cx, int(math.ceil(len(kids) / float(cpp)))))
            req = (acx * cell[0] + (acx - 1) * sp[0], acy * cell[1] + (acy - 1) * sp[1])
            so = start_offset(req[axis])
            for i, c in enumerate(kids):
                if start_axis == 0:
                    px, py = i % cpp, i // cpp
                else:
                    px, py = i // cpp, i % cpp
                if corner % 2 == 1:
                    px = acx - 1 - px
                if corner // 2 == 1:
                    py = acy - 1 - py
                p = (px, py)[axis]
                self.set_child(c, axis, so + (cell[axis] + sp[axis]) * p, cell[axis])
            return

        vertical = g == G_VLAYOUT
        control = int(lg.get('m_ChildControlWidth' if axis == 0 else 'm_ChildControlHeight', 1))
        expand = int(lg.get('m_ChildForceExpandWidth' if axis == 0 else 'm_ChildForceExpandHeight', 1))
        spacing = float(lg.get('m_Spacing', 0))
        along_other = vertical ^ (axis == 1)
        inner = size - padding
        if along_other:
            for c in kids:
                mn, pref, flex = self.child_sizes(c, axis, control, expand)
                req = min(max(inner, mn), size if flex > 0 else pref)
                so = start_offset(req)
                if control:
                    self.set_child(c, axis, so, req)
                else:
                    off = (req - c.size[axis]) * align_on_axis
                    self.set_child(c, axis, so + off)
            return
        tmin, tpref, tflex = self.inputs[(id(n), axis)]
        pos = left if axis == 0 else top
        mult = 0.0
        surplus = size - tpref
        if surplus > 0:
            if tflex == 0:
                pos = start_offset(tpref - padding)
            elif tflex > 0:
                mult = surplus / tflex
        lerp = 0.0
        if tmin != tpref:
            lerp = min(1.0, max(0.0, (size - tmin) / (tpref - tmin)))
        for c in kids:
            mn, pref, flex = self.child_sizes(c, axis, control, expand)
            cs = mn + (pref - mn) * lerp + flex * mult
            if control:
                self.set_child(c, axis, pos, cs)
            else:
                off = (cs - c.size[axis]) * align_on_axis
                self.set_child(c, axis, pos + off)
            pos += cs * c.scale[axis] + spacing

    def run(self, root):
        self.root = root
        for axis in (0, 1):
            self.calc(root, axis)
            self.place(root, axis)


_PROJ = _CACHE = None


def load_asset(path):
    rel = os.path.relpath(path, os.path.dirname(_PROJ))
    return lfsget.fetch(_PROJ, _CACHE, rel)


def canvas_scale(canvas):
    sc = canvas.comp[G_SCALER]
    ref = vec(sc, 'm_ReferenceResolution', (800, 600))
    match = float(sc.get('m_MatchWidthOrHeight', 0))
    return 2 ** ((1 - match) * math.log2(SCREEN_W / ref[0]) + match * math.log2(SCREEN_H / ref[1]))


def add_fonts(scene, fonts, guids):
    for n in scene.nodes.values():
        t = n.comp.get(G_TEXT)
        if t:
            g = (t['m_FontData']['m_Font'] or {}).get('guid')
            if g and g not in fonts and guids.path(g):
                fonts[g] = FontInfo(guids.path(g), load_asset(guids.path(g)))


def instantiate(scene, guids, fonts, prefab_rel, parent):
    """Object.Instantiate(prefab, parent): the prefab's root node (and its nodes and components, so
    references inside it resolve) grafted under parent, last"""
    ps = Scene(os.path.join(_PROJ, prefab_rel), guids)
    add_fonts(ps, fonts, guids)
    root = [n for n in ps.nodes.values() if n.parent is None][0]
    root.parent = parent
    root.order = len(parent.children)
    parent.children.append(root)
    scene.nodes.update({('%s#%d' % (prefab_rel, k)): n for k, n in ps.nodes.items()})
    scene.node_of_comp.update(ps.node_of_comp)
    scene.node_of_tf.update(ps.node_of_tf)
    scene.docs.update(ps.docs)
    return root, ps


# the Vita has one screen mode and fixed buttons: the options it has no use for are left out
VITA_HIDDEN = ('Resolution', 'Quality', 'Fullscreen')
# the options on the Vita's small screen: 1.45 times as large, the tabs at the top of the screen and room for
# the button prompts under Apply (x' = 480 + (x - 480) z, y' = 8 + (y - 115) z; Unity puts the tabs' top at 115)
OPTIONS_Z = 1.45
OPTIONS_ZOOM = (OPTIONS_Z, 480 - 480 * OPTIONS_Z, 8 - 115 * OPTIONS_Z)

_FAKE_FID = [-1000000]


def clone_node(scene, n, parent, fmap=None):
    """Object.Instantiate of a scene object (a template): a copy of its subtree under parent, its
    components' references to each other pointing into the copy"""
    top = fmap is None
    if top:
        fmap = {}
    tf_of = {id(v): k for k, v in scene.node_of_tf.items()}
    c = Node(n.name, n.active)
    for attr in ('amin', 'amax', 'pos', 'size', 'pivot', 'scale'):
        setattr(c, attr, list(getattr(n, attr)))
    c.rot = n.rot
    c.comp = copy.deepcopy(n.comp)
    for k in list(c.comp):
        if k.startswith('fid:'):
            _FAKE_FID[0] -= 1
            fmap[c.comp[k]] = _FAKE_FID[0]
            c.comp[k] = _FAKE_FID[0]
            scene.node_of_comp[_FAKE_FID[0]] = c
    old_tf = tf_of.get(id(n))
    if old_tf is not None:
        _FAKE_FID[0] -= 1
        fmap[old_tf] = _FAKE_FID[0]
        scene.node_of_tf[_FAKE_FID[0]] = c
        scene.node_of_comp[_FAKE_FID[0]] = c
    # (the other components on the object: Image, CanvasRenderer... are found through their object)
    for fid, node in list(scene.node_of_comp.items()):
        if node is n and fid not in fmap:
            _FAKE_FID[0] -= 1
            fmap[fid] = _FAKE_FID[0]
            scene.node_of_comp[_FAKE_FID[0]] = c
    c.parent = parent
    c.order = len(parent.children)
    parent.children.append(c)
    scene.nodes[('clone', _FAKE_FID[0])] = c
    for ch in n.children:
        clone_node(scene, ch, c, fmap)
    if top:
        def remap(o):
            if isinstance(o, dict):
                if 'fileID' in o and isinstance(o.get('fileID'), int) and o['fileID'] in fmap and 'guid' not in o:
                    o['fileID'] = fmap[o['fileID']]
                for v in o.values():
                    remap(v)
            elif isinstance(o, list):
                for v in o:
                    remap(v)

        def walk(x):
            for k, v in x.comp.items():
                if not k.startswith('fid:'):
                    remap(v)
            for ch in x.children:
                walk(ch)
        walk(c)
    return c


def rebind_rows(scene, root):
    """UIRebindContainerPopulator.populateRebindContainer: the template row switched off, a row per
    rebindable action (RebindableActions)"""
    container = scene.find('SchemeRowContainer', root)
    template = [c for c in container.children if c.name == 'SchemeRow'][0]
    sample = ['Move (Left Stick, D-Pad)', 'Interact (Circle)', 'Run (Cross)', 'LookUp (Triangle)',
              'LookDown (Square)', 'NextProfile (Select)', 'Strafe (L/R)']
    for i in range(REBIND_ROWS):
        row = clone_node(scene, template, container)
        row.active = True
        row.row_index = i
        for c in row.children:
            if c.name == 'ActionNameText':
                c.comp[G_TEXT]['m_Text'] = sample[i]
                c.tag_override = TAG_ROW0 + i
            c.row_index = i
    template.active = False
    # UIControlSchemeCreator.SubmitSchemeButtonText: "Create" or "Edit"
    creator = scene.find('SchemeCreateView', root)
    for n in scene.nodes.values():
        if n.name == 'Text' and n.parent is not None and n.parent.name == 'CreateButton' and \
                G_BUTTON in n.parent.comp and Scene.is_under(n, creator):
            n.tag_override = TAG_SUBMIT_TEXT
    return container


def binding_modals(scene, guids, fonts, modals):
    """UIBindingChoiceModal (its binding buttons, from its own switched-off template) and
    UIWaitingForInputModal, instantiated under the Modals object"""
    bm, bs = instantiate(scene, guids, fonts, 'Assets/UnityEditor/Resources/Prefabs/UI/Modals/BindingChoiceModal.prefab',
                         modals)
    wm, ws = instantiate(scene, guids, fonts,
                         'Assets/UnityEditor/Resources/Prefabs/UI/Modals/WaitingForInputModal.prefab', modals)
    for n in bs.nodes.values():
        if n.name == 'Title' and n.parent is bm:
            n.tag_override = TAG_BIND_TITLE
        elif n.name == 'CancelText' and n.parent is bm:
            n.tag_override = TAG_BIND_CANCEL
    for n in ws.nodes.values():
        if n.parent is wm and n.name in ('Title', 'WaitingText', 'CancelText'):
            n.tag_override = {'Title': TAG_WAIT_TITLE, 'WaitingText': TAG_WAIT_TEXT, 'CancelText': TAG_WAIT_CANCEL}[n.name]
    for n in bs.nodes.values():
        if n.name == 'Scrollbar Vertical':
            n.active = False  # (AutoHide: the bindings fit)
    cont = [n for n in bs.nodes.values() if n.name == 'SchemeRowContainer'][0]
    tmpl = [c for c in cont.children if c.name == 'BindingButton'][0]
    for i in range(BIND_CHOICES):
        b = clone_node(scene, tmpl, cont)
        b.active = True
        b.choice_index = i
        for c in b.children:
            if c.name == 'Text':
                c.comp[G_TEXT]['m_Text'] = 'Left Stick'
                c.tag_override = TAG_BIND_BUTTON0 + i
    bm.active = wm.active = False
    return bm, wm


def settings_view(scene, root):
    """the options (SettingsMenu.prefab): every tab laid out (UITabView shows one at a time)"""
    for n in scene.nodes.values():
        if not Scene.is_under(n, root):
            continue
        if n.name in VITA_HIDDEN:
            n.active = False
        tv = n.comp.get(G_TABVIEW)
        if tv:
            for v in tv.get('TabViews') or []:
                vn = scene.node_of_tf.get(int(v.get('fileID') or 0))
                if vn is not None:
                    vn.active = True


def slider_visuals(scene):
    """Slider.UpdateVisuals: the fill's anchorMax.x and the handle's anchors at the (normalised) value"""
    for n in list(scene.nodes.values()):
        b = n.comp.get(G_SLIDER)
        if not b:
            continue
        lo, hi = float(b.get('m_MinValue', 0)), float(b.get('m_MaxValue', 1))
        v = (float(b.get('m_Value', lo)) - lo) / (hi - lo) if hi != lo else 0.0
        v = min(1.0, max(0.0, v))
        fill = scene.node_of_tf.get(int((b.get('m_FillRect') or {}).get('fileID') or 0))
        handle = scene.node_of_tf.get(int((b.get('m_HandleRect') or {}).get('fileID') or 0))
        if fill is not None:
            fill.amin, fill.amax = [0.0, 0.0], [v, 1.0]
        if handle is not None:
            handle.amin, handle.amax = [v, 0.0], [v, 1.0]


def credits_content(scene, guids, fonts, credits_node):
    """UICredits.load: the title, a section per credits.json entry (UICreditsSection), the end cap"""
    import json
    uc = None
    for g, body in credits_node.comp.items():
        if not g.startswith('fid:') and g != 'canvasgroup' and 'UISectionContainer' in body:
            uc = body
    container = scene.node_of_tf.get(int(uc['UISectionContainer']['fileID']))
    path_of = {g: p for g, p in guids.map.items()}
    prefab = {k: os.path.relpath(guids.path(uc[k]['guid']), _PROJ) for k in ('UICreditsSectionPrefab', 'UITitleTextPrefab')}
    end_cap = os.path.relpath(guids.path(uc['UIEndCapObject']['guid']), _PROJ)
    data = json.load(open(os.path.join(_PROJ, 'Assets/StreamingAssets/credits.json'), encoding='utf-8-sig'))
    instantiate(scene, guids, fonts, prefab['UITitleTextPrefab'], container)
    by_path = {p: g for g, p in path_of.items()}
    for sec in data:
        root, ps = instantiate(scene, guids, fonts, prefab['UICreditsSectionPrefab'], container)
        body = root.comp[G_CREDITS_SECTION]
        title = ps.node_of_comp[int(body['Title']['fileID'])]
        title.comp[G_TEXT]['m_Text'] = sec.get('SectionTitle') or ''
        img = (sec.get('SectionImagePath') or '').strip()
        if img:
            rel = None
            for ext in ('.png', '.jpg'):
                for base in ('Assets/UnityEditor/Resources/', 'Assets/Resources/'):
                    if base + img + ext in by_path:
                        rel = base + img + ext
            if rel:
                im_node = ps.node_of_comp[int(body['Image']['fileID'])]
                raw = im_node.comp[G_RAWIMAGE]
                raw['m_Texture'] = {'fileID': 2800000, 'guid': by_path[rel]}
                raw['m_Color'] = {'r': 1, 'g': 1, 'b': 1, 'a': 1}
                w, h = Image.open(io.BytesIO(load_asset(os.path.join(_PROJ, rel)))).size
                le_node = ps.node_of_comp[int(body['ImageLayoutElement']['fileID'])]
                le = le_node.comp[G_LAYOUTELEM]
                le['m_MinHeight'] = h * float(le.get('m_MinWidth', 0)) / w
        names = list(sec.get('Names') or [])
        if sec.get('SortNames', True) is not False:
            names.sort()
        nc = ps.node_of_tf[int(body['NamesContainer']['fileID'])]
        name_prefab = os.path.relpath(guids.path(body['UICreditsSectionNamePrefab']['guid']), _PROJ)
        for nm in names:
            nn, _ = instantiate(scene, guids, fonts, name_prefab, nc)
            nn.comp[G_TEXT]['m_Text'] = nm
    instantiate(scene, guids, fonts, end_cap, container)
    return container


def main():
    global _PROJ, _CACHE
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--lfs-cache', default=os.environ.get('LSDR_LFS_CACHE', '.lfs-cache'))
    ap.add_argument('--data', required=True)
    ap.add_argument('--preview')
    a = ap.parse_args()
    _PROJ, _CACHE = os.path.abspath(a.project), a.lfs_cache
    idx = os.path.join(a.data, 'guid_index_ui.txt')
    unityyaml.build_guid_index(_PROJ, idx)
    guids = unityyaml.GuidIndex(idx, _PROJ)
    os.remove(idx)
    fonts = {}
    out = Builder(fonts)

    # the title screen: MainMenu, Graph, Credits, Settings (and the reset-progress question)
    scene = Scene(os.path.join(_PROJ, 'Assets/UnityEditor/Scenes/titlescreen.unity'), guids)
    canvas = scene.find('TitleScreenCanvas')
    scale = canvas_scale(canvas)
    canvas_size = (SCREEN_W / scale, SCREEN_H / scale)
    add_fonts(scene, fonts, guids)
    views = [scene.find(v, canvas) for v in ('MainMenu', 'Graph', 'Credits', 'Settings')]
    for v in views:
        v.active = True
    container = credits_content(scene, guids, fonts, views[2])
    settings_view(scene, views[3])
    rows = rebind_rows(scene, views[3])
    slider_visuals(scene)
    lay = Layout(scene, fonts, canvas_size)
    lay.run(canvas)
    print('TitleScreenCanvas: canvas %.1f x %.1f units, scale %.4f' % (canvas_size[0], canvas_size[1], scale))
    out.use(scene, lay, scale)
    for v in views:
        if v.name == 'Settings':
            out.set_zoom(*OPTIONS_ZOOM, highlight=True)
        out.add_view(v.name, v, scroll=container if v.name == 'Credits' else None,
                     extra=creator_extra(scene, canvas, v, rows, canvas_size, fonts, out) if v.name == 'Settings' else ())
        out.set_zoom(1.0)
    restore_selector(scene, views[3], out, lay, canvas)
    out.add_year_icon(os.path.join(_PROJ, 'Assets/UnityEditor/Resources/Prefabs/UI/YearIcon.prefab'))
    # UIModalController.ShowModal: the background, then the modal under the Modals object
    modals = scene.find('Modals', views[3])
    for c in modals.children:
        c.active = True
    modal, ms = instantiate(scene, guids, fonts, 'Assets/UnityEditor/Resources/Prefabs/UI/Modals/ChoiceModal.prefab',
                            modals)
    for n in ms.nodes.values():
        if n.name == 'Title' and G_TEXT in n.comp:
            n.comp[G_TEXT]['m_Text'] = 'Reset journal progress'
        elif n.name == 'Body' and G_TEXT in n.comp:
            n.comp[G_TEXT]['m_Text'] = 'This will reset progress in the current journal to Day 1.\n\nAre you sure?'
    lay.run(canvas)
    out.set_zoom(*OPTIONS_ZOOM, highlight=True)  # (over the options, as large)
    out.add_view('Modal', modals)
    modal_views(scene, guids, fonts, modals, canvas, lay, out, '')
    out.set_zoom(1.0)

    # the pause menu (PauseCanvas.prefab, over the dream) and its options
    scene = Scene(os.path.join(_PROJ, 'Assets/UnityEditor/Resources/Prefabs/UI/PauseCanvas.prefab'), guids)
    canvas = scene.find('PauseCanvas')
    scale = canvas_scale(canvas)
    canvas_size = (SCREEN_W / scale, SCREEN_H / scale)
    add_fonts(scene, fonts, guids)
    pause = scene.find('PauseMenu', canvas)
    psettings = scene.find('SettingsMenu', pause)
    pause.active = psettings.active = True
    settings_view(scene, psettings)
    prows = rebind_rows(scene, psettings)
    slider_visuals(scene)
    lay = Layout(scene, fonts, canvas_size)
    lay.run(canvas)
    print('PauseCanvas: canvas %.1f x %.1f units, scale %.4f' % (canvas_size[0], canvas_size[1], scale))
    out.use(scene, lay, scale)
    # (UIViewStack: the pause menu's Main, or its SettingsMenu, over the same Background)
    main_view = scene.find('Main', pause)
    psettings.active = False
    out.add_view('Pause', pause)
    psettings.active, main_view.active = True, False
    out.set_zoom(*OPTIONS_ZOOM, highlight=True)
    out.add_view('PauseSettings', pause, extra=creator_extra(scene, canvas, psettings, prows, canvas_size, fonts, out))
    restore_selector(scene, psettings, out, lay, canvas)
    main_view.active = True
    modal_views(scene, guids, fonts, scene.find('Modals', canvas), canvas, lay, out, 'P')
    out.set_zoom(1.0)

    sounds(a.data)
    out.write(os.path.join(a.data, 'ui.bin'))
    if a.preview:
        out.preview(a.preview)


def creator_extra(scene, canvas, settings_root, rows, canvas_size, fonts, out):
    """UIControlSchemeCreator.Show: the creator (SchemeCreateView) takes the selector's place
    (SchemeDisplayView) in the controls tab, laid out that way"""
    display = scene.find('SchemeDisplayView', settings_root)
    creator = scene.find('SchemeCreateView', settings_root)

    def setup():
        display.active, creator.active = False, True
        lay2 = Layout(scene, fonts, canvas_size)
        lay2.run(canvas)
        out.lay = lay2
    return [(creator, setup, [rows])]


def restore_selector(scene, settings_root, out, lay, canvas):
    scene.find('SchemeDisplayView', settings_root).active = True
    scene.find('SchemeCreateView', settings_root).active = False
    lay.run(canvas)
    out.lay = lay


def modal_views(scene, guids, fonts, modals, canvas, lay, out, prefix):
    """UIModalController: its Background, then the binding choice and waiting-for-input modals as views
    of their own (drawn over each other as the modal stack has them)"""
    bm, wm = binding_modals(scene, guids, fonts, modals)
    for c in modals.children:
        c.active = False
    bg = [c for c in modals.children if c.name == 'Background'][0]
    for name, on in (('ModalBg', bg), ('BindModal', bm), ('WaitModal', wm)):
        for c in modals.children:
            c.active = c is on
        lay.run(canvas)
        out.add_view(prefix + name, modals)
    for c in modals.children:
        c.active = False


MENU_SOUNDS = [('menu0', 'In Linking the Sapient Dream.ogg', 2), ('menu1', 'In Leisure the Sonorous Dream.ogg', 2),
               ('menu2', 'In Lability the Shifting Dream.ogg', 2), ('menu3', 'In Lucidity the Scintillating Dream.ogg', 2),
               ('select', 'select.ogg', 1), ('click', 'click.wav', 1), ('soundtest', 'sfxTest.wav', 1)]


def sounds(data):
    """the title screen's music (UIMainMenuMusic: Normal, Kanji, Downer, Upper) and button sounds, as
    ui/NAME.pcm: u32 rate, u32 channels, u32 frames, then 16-bit samples"""
    import hashlib
    import subprocess
    d = os.path.join(data, 'ui')
    os.makedirs(d, exist_ok=True)
    for name, src, ch in MENU_SOUNDS:
        raw = load_asset(os.path.join(_PROJ, 'Assets/UnityEditor/Resources/Sound/Menu', src))
        cp = os.path.join(_CACHE, hashlib.sha256(raw).hexdigest())
        if not os.path.exists(cp):
            with open(cp, 'wb') as f:
                f.write(raw)
        rate = 44100
        if src.endswith('.wav'):
            rate = struct.unpack_from('<I', raw, 24)[0]
        pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', cp, '-f', 's16le', '-ac', str(ch), '-ar', str(rate), '-'],
                             check=True, capture_output=True).stdout
        with open(os.path.join(d, name + '.pcm'), 'wb') as f:
            f.write(struct.pack('<III', rate, ch, len(pcm) // (2 * ch)) + pcm)


# settings the options menu's controls are bound to (UIVideoSettings, UIGameplaySettings,
# UIAudioSettings, UIControlSettings fields); src/settings.h's SET_*
SETTING_FIELDS = {
    G_VIDEO_SETTINGS: {'UseClassicShadersToggle': 1, 'UsePixelationShaderToggle': 2, 'UseDitheringToggle': 3,
                       'LimitFramerateToggle': 4, 'DrawDistanceToggle': 5, 'FOVSlider': 6, 'AffineSlider': 7},
    G_GAMEPLAY_SETTINGS: {'EnableHeadBobToggle': 8, 'SmoothHeadBobToggle': 9, 'HeadbobIntensitySlider': 10,
                          'EnableFootstepSoundsToggle': 11, 'SpecialDaysEnabledToggle': 12, 'CurrentModDropdown': 13,
                          'CurrentJournalDropdown': 14},
    G_AUDIO_SETTINGS: {'UseOriginalSoundtrackToggle': 15, 'MusicVolumeSlider': 16, 'SFXVolumeSlider': 17},
    G_CONTROL_SETTINGS: {'CurrentSchemeDropdown': 18},
}
# button actions from their onClick calls: (target asset or component, method) -> action
CLICKS = {('OnReturnToTitle.asset', 'Raise'): 8, ('OnPauseToggle.asset', 'Raise'): 9,
          ('UIPauseMenu.cs', 'ReturnToMenu'): 10, ('UIPauseMenu.cs', 'QuitGame'): 5, ('DreamSystem.asset', 'SkipSong'): 11,
          ('OnOptionsShouldOpen.asset', 'Raise'): 4, ('SettingsSystem.asset', 'Apply'): 12,
          ('OnCreditsShouldOpen.asset', 'Raise'): 13, ('AudioSource', 'Play'): 14,
          ('UIVideoSettingsProfile.cs', 'OnNextProfilePressed'): 15, ('UIVideoSettingsProfile.cs', 'OnNewProfilePressed'): 16,
          ('UIVideoSettingsProfile.cs', 'OnRevertProfilePressed'): 17,
          ('UIVideoSettingsProfile.cs', 'OnDeleteProfilePressed'): 18, ('UIControlSchemeCreator.cs', 'ShowEdit'): 19,
          ('UIControlSchemeCreator.cs', 'DeleteScheme'): 20, ('UIControlSchemeCreator.cs', 'ShowCreate'): 21,
          ('UIControlSchemeCreator.cs', 'Hide'): 30, ('UIControlSchemeCreator.cs', 'SubmitScheme'): 31}
ACT_RESET_PROGRESS, ACT_KOFI, ACT_SDK, ACT_MODAL_YES, ACT_MODAL_CANCEL, ACT_TAB = 22, 23, 24, 25, 26, 32
KIND_BUTTON, KIND_TOGGLE, KIND_SLIDER, KIND_DROPDOWN, KIND_INPUT = 0, 1, 2, 3, 4
NAV_MODES = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4}  # Unity Navigation.Mode: none, horizontal, vertical, automatic, explicit
# texts the game fills in, by node name (wide glyph sets)
NAMED_TAGS = {'ProfileHintText': 8, 'ProfileName': 9, 'SongName': 6, 'ArtistName': 7}


BUILTIN_GUID = '0000000000000000f000000000000000'


def builtin_sprite(fid):
    """Unity's built-in UI sprites (unity default resources), drawn the way they look"""
    im = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if fid == 10915:    # DropdownArrow
        d.polygon([(8, 11), (24, 11), (16, 21)], fill=(255, 255, 255, 255))
    elif fid == 10913:  # Checkmark
        d.line([(7, 16), (13, 23), (25, 9)], fill=(255, 255, 255, 255), width=4)
    elif fid == 10911:  # Knob
        d.ellipse([2, 2, 29, 29], fill=(255, 255, 255, 255))
    else:               # UISprite, Background, InputFieldBackground, UIMask: a white rounded box
        d.rounded_rectangle([0, 0, 31, 31], radius=4, fill=(255, 255, 255, 255))
    return im


class Builder:
    def __init__(self, fonts):
        self.s = self.lay = self.scale = None
        self.fonts = fonts
        self.views, self.elems, self.buttons, self.strings = [], [], [], []
        self.groups = []  # dicts: view, kind (0 a tab's panel, 1 scrolling content), tab, visible, height
        self.textures, self.tex_index = [], {}
        self.font_ids = {}
        self.font_chars = {}
        self.button_of_node = {}
        self.elems_of_node = {}
        self.group_of_node = {}
        self.tab_of_button = {}
        self.setting_of_node = {}
        # (the Vita's options: drawn larger than Unity lays them out, about a point; and a highlight
        # behind each control's row shown while it is selected)
        self.zoom, self.zox, self.zoy = 1.0, 0.0, 0.0
        self.highlight = False
        self.hl_at = {}       # node: [(control node, nodes it covers, flags)], emitted where that node is walked
        self.hl_pending = []  # (element, control node)
        self.hl_planned = set()

    def set_zoom(self, z, ox=0.0, oy=0.0, highlight=False):
        """the views that follow drawn z times larger, x' = ox + x z (screen pixels), with highlights"""
        self.zoom, self.zox, self.zoy, self.highlight = z, ox, oy, highlight

    def zx(self, x):
        return self.zox + x * self.zoom

    def zy(self, y):
        return self.zoy + y * self.zoom

    def use(self, scene, lay, scale):
        """the canvas the next views come from"""
        self.s, self.lay, self.scale = scene, lay, scale
        self.script_names = {}

    def script_name(self, guid):
        p = self.s.guids.map.get(guid or '', '')
        return os.path.basename(p)

    def px(self, n):
        x, y, w, h = self.lay.rect(n)
        # pivot-relative local scale
        px_, py_ = x + w * n.pivot[0], y + h * n.pivot[1]
        x0, x1 = px_ + (x - px_) * n.scale[0], px_ + (x + w - px_) * n.scale[0]
        y0, y1 = py_ + (y - py_) * n.scale[1], py_ + (y + h - py_) * n.scale[1]
        k = self.scale
        # screen pixels, y down; the canvas is pixel perfect (then the zoom)
        return (round(self.zx(x0 * k)), round(self.zy(SCREEN_H - y1 * k)), round(self.zx(x1 * k)),
                round(self.zy(SCREEN_H - y0 * k)))

    def texture(self, guid, max_w, max_h, fid=0):
        builtin = guid == BUILTIN_GUID
        path = 'builtin:%d' % fid if builtin else self.s.guids.path(guid)
        key = path
        if key not in self.tex_index:
            if builtin:
                im = builtin_sprite(fid)
            else:
                try:
                    im = Image.open(io.BytesIO(load_asset(path))).convert('RGBA')
                except Exception:
                    raise SystemExit('not an image: %s (%s)' % (path, guid))
            self.tex_index[key] = len(self.textures)
            self.textures.append([path, im, 1, 1])
        t = self.textures[self.tex_index[key]]
        t[2] = max(t[2], int(math.ceil(max_w)))
        t[3] = max(t[3], int(math.ceil(max_h)))
        return self.tex_index[key]

    def string(self, s):
        self.strings.append(s)
        return len(self.strings) - 1

    def font(self, guid, size, chars):
        px = max(1, int(round(size * self.scale * self.zoom)))
        key = (guid, px)
        if key not in self.font_ids:
            self.font_ids[key] = len(self.font_ids)
            self.font_chars[key] = set()
        self.font_chars[key].update(chars)
        return self.font_ids[key]

    def alpha_of(self, n):
        a = 1.0
        while n:
            cg = n.comp.get('canvasgroup')
            if cg:
                a *= float(cg.get('m_Alpha', 1))
                if int(cg.get('m_IgnoreParentGroups', 0)):
                    break
            n = n.parent
        return a

    def node_of(self, ref):
        """the node a component reference ({fileID}) is on"""
        return self.s.node_of_comp.get(int((ref or {}).get('fileID') or 0))

    def node_of_tf(self, ref):
        return self.s.node_of_tf.get(int((ref or {}).get('fileID') or 0))

    def prepare(self, root):
        """before walking a view: tab panels (UITabView) and the settings the controls are bound to"""
        def shown(n):  # (under root, and not under a switched-off object)
            while n is not None and n is not root:
                if not n.active:
                    return False
                n = n.parent
            return n is root

        for n in self.s.nodes.values():
            if not shown(n):
                continue
            tv = n.comp.get(G_TABVIEW)
            if tv:
                start = int(tv.get('StartOnTab', 0))
                for k, (b, v) in enumerate(zip(tv.get('TabButtons') or [], tv.get('TabViews') or [])):
                    bn, vn = self.node_of(b), self.node_of_tf(v)
                    if bn is None or vn is None:
                        continue
                    self.tab_of_button[id(bn)] = k
                    self.add_group(vn, dict(view=len(self.views), kind=0, tab=k, visible=int(k == start), height=0.0))
            if n.name in PANELS and n.active:
                self.add_group(n, dict(view=len(self.views), kind=2, tab=PANELS[n.name],
                                       visible=int(PANELS[n.name] == 1), height=0.0))
            for g, fields in SETTING_FIELDS.items():
                body = n.comp.get(g)
                if not body:
                    continue
                for f, sid in fields.items():
                    cn = self.node_of(body.get(f))
                    if cn is not None:
                        self.setting_of_node[id(cn)] = sid
            # the scheme creator's controls (UIControlSchemeCreator's onValueChanged handlers)
            if n.parent is not None and n.name == 'Toggle' and n.parent.name == 'FpsControls':
                self.setting_of_node[id(n)] = 19
            elif n.parent is not None and n.name == 'Toggle' and n.parent.name == 'InvertY':
                self.setting_of_node[id(n)] = 20
            elif n.name == 'MouseSensitivitySlider':
                self.setting_of_node[id(n)] = 21
            elif G_INPUTFIELD in n.comp and n.parent is not None and n.parent.name == 'SchemeName':
                self.setting_of_node[id(n)] = 22
            if 'UIControlSchemeDropdownPopulator' in [self.script_name(k) .replace('.cs', '') for k in n.comp
                                                      if not k.startswith('fid:') and k != 'canvasgroup']:
                self.setting_of_node[id(n)] = 18

    def plan_highlights(self, root):
        """where each control's highlight goes: a row of the options' lists (its label and the control; a
        slider with the heading above it) shown behind the row while the control is selected (flag 1),
        the control alone (0), or a tab's button, marked while its tab is shown (2)"""
        def selectable(n):
            return any(g in n.comp for g in SELECTABLES)

        def count(n):
            return int(n.active and selectable(n)) + sum(count(c) for c in n.children if c.active)

        def texts(n):
            return int(G_TEXT in n.comp) + sum(texts(c) for c in n.children if c.active)

        def heading_before(row):
            p = row.parent
            if p is None:
                return None
            sib = [c for c in p.children if c.active]
            i = sib.index(row) if row in sib else -1
            if i > 0 and count(sib[i - 1]) == 0 and texts(sib[i - 1]) > 0:
                return sib[i - 1]
            return None

        stack = [root]
        while stack:
            n = stack.pop()
            stack.extend(c for c in n.children if c.active)
            if not selectable(n) or id(n) in self.hl_planned:
                continue
            self.hl_planned.add(id(n))
            p = n.parent
            if id(n) in self.tab_of_button:
                entry = (n, [n], 2)
            elif G_SLIDER in n.comp:  # (its row, and the heading row above it)
                row = p if p is not None and G_HLAYOUT in p.comp and count(p) == 1 else n
                h = heading_before(row)
                entry = (n, [h, row], 1) if h is not None else (n, [n], 0)
            elif p is not None and G_HLAYOUT in p.comp and count(p) == 1 and texts(p) > texts(n):  # (a label)
                entry = (n, [p], 1)
            else:
                entry = (n, [n], 0)
            self.hl_at.setdefault(id(entry[1][0]), []).append(entry)

    def add_group(self, n, g):
        """a group of elements (and controls) under n; inside another group, shown only while it is"""
        p, a = -1, n.parent
        while a is not None:
            if id(a) in self.group_of_node:
                p = self.group_of_node[id(a)]
                break
            a = a.parent
        if id(n) in self.group_of_node:
            return self.group_of_node[id(n)]
        g.setdefault('parent', p)
        g.setdefault('clip', (0.0, 0.0, 0.0, 0.0))
        g.setdefault('bar', (0.0, 0.0, 0.0, 0.0))
        self.group_of_node[id(n)] = len(self.groups)
        self.groups.append(g)
        return len(self.groups) - 1

    def add_view(self, name, root, scroll=None, extra=()):
        """extra: (subtree, setup) pairs walked after the view, each laid out by its own setup (a panel
        that takes the place of another one when the game shows it)"""
        first = len(self.elems)
        vi = len(self.views)
        was = root.active
        root.active = True  # (views are switched on by the view stack)
        self.prepare(root)
        if self.highlight:
            self.plan_highlights(root)
        if scroll is not None:
            # the credits' SectionContainer: its content scrolls up through the view
            r = self.lay.rect(scroll)
            self.add_group(scroll, dict(view=vi, kind=1, tab=-1, visible=1, height=r[3] * self.scale * self.zoom))
        self.walk(root, vi, -1)
        for sub, setup, scrolls in extra:
            setup()
            self.prepare(root)
            if self.highlight:
                self.plan_highlights(sub)
            for c in scrolls:
                self.scroll_group(c, vi)
            p, a = -1, sub.parent
            while a is not None and p < 0:
                p = self.group_of_node.get(id(a), -1)
                a = a.parent
            self.walk(sub, vi, p)
        root.active = was
        self.views.append((name, first, len(self.elems) - first))

    def scroll_group(self, content, vi):
        """a ScrollRect's content: moves under its viewport (Mask), which clips it; its vertical
        scrollbar's handle shows where (Scrollbar.UpdateVisuals, BottomToTop)"""
        vp = content.parent
        x0, y0, x1, y1 = self.px(vp)
        r = self.lay.rect(content)
        bar = (0.0, 0.0, 0.0, 0.0)
        sr = vp.parent.comp.get(G_SCROLLRECT) if vp.parent is not None else None
        handle = None
        if sr:
            sbn = self.node_of(sr.get('m_VerticalScrollbar'))
            sb = sbn.comp.get(G_SCROLLBAR) if sbn is not None else None
            handle = self.node_of_tf(sb.get('m_HandleRect')) if sb else None
            if handle is not None and handle.parent is not None:
                ax, ay, aw, ah = self.lay.rect(handle.parent)
                k, z = self.scale, self.zoom
                offmin = handle.pos[1] - handle.size[1] * handle.pivot[1]
                offmax = handle.pos[1] + handle.size[1] * (1 - handle.pivot[1])
                bar = (self.zy(SCREEN_H - ay * k), ah * k * z, offmin * k * z, offmax * k * z)
                handle.scroll_handle = True
        gi = self.add_group(content, dict(view=vi, kind=1, tab=-1, visible=1, height=r[3] * self.scale * self.zoom,
                                          clip=(float(x0), float(y0), float(x1), float(y1)), bar=bar))
        if handle is not None:
            handle.scroll_handle_group = gi
        return gi

    def rotation_of(self, n, rect):
        """a RectTransform's Z rotation with its parents' (each about its own pivot): the angle, and
        where the rect's middle ends up (screen pixels, y down)"""
        cx, cy = (rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5
        total = 0.0
        a = n
        while a is not None and a is not self.lay.root:
            r = getattr(a, 'rot', 0.0) % 360.0
            if abs(r) > 0.01 and abs(r - 360.0) > 0.01:
                x0, y0, x1, y1 = rect if a is n else self.px(a)
                px_, py_ = x0 + (x1 - x0) * a.pivot[0], y1 - (y1 - y0) * a.pivot[1]
                t = math.radians(r)  # (counter-clockwise on the screen, y down)
                dx, dy = cx - px_, cy - py_
                cx, cy = px_ + dx * math.cos(t) + dy * math.sin(t), py_ - dx * math.sin(t) + dy * math.cos(t)
                total += r
            a = a.parent
        return total % 360.0, cx, cy

    def add_elem(self, n, **e):
        if n is not None and self.lay is not None:
            x0, y0, x1, y1 = e['rect']
            rot, cx, cy = self.rotation_of(n, e['rect'])
            dx, dy = cx - (x0 + x1) * 0.5, cy - (y0 + y1) * 0.5
            if abs(dx) > 0.01 or abs(dy) > 0.01:
                e['rect'] = (round(x0 + dx), round(y0 + dy), round(x1 + dx), round(y1 + dy))
            if abs(rot) > 0.01 and abs(rot - 360.0) > 0.01:
                x0, y0, x1, y1 = e['rect']
                e['rot'] = rot
                e['pivot'] = ((x0 + x1) * 0.5, (y0 + y1) * 0.5)
        e.setdefault('vis', -1)
        e.setdefault('follow', -1)
        e.setdefault('follow_mode', 0)
        if n is not None and hasattr(n, 'scroll_handle_group'):
            e['follow'], e['follow_mode'] = n.scroll_handle_group, 3
        self.elems.append(e)
        if n is not None:
            self.elems_of_node.setdefault(id(n), []).append(len(self.elems) - 1)
        return len(self.elems) - 1

    def walk(self, n, vi, group):
        if not n.active:
            return
        group = self.group_of_node.get(id(n), group)
        for ctl, covers, flags in (self.hl_at.get(id(n), []) if self.highlight else []):
            rs = [self.px(m) for m in covers if m.active]
            if rs:
                hr = (min(r[0] for r in rs), min(r[1] for r in rs), max(r[2] for r in rs), max(r[3] for r in rs))
                e = self.add_elem(None, kind=5, view=vi, tex=-1, font=-1, rect=hr, color=[1, 1, 1, 1], tag=0, text=-1,
                                  align=flags, shadow=(0.0, 0.0, 0.0), node=None, group=group)
                self.hl_pending.append((e, ctl))
        alpha = self.alpha_of(n)
        rect = self.px(n)
        tag = getattr(n, 'tag_override', 0) or TAGS.get(n.name, NAMED_TAGS.get(n.name, 0))
        for g in (G_IMAGE, G_RAWIMAGE):
            im = n.comp.get(g)
            if not im:
                continue
            if int((n.comp.get(G_MASK) or {}).get('m_ShowMaskGraphic', 1)) == 0:
                continue  # (Mask: the graphic only cuts out what is under it)
            c = col(im)
            c[3] *= alpha
            tex = -1
            ref = im.get('m_Sprite' if g == G_IMAGE else 'm_Texture') or {}
            if ref.get('guid') and int(ref.get('fileID', 0)) != 0:
                tex = self.texture(ref['guid'], rect[2] - rect[0], rect[3] - rect[1], int(ref.get('fileID', 0)))
            self.add_elem(n, kind=0, view=vi, tex=tex, font=-1, rect=rect, color=c, tag=tag, text=-1, align=0,
                          shadow=self.shadow(n), node=n, group=group)
        t = n.comp.get(G_TEXT)
        if t:
            fd = t['m_FontData']
            text = str(t.get('m_Text') or '')
            size = float(fd.get('m_FontSize', 14))
            fi = self.fonts.get((fd['m_Font'] or {}).get('guid'))
            if fi and int(fd.get('m_HorizontalOverflow', 0)) == 0 and not tag:
                text = fi.wrap(text, size, self.lay.rect(n)[2])
            chars = set(text)
            if tag == 1:
                chars |= set('Day 0123456789')
            dynamic = tag in (2, 3, 6, 7) or tag >= 8 or getattr(n, 'dynamic_text', False)
            if dynamic:
                chars |= set(chr(c) for c in range(32, 127))
            fid = self.font(fd['m_Font']['guid'], size, chars)
            c = col(t)
            c[3] *= alpha
            self.add_elem(n, kind=1, view=vi, tex=-1, font=fid, rect=rect, color=c, tag=tag, text=self.string(text),
                          align=int(fd.get('m_Alignment', 0)), shadow=self.shadow(n), node=n, group=group)
        if tag and not any(g in n.comp for g in (G_IMAGE, G_RAWIMAGE, G_TEXT)):
            # a place the game fills in (the year icons)
            self.add_elem(n, kind=3, view=vi, tex=-1, font=-1, rect=rect, color=[1, 1, 1, alpha], tag=tag,
                          text=-1, align=4, shadow=(0.0, 0.0, 0.0), node=n, group=group)
        for g in SELECTABLES:
            b = n.comp.get(g)
            if b:
                self.button_of_node[id(n)] = (len(self.buttons), b, n, vi, self.s, g, group, rect, self.lay, self.scale)
                self.buttons.append(None)
                if g == G_SLIDER:  # (where the fill and handle move, as laid out now)
                    n.slider_geom = self.slider_geom(b, self.s, self.lay, self.scale)
                if g == G_INPUTFIELD:
                    for ref in ('m_TextComponent', 'm_Placeholder'):
                        tn = self.node_of(b.get(ref))
                        if tn is not None:
                            tn.dynamic_text = True
                if g == G_DROPDOWN:
                    cap = self.node_of(b.get('m_CaptionText'))
                    if cap is not None:
                        cap.dynamic_text = True
                elif g == G_SLIDER:
                    hd = n.comp.get(G_HANDLE_DISPLAY)
                    ht = self.node_of(hd.get('HandleText')) if hd else None
                    if ht is not None:
                        ht.dynamic_text = True
                break
        for ch in n.children:
            self.walk(ch, vi, group)
        if n.name == 'GraphImage':
            # UIGraph instantiates its squares as the last children of GraphImage
            self.add_elem(None, kind=2, view=vi, tex=-1, font=-1, rect=rect, color=[1, 1, 1, alpha], tag=0,
                          text=-1, align=0, shadow=(0.0, 0.0, 0.0), node=None, square=16 * self.scale * self.zoom,
                          group=group)

    def shadow(self, n):
        sh = n.comp.get(G_SHADOW)
        if not sh:
            return (0.0, 0.0, 0.0)
        d = vec(sh, 'm_EffectDistance', (1, -1))
        c = col(sh, 'm_EffectColor')
        return (d[0] * self.scale * self.zoom, -d[1] * self.scale * self.zoom, c[3])

    def add_year_icon(self, prefab):
        """YearIcon.prefab: a 9x9 Image (prestige.png) with a Shadow, laid out by YearCount's HLayout"""
        docs = unityyaml.load(prefab)
        sprite = shadow = None
        for d in docs.values():
            if d.type == 'MonoBehaviour':
                g = (d.body.get('m_Script') or {}).get('guid')
                if g == G_IMAGE:
                    sprite = d.body
                elif g == G_SHADOW:
                    shadow = d.body
        k = self.scale
        tex = self.texture(sprite['m_Sprite']['guid'], 9 * k, 9 * k)
        sh = (0.0, 0.0, 0.0)
        if shadow:
            dd = vec(shadow, 'm_EffectDistance', (1, -1))
            sh = (dd[0] * k, -dd[1] * k, col(shadow, 'm_EffectColor')[3])
        self.add_elem(None, kind=4, view=-1, tex=tex, font=-1, rect=(0, 0, 9 * k, 9 * k), color=col(sprite), tag=0,
                      text=-1, align=0, shadow=sh, node=None, spacing=2 * k, group=-1)

    def click_action(self, b, scene, n):
        for key in ('m_OnClick',):
            ev = b.get(key) or {}
            for c in (ev.get('m_PersistentCalls') or {}).get('m_Calls') or []:
                t = c.get('m_Target') or {}
                if 'guid' in t:
                    tname = os.path.basename(scene.guids.map.get(t['guid'], ''))
                else:
                    d = scene.docs.get(int(t.get('fileID') or 0))
                    if d is None:
                        continue
                    if d.type == 'MonoBehaviour':
                        tname = os.path.basename(scene.guids.map.get((d.body.get('m_Script') or {}).get('guid', ''), ''))
                    else:
                        tname = d.type
                a = CLICKS.get((tname, c.get('m_MethodName')))
                if a:
                    return a
        if G_KOFI in n.comp:
            return ACT_KOFI
        if G_SDK in n.comp:
            return ACT_SDK
        return 0

    def resolve_buttons(self):
        idx_of_node = {k: v[0] for k, v in self.button_of_node.items()}
        for e, ctl in self.hl_pending:  # (kind 5: follow = the control it highlights, -1 if it was not laid out)
            self.elems[e]['follow'], self.elems[e]['follow_mode'] = idx_of_node.get(id(ctl), -1), 5
        for key, (bi, b, n, vi, scene, kind_g, group, rect, lay, scale) in self.button_of_node.items():
            tg = (b.get('m_TargetGraphic') or {}).get('fileID', 0)
            gnode = scene.node_of_comp.get(tg)
            graphic = -1
            if gnode is not None and self.elems_of_node.get(id(gnode)):
                graphic = self.elems_of_node[id(gnode)][0]
            nav = b.get('m_Navigation') or {}

            def link(k, scene=scene):
                fid = (nav.get(k) or {}).get('fileID', 0)
                tn = scene.node_of_comp.get(fid)
                return idx_of_node.get(id(tn), -1) if tn is not None else -1
            cs = b.get('m_Colors') or {}
            kind = {G_BUTTON: KIND_BUTTON, G_TOGGLE: KIND_TOGGLE, G_SLIDER: KIND_SLIDER, G_DROPDOWN: KIND_DROPDOWN,
                    G_INPUTFIELD: KIND_INPUT}[kind_g]
            action = ACTIONS.get(n.name, 0) if kind == KIND_BUTTON else 0
            if kind == KIND_BUTTON:
                a = self.click_action(b, scene, n)
                if a:
                    action = a
                if id(n) in self.tab_of_button:
                    action = ACT_TAB + self.tab_of_button[id(n)]
                if n.name == 'ResetProgressButton':
                    action = ACT_RESET_PROGRESS
                if n.name == 'YesButton':
                    action = ACT_MODAL_YES
                if n.name == 'CancelButton' and n.parent is not None and n.parent.name == 'ChoiceModal':
                    action = ACT_MODAL_CANCEL
                if hasattr(n, 'row_index'):  # (the rows' buttons get their listeners from the populator)
                    action = {'RebindButton': ACT_ROW_REBIND, 'ResetButton': ACT_ROW_RESET,
                              'DeleteButton': ACT_ROW_CLEAR}.get(n.name, 0) + n.row_index
                if hasattr(n, 'choice_index'):
                    action = ACT_BIND_CHOICE + n.choice_index
            bt = dict(kind=kind, graphic=graphic, up=link('m_SelectOnUp'), down=link('m_SelectOnDown'),
                      left=link('m_SelectOnLeft'), right=link('m_SelectOnRight'),
                      nav_mode=int(nav.get('m_Mode', 3)), action=action, view=vi, group=group,
                      interactable=int(b.get('m_Interactable', 1)), setting=self.setting_of_node.get(id(n), 0),
                      normal=col(cs, 'm_NormalColor'), highlighted=col(cs, 'm_HighlightedColor'),
                      pressed=col(cs, 'm_PressedColor'), disabled=col(cs, 'm_DisabledColor'),
                      mult=float(cs.get('m_ColorMultiplier', 1)), name=n.name, rect=rect,
                      check=-1, fill=-1, handle=-1, farea=(0.0, 0.0), foff=(0.0, 0.0), harea=(0.0, 0.0),
                      hoff=(0.0, 0.0), vmin=0.0, vmax=1.0, whole=0, text=-1, decimals=0, postfix=-1, tscale=1.0,
                      caption=-1, nopt=0, opt0=-1, selectme=int(G_SELECTME in n.comp))
            if kind == KIND_TOGGLE:
                cn = scene.node_of_comp.get(int((b.get('graphic') or {}).get('fileID') or 0))
                if cn is not None and self.elems_of_node.get(id(cn)):
                    bt['check'] = self.elems_of_node[id(cn)][0]
                    for e in self.elems_of_node[id(cn)]:
                        self.elems[e]['vis'] = bi
            elif kind == KIND_SLIDER:
                bt['vmin'], bt['vmax'] = float(b.get('m_MinValue', 0)), float(b.get('m_MaxValue', 1))
                bt['whole'] = int(b.get('m_WholeNumbers', 0))
                for which, (tn, area, off, base) in getattr(n, 'slider_geom', {}).items():
                    if which == 'fill':
                        bt['farea'], bt['foff'] = area, off
                        for e in self.elems_of_node.get(id(tn), []):
                            self.elems[e]['follow'], self.elems[e]['follow_mode'] = bi, 1
                        bt['fill'] = self.elems_of_node.get(id(tn), [-1])[0]
                    else:
                        bt['harea'], bt['hoff'] = area, off
                        # (base: where its anchor sat when it was laid out)
                        for e in self.subtree_elems(tn):
                            self.elems[e]['follow'], self.elems[e]['follow_mode'] = bi, 2
                            self.elems[e]['follow_base'] = base
                        bt['handle'] = self.elems_of_node.get(id(tn), [-1])[0]
                hd = n.comp.get(G_HANDLE_DISPLAY)
                if hd:
                    ht = scene.node_of_comp.get(int((hd.get('HandleText') or {}).get('fileID') or 0))
                    if ht is not None and self.elems_of_node.get(id(ht)):
                        bt['text'] = [e for e in self.elems_of_node[id(ht)] if self.elems[e]['kind'] == 1][0]
                    bt['tscale'] = float(hd.get('ScaleFactor', 100))
                    bt['decimals'] = int(hd.get('DecimalPlaces', 0))
                    bt['postfix'] = self.string(str(hd.get('PostFix') if hd.get('PostFix') is not None else '%'))
            elif kind == KIND_INPUT:
                # the text shown (the scheme's name), and the placeholder shown while it is empty
                tn = scene.node_of_comp.get(int((b.get('m_TextComponent') or {}).get('fileID') or 0))
                if tn is not None and self.elems_of_node.get(id(tn)):
                    bt['text'] = [e for e in self.elems_of_node[id(tn)] if self.elems[e]['kind'] == 1][0]
                pn = scene.node_of_comp.get(int((b.get('m_Placeholder') or {}).get('fileID') or 0))
                if pn is not None and self.elems_of_node.get(id(pn)):
                    bt['check'] = self.elems_of_node[id(pn)][0]
                    for e in self.elems_of_node[id(pn)]:
                        self.elems[e]['vis'] = bi
            elif kind == KIND_DROPDOWN:
                cn = scene.node_of_comp.get(int((b.get('m_CaptionText') or {}).get('fileID') or 0))
                if cn is not None and self.elems_of_node.get(id(cn)):
                    bt['caption'] = [e for e in self.elems_of_node[id(cn)] if self.elems[e]['kind'] == 1][0]
            self.buttons[bi] = bt

    def slider_geom(self, b, scene, lay, k):
        out = {}
        for which, ref in (('fill', 'm_FillRect'), ('handle', 'm_HandleRect')):
            tn = self.node_of_tf_in(scene, ref, b)
            if tn is None or tn.parent is None:
                continue
            ax, _ay, aw, _ah = lay.rect(tn.parent)
            off0 = tn.pos[0] - tn.size[0] * tn.pivot[0]
            off1 = tn.pos[0] + tn.size[0] * (1 - tn.pivot[0])
            z = self.zoom
            out[which] = (tn, (self.zx(ax * k), self.zx((ax + aw) * k)), (off0 * k * z, off1 * k * z),
                          self.zx((ax + tn.amin[0] * aw) * k))
        return out

    def node_of_tf_in(self, scene, ref, b):
        return scene.node_of_tf.get(int((b.get(ref) or {}).get('fileID') or 0))

    def subtree_elems(self, n):
        out = list(self.elems_of_node.get(id(n), []))
        for c in n.children:
            out += self.subtree_elems(c)
        return out

    def bake_fonts(self):
        out = []
        for (guid, px), fid in sorted(self.font_ids.items(), key=lambda kv: kv[1]):
            fi = self.fonts[guid]
            f = fi.at(px)
            asc, desc = f.getmetrics()
            chars = sorted(self.font_chars[(guid, px)] | {' '})
            chars = [ch for ch in chars if ch != '\n']
            glyphs = []
            x = y = 1
            row_h = 0
            W = 256
            boxes = []
            for ch in chars:
                bb = f.getbbox(ch, anchor='ls')
                gw, gh = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
                if x + gw + 1 > W:
                    x, y = 1, y + row_h + 1
                    row_h = 0
                boxes.append((ch, x, y, gw, gh, bb[0], bb[1]))
                x += gw + 2
                row_h = max(row_h, gh)
            H = 1
            while H < y + row_h + 2:
                H *= 2
            atlas = Image.new('L', (W, H), 0)
            d = ImageDraw.Draw(atlas)
            for ch, gx, gy, gw, gh, bx, by in boxes:
                if gw and gh:
                    d.text((gx - bx, gy - by), ch, font=f, fill=255, anchor='ls')
                glyphs.append((ord(ch), f.getlength(ch), gx, gy, gw, gh, bx, by))
            out.append(dict(px=px, asc=float(asc), lh=float(asc + desc), glyphs=glyphs, atlas=atlas))
        return out

    def write(self, out_path):
        self.resolve_buttons()
        fonts = self.bake_fonts()
        self.baked_fonts = fonts
        b = bytearray(b'LSDU' + struct.pack('<I', 3))
        b += struct.pack('<I', len(self.views))
        for name, first, count in self.views:
            b += name.encode()[:23].ljust(24, b'\0') + struct.pack('<II', first, count)
        b += struct.pack('<I', len(self.elems))
        for e in self.elems:
            sh = (e['square'], 0.0, 0.0) if e['kind'] == 2 else e['shadow']
            b += struct.pack('<Iiii4f4fiiI3f', e['kind'], e['view'], e['tex'], e['font'], *e['rect'], *e['color'],
                             e['tag'], e['text'], e['align'], *sh)
            b += struct.pack('<iiiif3f', e.get('group', -1), e['vis'], e['follow'], e['follow_mode'],
                             e.get('follow_base', 0.0), e.get('rot', 0.0), *e.get('pivot', (0.0, 0.0)))
        b += struct.pack('<I', len(self.buttons))
        for bt in self.buttons:
            b += struct.pack('<12i17f4fi2i8f2f4if3i', bt['kind'], bt['graphic'], bt['up'], bt['down'], bt['left'],
                             bt['right'], bt['nav_mode'], bt['action'], bt['view'], bt['group'], bt['interactable'],
                             bt['setting'], *bt['normal'], *bt['highlighted'], *bt['pressed'], *bt['disabled'],
                             bt['mult'], *bt['rect'], bt['check'], bt['fill'], bt['handle'], *bt['farea'],
                             *bt['foff'], *bt['harea'], *bt['hoff'], bt['vmin'], bt['vmax'], bt['whole'], bt['text'],
                             bt['decimals'], bt['postfix'], bt['tscale'], bt['caption'], bt['selectme'], 0)
        b += struct.pack('<I', len(self.groups))
        for g in self.groups:
            b += struct.pack('<iiiifi4f4f', g['view'], g['kind'], g['tab'], g['visible'], g['height'], g['parent'],
                             *g['clip'], *g['bar'])
        b += struct.pack('<I', len(self.strings))
        for s in self.strings:
            sb = s.encode('utf-8')
            b += struct.pack('<I', len(sb)) + sb + b'\0' * ((4 - len(sb) % 4) % 4)
        b += struct.pack('<I', len(fonts))
        for f in fonts:
            b += struct.pack('<3fIII', f['px'], f['asc'], f['lh'], len(f['glyphs']), *f['atlas'].size)
            for g in f['glyphs']:
                b += struct.pack('<If6h', g[0], g[1], *g[2:])
            atl = f['atlas'].tobytes()
            b += atl + b'\0' * ((4 - len(atl) % 4) % 4)
        b += struct.pack('<I', len(self.textures))
        for _tpath, im, w, h in self.textures:
            # resized to what the screen shows (up to twice), then bottom row first
            tw = min(im.size[0], max(8, w * 2))
            th = min(im.size[1], max(8, h * 2))
            im2 = im.resize((tw, th), Image.LANCZOS) if (tw, th) != im.size else im
            b += struct.pack('<II', tw, th) + im2.transpose(Image.FLIP_TOP_BOTTOM).tobytes()
        with open(out_path, 'wb') as f:
            f.write(b)
        print('ui.bin: %d views, %d elements, %d controls, %d groups, %d fonts, %d textures, %d bytes' % (
            len(self.views), len(self.elems), len(self.buttons), len(self.groups), len(fonts), len(self.textures),
            len(b)))
        for i, bt in enumerate(self.buttons):
            print('  %3d %-24s %s view %d group %2d graphic %3d nav %d u%3d d%3d l%3d r%3d action %2d setting %2d%s' % (
                i, bt['name'], 'BTSDI'[bt['kind']], bt['view'], bt['group'], bt['graphic'], bt['nav_mode'], bt['up'],
                bt['down'], bt['left'], bt['right'], bt['action'], bt['setting'],
                '' if bt['interactable'] else ' (not interactable)'))

    def preview(self, d):
        """draw each view the way the game will (normal button colours); a view with game-shown panels is
        drawn again with each of them shown (and its tab)"""
        os.makedirs(d, exist_ok=True)
        for vi, (name, first, count) in enumerate(self.views):
            vis = [g['visible'] for g in self.groups]
            self.preview_one(d, 'ui_%s.png' % name, first, count, vis)
            for gi, g in enumerate(self.groups):
                if g['view'] != vi or g['kind'] != 2:
                    continue
                v2 = list(vis)
                chain, a = [], gi
                while a >= 0:
                    chain.append(a)
                    a = self.groups[a]['parent']
                for k, h in enumerate(self.groups):
                    if h['view'] == vi and h['kind'] in (0, 2):
                        v2[k] = int(k in chain)
                self.preview_one(d, 'ui_%s_panel%d.png' % (name, g['tab']), first, count, v2)

    def preview_one(self, d, fname, first, count, vis):
        im = Image.new('RGBA', (SCREEN_W, SCREEN_H), (24, 8, 66, 255))

        def shown(g):
            while g >= 0:
                if not vis[g]:
                    return False
                g = self.groups[g]['parent']
            return True
        for e in self.elems[first:first + count]:
            g = e.get('group', -1)
            if not shown(g):
                continue
            if e['kind'] > 1:
                continue
            if e['vis'] >= 0 and self.buttons[e['vis']]['kind'] != KIND_INPUT:
                continue
            tint = [1, 1, 1, 1]
            for bt in self.buttons:
                if bt['graphic'] == self.elems.index(e):
                    tint = [min(1, c * bt['mult']) for c in bt['normal']]
            c = [e['color'][i] * tint[i] for i in range(4)]
            x0, y0, x1, y1 = [int(v) for v in e['rect']]
            clip = self.groups[g]['clip'] if g >= 0 else (0, 0, 0, 0)
            if clip[2] > clip[0] and (y0 < clip[1] or y1 > clip[3]):
                continue
            if e['kind'] == 0:
                if x1 <= x0 or y1 <= y0:
                    continue
                if e['tex'] >= 0:
                    src = self.textures[e['tex']][1].resize((x1 - x0, y1 - y0), Image.LANCZOS)
                else:
                    src = Image.new('RGBA', (x1 - x0, y1 - y0), (255, 255, 255, 255))
                px = src.load()
                lay = Image.new('RGBA', src.size)
                lp = lay.load()
                for yy in range(src.size[1]):
                    for xx in range(src.size[0]):
                        r, gg, bb, a = px[xx, yy]
                        lp[xx, yy] = (int(r * c[0]), int(gg * c[1]), int(bb * c[2]), int(a * c[3]))
                im.alpha_composite(lay, (x0, y0))
            else:
                f = self.baked_fonts[e['font']]
                text = self.strings[e['text']]
                self.draw_text(im, f, text, e['rect'], e['align'], c, e['shadow'])
        im.save(os.path.join(d, fname))

    def draw_text(self, im, f, text, rect, align, c, shadow):
        x0, y0, x1, y1 = rect
        lines = text.split('\n')
        gl = {g[0]: g for g in f['glyphs']}
        widths = [sum(gl[ord(ch)][1] if ord(ch) in gl else 0 for ch in ln) for ln in lines]
        total_h = f['lh'] * len(lines)
        va, ha = align // 3, align % 3
        top = y0 if va == 0 else ((y0 + y1 - total_h) / 2 if va == 1 else y1 - total_h)
        for pas in ((shadow, (0, 0, 0, shadow[2] * c[3])), ((0, 0, 0), c)) if shadow[2] > 0 else (((0, 0, 0), c),):
            (dx, dy, _a), cc = pas
            for li, ln in enumerate(lines):
                x = x0 if ha == 0 else ((x0 + x1 - widths[li]) / 2 if ha == 1 else x1 - widths[li])
                base = top + li * f['lh'] + f['asc']
                for ch in ln:
                    g = gl.get(ord(ch))
                    if not g:
                        continue
                    _cp, adv, gx, gy, gw, gh, bx, by = g
                    if gw and gh:
                        glyph = f['atlas'].crop((gx, gy, gx + gw, gy + gh))
                        lay = Image.new('RGBA', (gw, gh), tuple(int(v * 255) for v in cc[:3]) + (0,))
                        a = glyph.point(lambda v, k=cc[3]: int(v * k))
                        lay.putalpha(a)
                        im.alpha_composite(lay, (int(round(x + bx + dx)), int(round(base + by + dy))))
                    x += adv


if __name__ == '__main__':
    main()
