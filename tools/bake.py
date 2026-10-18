#!/usr/bin/env python3
"""Bake one LSD Revamped dream into the compact format the Vita viewer loads.

usage: bake.py --project <LSDRevamped/LSDR> --dream <dream .asset> --out <dir>

Reads the dream's Unity prefab (LBD chunk placement, spawn points), its
environment sequence (fog/sky per day), the LBD tile maps and TIX texture sets,
and writes:
  <out>/dream.bin      geometry batches, collision, environments, spawns
  <out>/tex_<SET>.bin  one 2048x512 RGBA8 VRAM atlas per texture set (A-D)
  <out>/report.json    what was baked (for checking)
"""
import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import io              # noqa: E402
import lfsget          # noqa: E402
import lsdfmt          # noqa: E402
import unityyaml       # noqa: E402
import numpy as np     # noqa: E402
from PIL import Image  # noqa: E402

MAGIC = b'LSDV'
VERSION = 4   # v4: + the collision grid. batch kind: 0 solid, 1 cutout, 2 alpha-blended, 3+i water material i; batches carry their LBD group
KIND_WATER0 = 3
CELL = 10            # tiles per batch cell edge (LBDs are 20x20 tiles)

# ------------------------------------------------------------ small maths


def qmat(q):
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def trs(pos, rot, scale):
    r = qmat(rot)
    return [[r[i][0] * scale[0], r[i][1] * scale[1], r[i][2] * scale[2], pos[i]] for i in range(3)] + [[0, 0, 0, 1]]


def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def xform(m, p):
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


IDENT = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def ry(deg):
    # Unity Quaternion.AngleAxis(deg, Vector3.up) as a matrix
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def translate(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def c32(f):
    return int(round(min(1.0, max(0.0, f)) * 255.0))

# ------------------------------------------------------------ Unity scene graph


class Scene:
    """World transforms for a prefab/scene file, including nested prefab
    instances (only their root transform overrides are applied)."""

    def __init__(self, docs):
        self.docs = docs
        self._world = {}

    def _vec(self, d, key, default):
        v = d.get(key)
        if not isinstance(v, dict):
            return default
        return tuple(float(v.get(k, dv)) for k, dv in zip('xyzw'[:len(default)], default))

    def _instance_local(self, pi):
        mods = pi['m_Modification'].get('m_Modifications') or []
        pos = [0.0, 0.0, 0.0]
        rot = [0.0, 0.0, 0.0, 1.0]
        scl = [1.0, 1.0, 1.0]
        for m in mods:
            path = m.get('propertyPath', '')
            val = m.get('value')
            try:
                val = float(val)
            except (TypeError, ValueError):
                continue
            for key, arr, comps in (('m_LocalPosition.', pos, 'xyz'), ('m_LocalRotation.', rot, 'xyzw'),
                                    ('m_LocalScale.', scl, 'xyz')):
                if path.startswith(key) and path[len(key):] in comps:
                    arr[comps.index(path[len(key):])] = val
        return trs(pos, rot, scl)

    def instance_world(self, pi_fid):
        key = ('pi', pi_fid)
        if key in self._world:
            return self._world[key]
        pi = self.docs[pi_fid].body
        parent = pi['m_Modification'].get('m_TransformParent', {}).get('fileID', 0)
        pw = self.world(parent) if parent else IDENT
        w = mmul(pw, self._instance_local(pi))
        self._world[key] = w
        return w

    def world(self, tfid):
        if tfid in self._world:
            return self._world[tfid]
        d = self.docs[tfid]
        if d.stripped:
            w = self.instance_world(d.body['m_PrefabInstance']['fileID'])
        else:
            b = d.body
            local = trs(self._vec(b, 'm_LocalPosition', (0.0, 0.0, 0.0)),
                        self._vec(b, 'm_LocalRotation', (0.0, 0.0, 0.0, 1.0)),
                        self._vec(b, 'm_LocalScale', (1.0, 1.0, 1.0)))
            father = (b.get('m_Father') or {}).get('fileID', 0)
            w = mmul(self.world(father), local) if father else local
        self._world[tfid] = w
        return w

    def transform_of_go(self, go_fid):
        go = self.docs[go_fid].body
        for c in go.get('m_Component', []):
            cid = c['component']['fileID']
            if cid in self.docs and self.docs[cid].type in ('Transform', 'RectTransform'):
                return cid
        return None

# ------------------------------------------------------------ assets


def material_info(guids, mat_guid):
    path = guids.path(mat_guid)
    docs = unityyaml.load(path)
    mat = next(d.body for d in docs.values() if d.type == 'Material')
    props = mat.get('m_SavedProperties', {})
    floats = {}
    for f in props.get('m_Floats', []):
        floats.update(f)
    texs = {}
    for t in props.get('m_TexEnvs', []):
        for k, v in t.items():
            g = (v.get('m_Texture') or {}).get('guid')
            if g:
                texs[k] = g
    colors = {}
    for c in props.get('m_Colors', []):
        for k, v in c.items():
            colors[k] = [float(v.get(ch, 1)) for ch in 'rgba']
    shader = guids.name(mat['m_Shader'].get('guid', '')) if mat.get('m_Shader') else None
    return dict(path=path, shader=shader, floats=floats, textures=texs, colors=colors)


def importer_settings(lbd_path):
    meta = unityyaml.yaml.safe_load(open(lbd_path + '.meta', encoding='utf-8'))
    return meta.get('ScriptedImporter', {})


def env_values(body):
    def col(k):
        c = body.get(k) or {}
        return [float(c.get('r', 1)), float(c.get('g', 1)), float(c.get('b', 1))]
    return dict(name=body.get('m_Name'), fog=col('FogColor'), subtractive=int(body.get('SubtractiveFog', 0)),
                fog_start=float(body.get('FogStartDistance', 10)), fog_end=float(body.get('FogEndDistance', 30)),
                fog_height=float(body.get('FogHeight', 0.24)), fog_gradient=float(body.get('FogGradient', 0.24)),
                sky=col('SkyColor'), sky_fog=col('SkyFogColor'))

# ------------------------------------------------------------ mesh building


def load_overrides(guids, settings):
    """The LBD importer's UVMaterialOverrides: [(rect x, y, w, h as float32, material info)]"""
    ref = settings.get('UVMaterialOverrides') or {}
    if not ref.get('guid') or int(ref.get('fileID', 0)) == 0:
        return []
    asset = unityyaml.load_single(guids.path(ref['guid']))
    out = []
    for o in asset.get('Overrides') or []:
        r = o['UVRect']
        rect = tuple(np.float32(float(r[k])) for k in ('x', 'y', 'width', 'height'))
        out.append((rect, material_info(guids, o['Material']['guid'])))
    return out


def _rect_contains(rect, uv):
    # UnityEngine.Rect.Contains: xMin <= x < xMax, yMin <= y < yMax (xMax = x + width as a float)
    x, y, w, h = rect
    xmax = np.float32(float(x) + float(w))
    ymax = np.float32(float(y) + float(h))
    return uv[0] >= x and uv[0] < xmax and uv[1] >= y and uv[1] < ymax


def tmd_object_tris(obj, overrides=()):
    """LibLSDUnity.MeshFromTMDObject for one TMD object.
    Returns (opaque_tris, alpha_tris, [override_tris per UV material override]);
    each tri is 3 x (pos, uv, rgba). Override UVs are mapped into the override's rect (0-1)."""
    opaque, alpha = [], []
    over = [[] for _ in overrides]
    for prim in obj.prims:
        if prim is None or prim.ptype != lsdfmt.TYPE_POLYGON:
            continue
        is_alpha = (prim.options & lsdfmt.OPT_ALPHA) != 0
        textured = (prim.options & lsdfmt.OPT_TEXTURED) != 0
        n = len(prim.verts)
        vs = []
        dst = alpha if is_alpha else opaque
        for i in range(n):
            x, y, z = obj.verts[prim.verts[i]]
            pos = (x / 2048.0, -y / 2048.0, z / 2048.0)
            col = (255, 255, 255, 255)
            if prim.colors:
                cr, cg, cb = prim.colors[i if len(prim.colors) > 1 else 0]
                col = [c32(cr), c32(cg), c32(cb), 255]
                if col[0] > 0 and col[1] > 0 and col[2] > 0 and not textured and is_alpha:
                    col[3] = 127
                col = tuple(col)
            uv = (1.0, 1.0)
            if prim.uvs:
                uv = lsdfmt.prim_uv(prim, i)
                if overrides:
                    luv = lsdfmt.prim_uv_lsdr(prim, i)
                    for oi, (rect, _m) in enumerate(overrides):
                        if _rect_contains(rect, luv):
                            # the primitive goes to the override's submesh; this vertex's UV is
                            # mapped into the rect (MapUVInRect), the others keep theirs
                            dst = over[oi]
                            uv = (float(np.float32(np.float32(luv[0] - rect[0]) / rect[2])),
                                  float(np.float32(np.float32(luv[1] - rect[1]) / rect[3])))
                            break
            vs.append((pos, uv, col))
        idx = [0, 1, 2]
        quad = (prim.options & lsdfmt.OPT_QUAD) != 0
        if quad:
            idx += [2, 1, 3]
        if prim.flag & lsdfmt.FLAG_DOUBLE_SIDED:
            idx += [1, 0, 2]
            if quad:
                idx += [1, 2, 3]
        for t in range(0, len(idx), 3):
            dst.append((vs[idx[t]], vs[idx[t + 1]], vs[idx[t + 2]]))
    return opaque, alpha, over


def collision_grid(tris, batches):
    """The collision triangles bucketed by 1x1 cells over XZ (what dream.c used to build at load):
    f32 gx0, gz0, cell; u32 gw, gh; u32 start[gw*gh+1]; u32 n; u32 tri[n] (ascending within a cell)."""
    f32 = np.float32
    lo = [min(f32(b.lo[a]) for b in batches) for a in range(3)]
    hi = [max(f32(b.hi[a]) for b in batches) for a in range(3)]
    cell = f32(1.0)
    gx0 = f32(math.floor(lo[0]) - 1.0)
    gz0 = f32(math.floor(lo[2]) - 1.0)
    gw = int(math.ceil(hi[0]) - gx0) + 2
    gh = int(math.ceil(hi[2]) - gz0) + 2
    if gw <= 0 or gh <= 0 or gw > 4096 or gh > 4096:
        raise SystemExit('collision grid %dx%d out of range' % (gw, gh))
    t = np.array([[c for v in tri for c in v] for tri in tris], dtype=f32).reshape(-1, 3, 3)
    tlo, thi = t.min(axis=1), t.max(axis=1)
    m = f32(0.1)
    x0 = np.clip(np.floor((tlo[:, 0] - m - gx0) / cell).astype(np.int64), 0, None)
    x1 = np.clip(np.floor((thi[:, 0] + m - gx0) / cell).astype(np.int64), None, gw - 1)
    z0 = np.clip(np.floor((tlo[:, 2] - m - gz0) / cell).astype(np.int64), 0, None)
    z1 = np.clip(np.floor((thi[:, 2] + m - gz0) / cell).astype(np.int64), None, gh - 1)
    nx = np.maximum(x1 - x0 + 1, 0)
    nz = np.maximum(z1 - z0 + 1, 0)
    cnt = nx * nz
    tri_id = np.repeat(np.arange(len(tris), dtype=np.int64), cnt)
    k = np.arange(int(cnt.sum()), dtype=np.int64) - np.repeat(np.cumsum(cnt) - cnt, cnt)
    nxr = np.repeat(nx, cnt)
    cx = np.repeat(x0, cnt) + k % nxr
    cz = np.repeat(z0, cnt) + k // nxr
    cid = cz * gw + cx
    order = np.argsort(cid, kind='stable')
    entries = tri_id[order].astype(np.uint32)
    start = np.zeros(gw * gh + 1, dtype=np.uint32)
    np.add.at(start, cid + 1, 1)
    start = np.cumsum(start).astype(np.uint32)
    return (struct.pack('<3f2I', gx0, gz0, cell, gw, gh) + start.tobytes() + struct.pack('<I', len(entries)) +
            entries.tobytes())


class Batch:
    def __init__(self, alpha, key):
        self.alpha = alpha
        self.key = key
        self.verts = []
        self.vmap = {}
        self.idx = []
        self.lo = [1e30] * 3
        self.hi = [-1e30] * 3

    def add_tri(self, tri):
        for v in tri:
            k = (tuple(round(c, 6) for c in v[0]), tuple(round(c, 7) for c in v[1]), v[2])
            i = self.vmap.get(k)
            if i is None:
                i = len(self.verts)
                self.vmap[k] = i
                self.verts.append(k)
                for a in range(3):
                    self.lo[a] = min(self.lo[a], k[0][a])
                    self.hi[a] = max(self.hi[a], k[0][a])
            self.idx.append(i)


def bake(args):
    proj = os.path.abspath(args.project)
    idx_file = os.path.join(args.out, 'guid_index.txt')
    os.makedirs(args.out, exist_ok=True)
    unityyaml.build_guid_index(proj, idx_file)
    guids = unityyaml.GuidIndex(idx_file, proj)

    dream = unityyaml.load_single(os.path.join(proj, args.dream))
    report = dict(dream=dream.get('Name'), author=dream.get('Author'))
    prefab = os.path.join(proj, 'Assets', 'Resources', dream['DreamPrefabPath'] + '.prefab')
    report['prefab'] = os.path.relpath(prefab, proj)
    docs = unityyaml.load(prefab)
    scene = Scene(docs)

    # environments (one per day of the sequence)
    envs = []
    seq_guid = (dream.get('Environments') or {}).get('guid')
    if seq_guid:
        seq = unityyaml.load_single(guids.path(seq_guid))
        for e in seq.get('Environments', []):
            envs.append(env_values(unityyaml.load_single(guids.path(e['guid']))))
    if not envs:
        envs.append(env_values({}))
    report['environments'] = len(envs)

    # player spawns
    spawn_script = next(g for g, p in guids.map.items() if p.endswith('/PlayerSpawn.cs'))
    spawns = []
    for d in docs.values():
        if d.type != 'MonoBehaviour' or d.stripped:
            continue
        if (d.body.get('m_Script') or {}).get('guid') != spawn_script:
            continue
        go = d.body['m_GameObject']['fileID']
        tf = scene.transform_of_go(go)
        w = scene.world(tf)
        pos = (w[0][3], w[1][3], w[2][3])
        fx, fz = w[0][2], w[2][2]
        yaw = math.degrees(math.atan2(fx, fz))
        name = docs[go].body.get('m_Name', '')
        flags = (1 if int(d.body.get('DayOneSpawn', 0)) else 0) | (2 if int(d.body.get('TunnelEntrance', 0)) else 0)
        spawns.append(dict(name=name, pos=pos, yaw=yaw, flags=flags))
    spawns.sort(key=lambda s: (-(s['flags'] & 1), s['name']))
    report['spawns'] = spawns

    # LBD chunks placed in the prefab (texture sets resolved from the first chunk's material)
    batches = {}
    col_tris = {}
    groups = []          # each LBD renderer's bounds (transparent draws are sorted by them)
    water_mats = []      # UV material override materials (LSDR/ClassicWater)
    water_index = {}
    tex_sets = None
    mat_report = None
    lbd_report = []
    tri_cache = {}
    for fid, d in docs.items():
        if d.type != 'PrefabInstance':
            continue
        src = d.body.get('m_SourcePrefab', {}).get('guid')
        path = guids.path(src)
        if not path or not path.upper().endswith('.LBD'):
            continue
        inst = scene.instance_world(fid)
        settings = importer_settings(path)
        if tex_sets is None:
            om = material_info(guids, settings['OpaqueMaterial']['guid'])
            am = material_info(guids, settings['TransparentMaterial']['guid'])
            mat_report = dict(opaque=om, alpha=am)
            tex_sets = {}
            for s in 'ABCD':
                g = om['textures'].get('_MainTex' + s) or om['textures'].get('_MainTex')
                tex_sets[s] = guids.path(g)
        collide = int(settings.get('Collision', 1)) == 1
        overrides = load_overrides(guids, settings)
        # global water material index of each of this LBD's overrides
        okinds = []
        for _rect, m in overrides:
            if m['path'] not in water_index:
                water_index[m['path']] = len(water_mats)
                water_mats.append(m)
            okinds.append(KIND_WATER0 + water_index[m['path']])
        lbd = lsdfmt.read_lbd(open(path, 'rb').read())
        name = os.path.splitext(os.path.basename(path))[0]
        group = len(lbd_report)
        lbd_report.append(dict(lbd=os.path.relpath(path, proj), pos=[inst[0][3], inst[1][3], inst[2][3]],
                               collision=collide, has_mml=lbd.has_mml, overrides=len(overrides)))
        glo, ghi = [1e30] * 3, [-1e30] * 3
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
                    tri_cache[key] = tmd_object_tris(lbd.tiles[t.type], overrides)
                opaque, alpha, over = tri_cache[key]
                m = mmul(inst, mmul(translate(x, -t.height, y), ry(t.dir * 90)))
                cell = (group, name, x // CELL, y // CELL)
                for kind, tris in [(0, opaque), (1, alpha)] + list(zip(okinds, over)):
                    if not tris:
                        continue
                    bkey = (kind,) + cell
                    b = batches.get(bkey)
                    if b is None:
                        b = batches[bkey] = Batch(kind, bkey)
                    for tri in tris:
                        wt = tuple((xform(m, v[0]), v[1], v[2]) for v in tri)
                        b.add_tri(wt)
                        for v in wt:
                            for a in range(3):
                                glo[a] = min(glo[a], v[0][a])
                                ghi[a] = max(ghi[a], v[0][a])
                        if collide:
                            pts = tuple(tuple(round(c, 5) for c in v[0]) for v in wt)
                            col_tris[tuple(sorted(pts))] = pts
        groups.append((glo, ghi))
    report['lbds'] = lbd_report

    # texture atlases
    tex_files = {}
    alpha_masks = []
    for s_, p in sorted(tex_sets.items()):
        tims = lsdfmt.read_tix(open(p, 'rb').read())
        atlas = lsdfmt.build_vram_atlas(tims)
        fn = 'tex_%s.bin' % s_
        with open(os.path.join(args.out, fn), 'wb') as f:
            f.write(atlas)
        tex_files[s_] = os.path.relpath(p, proj)
        a = np.frombuffer(atlas, dtype=np.uint8).reshape(lsdfmt.VRAM_H, lsdfmt.ATLAS_W, 4)[:, :, 3]
        alpha_masks.append(a <= 25)   # LSDR_CUTOUT_ALPHA: discard if alpha <= 0.1
    report['texture_sets'] = tex_files
    cut = np.zeros_like(alpha_masks[0])
    for m in alpha_masks:
        cut |= m
    # summed-area table: count of cutout texels in any rectangle
    sat = np.zeros((cut.shape[0] + 1, cut.shape[1] + 1), dtype=np.int64)
    sat[1:, 1:] = cut.astype(np.int64).cumsum(0).cumsum(1)

    def needs_cutout(tri):
        us = [v[1][0] * lsdfmt.ATLAS_W for v in tri]
        vs = [v[1][1] * lsdfmt.VRAM_H for v in tri]
        x0 = max(0, int(math.floor(min(us))) - 1)
        x1 = min(lsdfmt.ATLAS_W, int(math.ceil(max(us))) + 1)
        y0 = max(0, int(math.floor(min(vs))) - 1)
        y1 = min(lsdfmt.VRAM_H, int(math.ceil(max(vs))) + 1)
        if x1 <= x0 or y1 <= y0:
            return False
        return (sat[y1, x1] - sat[y0, x1] - sat[y1, x0] + sat[y0, x0]) > 0

    # split opaque batches into solid (no discard) and cutout
    kinds = {}
    for key, b in list(batches.items()):
        if b.alpha >= KIND_WATER0:
            kinds[key] = b
            continue
        if b.alpha:
            b.alpha = 2
            b.key = (2,) + key[1:]
            kinds[b.key] = b
            continue
        for t in range(0, len(b.idx), 3):
            tri = [b.verts[b.idx[t + k]] for k in range(3)]
            kind = 1 if needs_cutout(tri) else 0
            nk = (kind,) + key[1:]
            nb = kinds.get(nk)
            if nb is None:
                nb = kinds[nk] = Batch(kind, nk)
            nb.add_tri(tri)
    batches = kinds
    report['batch_kinds'] = {k: [sum(1 for b in batches.values() if b.alpha == i),
                                 sum(len(b.idx) // 3 for b in batches.values() if b.alpha == i)]
                             for i, k in enumerate(('solid', 'cutout', 'alpha'))}

    of = mat_report['opaque']['floats']
    brightness = float(of.get('_Brightness', 1.5))
    vci = float(of.get('_VertexColorIntensity', 2.0))
    cull = int(of.get('_Cull', 2))
    report['material'] = dict(opaque_shader=mat_report['opaque']['shader'], alpha_shader=mat_report['alpha']['shader'],
                              brightness=brightness, vertex_color_intensity=vci, cull=cull)

    # split batches that exceed 16-bit indices
    final = []
    for key in sorted(batches, key=str):
        b = batches[key]
        if len(b.verts) <= 65535:
            final.append(b)
            continue
        part = Batch(b.alpha, key)
        for t in range(0, len(b.idx), 3):
            if len(part.verts) > 65000:
                final.append(part)
                part = Batch(b.alpha, key)
            part.add_tri([b.verts[b.idx[t + k]] for k in range(3)])
        final.append(part)

    out = bytearray()
    out += MAGIC + struct.pack('<I', VERSION)
    out += struct.pack('<I', len(envs))
    for e in envs:
        out += struct.pack('<3fI4f3f3f', *e['fog'], e['subtractive'], e['fog_start'], e['fog_end'],
                           e['fog_height'], e['fog_gradient'], *e['sky'], *e['sky_fog'])
    out += struct.pack('<I', len(spawns))
    for s in spawns:
        out += struct.pack('<4fI', *s['pos'], s['yaw'], s['flags'])
    out += struct.pack('<2fI', brightness, vci, cull)
    out += struct.pack('<I', len(final))
    nv = ni = 0
    for b in final:
        out += struct.pack('<IIII6f', b.alpha, b.key[1], len(b.verts), len(b.idx), *b.lo, *b.hi)
        for (p, uv, c) in b.verts:
            out += struct.pack('<5f4B', *p, *uv, *c)
        out += struct.pack('<%dH' % len(b.idx), *b.idx)
        if len(b.idx) % 2:
            out += b'\0\0'
        nv += len(b.verts)
        ni += len(b.idx)
    # LBD renderer bounds
    out += struct.pack('<I', len(groups))
    for lo, hi in groups:
        out += struct.pack('<6f', *lo, *hi)
    # water materials (LSDR/ClassicWater) and their water map / palette textures
    images, image_index = [], {}

    def image(guid, palette):
        if guid not in image_index:
            rel = os.path.relpath(guids.path(guid), os.path.dirname(proj))
            im = Image.open(io.BytesIO(lfsget.fetch(proj, args.lfs_cache, rel))).convert('RGBA')
            if palette and im.size != (256, 1):
                raise SystemExit('unexpected water palette size %s' % (im.size,))
            im = im.transpose(Image.FLIP_TOP_BOTTOM)   # Unity's bottom-up rows
            image_index[guid] = len(images)
            images.append(im)
        return image_index[guid]

    out += struct.pack('<I', len(water_mats))
    wreport = []
    for m in water_mats:
        if m['shader'] not in ('ClassicWater.shader', 'RevampedWater.shader'):
            raise SystemExit('UV override material %s uses %s' % (m['path'], m['shader']))
        f = m['floats']
        tint = m['colors'].get('_Tint', [1, 1, 1, 1])
        maps = [image(m['textures']['_MainTexture' + s_], False) for s_ in 'ABCD']
        pals = [image(m['textures']['_WaterPalette' + s_], True) for s_ in 'ABCD']
        out += struct.pack('<3fI4f4i4i', float(f.get('_Alpha', 0.5)), float(f.get('_AnimationSpeed', 1)),
                           float(f.get('_Brightness', 1.5)), int(f.get('_Cull', 0)), *tint, *maps, *pals)
        wreport.append(dict(material=os.path.relpath(m['path'], proj), alpha=f.get('_Alpha'), cull=f.get('_Cull'),
                            maps=maps, palettes=pals))
    out += struct.pack('<I', len(images))
    for im in images:
        out += struct.pack('<II', *im.size) + im.tobytes()
    report['water'] = wreport
    report['water_triangles'] = sum(len(b.idx) // 3 for b in final if b.alpha >= KIND_WATER0)

    tris = list(col_tris.values())
    out += struct.pack('<I', len(tris))
    for t in tris:
        out += struct.pack('<9f', *t[0], *t[1], *t[2])
    out += collision_grid(tris, final)
    with open(os.path.join(args.out, 'dream.bin'), 'wb') as f:
        f.write(out)
    report['batches'] = len(final)
    report['vertices'] = nv
    report['indices'] = ni
    report['collision_triangles'] = len(tris)
    report['dream_bin_bytes'] = len(out)
    allv = [v[0] for b in final for v in b.verts]
    report['bounds'] = [[min(p[a] for p in allv) for a in range(3)], [max(p[a] for p in allv) for a in range(3)]]
    with open(os.path.join(args.out, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1, default=str)
    os.remove(idx_file)
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--dream', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--lfs-cache', default=os.environ.get('LSDR_LFS_CACHE', '.lfs-cache'))
    r = bake(ap.parse_args())
    print(json.dumps({k: r[k] for k in ('dream', 'batches', 'batch_kinds', 'vertices', 'indices', 'collision_triangles',
                                        'dream_bin_bytes', 'bounds', 'environments', 'water_triangles')}, indent=1))
    print('spawns:', [(s['name'], [round(c, 2) for c in s['pos']], round(s['yaw'], 1), s['flags']) for s in r['spawns']])


if __name__ == '__main__':
    main()
