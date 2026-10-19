#!/usr/bin/env python3
"""Export a dream prefab's entity scene for the Vita port.

usage: scene.py --project <LSDR> --dream <dream .asset> --data <data dir> [--journal <asset>]

Reads the prefab's GameObject hierarchy (everything except the LBD chunk instances),
the entity components on it, and the MOM models (meshes + baked TOD animations) the
entities use, and writes into <data>/<stgNN>/:
  entities.bin  (v2) nodes, components, strings
  models.bin    MOM meshes (solid/cutout/alpha batches) and animation clips
  scene.json    report
Converted Lua scripts go to <data>/scripts/.
"""
import argparse
import json
import math
import os
import tempfile
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake        # noqa: E402
import lsdfmt      # noqa: E402
import luafix      # noqa: E402
import mom as momlib  # noqa: E402
import unityyaml   # noqa: E402
import numpy as np  # noqa: E402

# sub-asset fileIDs LSDR's LBD importer gives each MOM's pieces (constant per identifier)
CONTROLLERS = {6077344094329684806: 0, -3363204436533838170: 1, -1322967940572544369: 2}
CLIPS = {-5221035980672895082: (0, 0), 2609645605654691667: (0, 1), 4819853848494224375: (0, 2),
         -1970849163090286690: (0, 3), 8564894844509618233: (1, 0), 657952904489353110: (1, 1),
         2509275852554096681: (2, 0), 2325114767772194991: (2, 1)}
_MESH_IDS = {
    0: [8000833496174367241, 554225713609026329, 4773014838954771858, 4332832924918431054, 814425935100885738,
        -5926715820632078454, 5884602064299754759, 8715632401468240974, 47359742771340991, -6518021635769937273,
        -4252856191660091562, 3217869932248902399, 6165490217511097854, -4000461167131436032,
        1099761113262661822, 7390723981978930992, 3151391480069526581],
    1: [-6609946886778231266, -6628216673660788205, -5817695593291365566, 6134980221943089227,
        5564276732318710561, -6390166771829375889, -3493283140697805720, -6303904791092144808,
        6300346459010798962],
    2: [633664023388945597, -5070249510684567155, 5683660052668897432, -5034727440954636888,
        8908328740089416836, -4283574816882517230]}
MESHES = {fid: (m, j) for m, ids in _MESH_IDS.items() for j, fid in enumerate(ids)}

T_INTERACTIVE, T_RUNNER, T_ANIMATED, T_TARGET, T_CHANCE, T_APPEAR_DAY, T_AUDIO, T_MOVIE, T_TRIGGER_LUA, \
    T_TRIGGER_LINK, T_SPAWN, T_GRAPH, T_PREFAB = range(1, 14)

SCRIPT_TYPES = {'InteractiveObject': T_INTERACTIVE, 'LuaAsyncActionRunner': T_RUNNER, 'AnimatedObject': T_ANIMATED,
                'Target': T_TARGET, 'ChanceToAppear': T_CHANCE, 'AppearOnDay': T_APPEAR_DAY, 'DreamAudio': T_AUDIO,
                'MovieClip': T_MOVIE, 'TriggerLua': T_TRIGGER_LUA, 'TriggerLink': T_TRIGGER_LINK,
                'PlayerSpawn': T_SPAWN, 'LBDGraphContributor': T_GRAPH, 'Prefab': T_PREFAB}
IGNORED_SCRIPTS = {'ShaderSetter'}


def fnv1a(s):
    if not s:
        return 0
    h = 0x811C9DC5
    for b in s.encode('utf-8'):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h or 1


class Strings:
    def __init__(self):
        self.buf = bytearray(b'\0')
        self.map = {'': 0}

    def add(self, s):
        s = '' if s is None else str(s)
        if s not in self.map:
            self.map[s] = len(self.buf)
            self.buf += s.encode('utf-8') + b'\0'
        return self.map[s]


def vec(b, key, default):
    v = b.get(key)
    if not isinstance(v, dict):
        return default
    return tuple(float(v.get(k, d)) for k, d in zip('xyzw'[:len(default)], default))


def script_name_of(guids, guid):
    p = guids.map.get(guid)
    return os.path.splitext(os.path.basename(p))[0] if p else None


def build_cutout_sat(data_dir):
    masks = []
    for s in 'ABCD':
        p = os.path.join(data_dir, 'tex_%s.bin' % s)
        if os.path.exists(p):
            a = np.fromfile(p, dtype=np.uint8).reshape(lsdfmt.VRAM_H, lsdfmt.ATLAS_W, 4)[:, :, 3]
            masks.append(a <= 25)
    cut = np.zeros_like(masks[0])
    for m in masks:
        cut |= m
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
    return needs_cutout


def mesh_batches(tmd_obj, needs_cutout):
    opaque, alpha = bake.tmd_object_tris(tmd_obj)[:2]
    kinds = {0: bake.Batch(0, 0), 1: bake.Batch(1, 1), 2: bake.Batch(2, 2)}
    for tri in opaque:
        kinds[1 if needs_cutout(tri) else 0].add_tri(tri)
    for tri in alpha:
        kinds[2].add_tri(tri)
    return [kinds[k] for k in (0, 1, 2) if kinds[k].idx]


def export(project, dream_asset, journal_asset, data_root):
    proj = os.path.abspath(project)
    idx_file = os.path.join(tempfile.gettempdir(), 'lsd_guid_index_s%d.txt' % os.getpid())
    unityyaml.build_guid_index(proj, idx_file)
    guids = unityyaml.GuidIndex(idx_file, proj)
    os.remove(idx_file)
    journal = unityyaml.load_single(os.path.join(proj, journal_asset))
    jindex = {d['guid']: i for i, d in enumerate(journal['Dreams'])}

    dream = unityyaml.load_single(os.path.join(proj, dream_asset))
    stg = re.search(r'(STG\d\d)', dream_asset).group(1).lower()
    out_dir = os.path.join(data_root, stg)
    prefab = os.path.join(proj, 'Assets', 'Resources', dream['DreamPrefabPath'] + '.prefab')
    docs = unityyaml.load(prefab)
    scene = bake.Scene(docs)
    script_types = {g: os.path.splitext(os.path.basename(p))[0] for g, p in guids.map.items() if p.endswith('.cs')}

    root_tf = next(f for f, d in docs.items() if d.type == 'Transform' and not d.stripped and
                   not (d.body.get('m_Father') or {}).get('fileID'))
    strings = Strings()
    ints = []
    nodes = []          # dicts
    comps = []
    tf_to_node = {}
    report = dict(dream=dream.get('Name'), unknown_scripts={}, warnings=[])

    models = {}         # (lbd path, mom index) -> index
    model_list = []
    lbd_cache = {}

    def model_index(lbd_guid, mom_idx):
        path = guids.path(lbd_guid)
        key = (path, mom_idx)
        if key not in models:
            if path not in lbd_cache:
                lbd_cache[path] = momlib.lbd_moms(open(path, 'rb').read())
            moms = lbd_cache[path]
            if mom_idx >= len(moms):
                raise ValueError('%s has %d MOMs, wanted %d' % (path, len(moms), mom_idx))
            models[key] = len(model_list)
            model_list.append(dict(name='%s/MOM%d' % (os.path.splitext(os.path.basename(path))[0], mom_idx),
                                   mom=moms[mom_idx]))
        return models[key]

    def walk(tf_fid, parent):
        d = docs[tf_fid]
        if d.stripped:
            return
        go_fid = d.body['m_GameObject']['fileID']
        go = docs[go_fid].body
        b = d.body
        ni = len(nodes)
        tf_to_node[tf_fid] = ni
        node = dict(parent=parent, name=str(go.get('m_Name', '')), pos=vec(b, 'm_LocalPosition', (0.0, 0.0, 0.0)),
                    rot=vec(b, 'm_LocalRotation', (0.0, 0.0, 0.0, 1.0)), scale=vec(b, 'm_LocalScale', (1.0, 1.0, 1.0)),
                    active=int(go.get('m_IsActive', 1)), mesh=-1, renderer=0, go=go_fid)
        nodes.append(node)
        mesh_ref = None
        for c in go.get('m_Component', []):
            cd = docs.get(c['component']['fileID'])
            if cd is None:
                continue
            cb = cd.body
            if cd.type == 'MeshFilter':
                mesh_ref = cb.get('m_Mesh') or {}
            elif cd.type == 'MeshRenderer':
                node['renderer'] = 1 | (2 if int(cb.get('m_Enabled', 1)) else 0)
            elif cd.type == 'Animator':
                node['animator'] = cb
            elif cd.type == 'BoxCollider':
                node['box'] = (vec(cb, 'm_Center', (0.0, 0.0, 0.0)), vec(cb, 'm_Size', (1.0, 1.0, 1.0)))
            elif cd.type == 'MonoBehaviour':
                st = script_types.get((cb.get('m_Script') or {}).get('guid'))
                if st in IGNORED_SCRIPTS:
                    continue
                if st not in SCRIPT_TYPES:
                    report['unknown_scripts'][st] = report['unknown_scripts'].get(st, 0) + 1
                    continue
                comps.append(dict(type=SCRIPT_TYPES[st], node=ni, body=cb, enabled=int(cb.get('m_Enabled', 1))))
        if mesh_ref and mesh_ref.get('fileID') in MESHES:
            m, j = MESHES[mesh_ref['fileID']]
            node['mesh'] = (model_index(mesh_ref['guid'], m) << 16) | j
            node['mesh_name_check'] = (node['name'] == str(j))
        elif mesh_ref and mesh_ref.get('fileID'):
            report['warnings'].append('unknown mesh %s on %s' % (mesh_ref, node['name']))
        for ch in b.get('m_Children', []):
            walk(ch['fileID'], ni)

    walk(root_tf, -1)

    # scripts
    scripts_dir = os.path.join(data_root, 'scripts')
    os.makedirs(scripts_dir, exist_ok=True)
    used_scripts = {}

    def script_ref(ref):
        g = (ref or {}).get('guid')
        if not g:
            return ''
        p = guids.path(g)
        if not p:
            return ''
        name = '%s/%s' % (stg, os.path.splitext(os.path.basename(p))[0])
        if name not in used_scripts:
            used_scripts[name] = p
            os.makedirs(os.path.join(scripts_dir, stg), exist_ok=True)
            with open(os.path.join(scripts_dir, name + '.lua'), 'w', encoding='utf-8') as f:
                f.write(luafix.convert(open(p, encoding='utf-8').read()))
        return name

    def clip_ref(ref):
        g = (ref or {}).get('guid')
        if not g:
            return ''
        p = guids.map.get(g, '')
        return re.sub(r'^Assets/Resources/', '', p)

    def world(ni):
        n = nodes[ni]
        local = bake.trs(n['pos'], n['rot'], n['scale'])
        return local if n['parent'] < 0 else bake.mmul(world(n['parent']), local)

    out_comps = []
    for c in comps:
        t, b, ni = c['type'], c['body'], c['node']
        ident = b.get('ID') if t not in (T_RUNNER, T_ANIMATED, T_CHANCE, T_APPEAR_DAY, T_GRAPH) else None
        words = []
        if t == T_INTERACTIVE:
            words = [strings.add(script_ref(b.get('Script'))), int(b.get('InteractionKind', 0)),
                     float(b.get('InteractionDistance', 3)), int(b.get('CallStartOnEnable', 0))]
            fmt = 'IIfI'
        elif t == T_ANIMATED:
            an = nodes[ni].get('animator') or {}
            ctrl = (an.get('m_Controller') or {})
            cm = CONTROLLERS.get(ctrl.get('fileID'))
            model = model_index(ctrl['guid'], cm) if cm is not None else -1
            clips = []
            for r in b.get('Clips') or []:
                mc = CLIPS.get(r.get('fileID'))
                if mc is None or cm is None or mc[0] != cm or r.get('guid') != ctrl.get('guid'):
                    report['warnings'].append('clip %s on %s does not belong to its controller' % (r, nodes[ni]['name']))
                    clips.append(-1)
                else:
                    clips.append(mc[1])
            clips = (clips + [-1] * 8)[:8]
            words = [model, len(b.get('Clips') or [])] + clips + [int(b.get('AutoPlay', -1)),
                                                                   int(an.get('m_Enabled', 1))]
            fmt = 'iI8iii'
        elif t == T_CHANCE:
            words, fmt = [int(b.get('OneIn', 1))], 'I'
        elif t == T_APPEAR_DAY:
            words, fmt = [int(b.get('Day', 0))], 'I'
        elif t == T_AUDIO:
            words = [strings.add(clip_ref(b.get('Clip'))), int(b.get('Spatial', 1)),
                     float(b.get('FullVolumeRangeUnits', 3)), float(b.get('RangeUnits', 10)), float(b.get('Pitch', 1)),
                     float(b.get('Volume', 1)), float(b.get('DelayBetweenPlaysSeconds', 1)),
                     float(b.get('PlayClipForSeconds', 0)), float(b.get('DelayBeforePlayingSeconds', 0)),
                     int(b.get('WaitUntilAudibleBeforePlaying', 1)), int(b.get('Loop', 0)), int(b.get('OneShot', 0)),
                     int(b.get('ControlledWithScript', 0))]
            fmt = 'IIfffffffIIII'
        elif t == T_MOVIE:
            words, fmt = [strings.add(clip_ref(b.get('Clip')))], 'I'
        elif t == T_TRIGGER_LUA:
            box = nodes[ni].get('box', ((0.0, 0.0, 0.0), (1.0, 1.0, 1.0)))
            words = [strings.add(script_ref(b.get('Script'))), strings.add(b.get('TriggerFunctionName') or ''),
                     strings.add(b.get('TriggerExitFunctionName') or ''), int(b.get('OnlyOnce', 0))] + \
                list(box[0]) + list(box[1])
            fmt = 'IIII6f'
        elif t == T_TRIGGER_LINK:
            box = nodes[ni].get('box', ((0.0, 0.0, 0.0), (1.0, 1.0, 1.0)))
            linked = (b.get('Linked') or {}).get('guid')
            spid = (b.get('SpawnPointEntityID') or '').strip()
            fl = ((1 if int(b.get('ForceFadeColor', 0)) else 0) | (2 if int(b.get('PlayLinkSound', 0)) else 0) |
                  (4 if int(b.get('LockInput', 0)) else 0) | (8 if int(b.get('AffectsLinksWithin', 0)) else 0) |
                  (16 if int(b.get('OnlyOnce', 0)) else 0))
            col = b.get('ForcedLinkColor') or {}
            words = [jindex.get(linked, -1) if linked else -1, fnv1a(spid), fl] + \
                [float(col.get(k, 0)) for k in 'rgb'] + list(box[0]) + list(box[1])
            fmt = 'iII3f6f'
        elif t == T_SPAWN:
            fl = (1 if int(b.get('DayOneSpawn', 0)) else 0) | (2 if int(b.get('TunnelEntrance', 0)) else 0)
            words, fmt = [fl, fnv1a(b.get('ID') or nodes[ni]['name'])], 'II'
        elif t == T_GRAPH:
            data = [(int(e['_vector']['x']), int(e['_vector']['y'])) for e in b.get('LBDGraphData', [])]
            words = [int(b.get('LayoutType', 0)), int(b.get('TileWidth', 1)), len(ints), len(data)]
            for dyn, up in data:
                ints.extend([dyn, up])
            fmt = 'IiII'
        else:
            fmt = ''
        out_comps.append((t, ni, strings.add(ident or ''), c['enabled'], fmt, words))

    # ---- models.bin
    needs_cutout = build_cutout_sat(out_dir)
    mb = bytearray(b'LSDM' + struct.pack('<II', 1, len(model_list)))
    model_report = []
    for m in model_list:
        root, clips = momlib.import_mom(m['mom'])
        nm = m['name'].encode()[:31]
        mb += nm.ljust(32, b'\0')
        tmd = m['mom']['tmd']
        mb += struct.pack('<I', len(tmd))
        ntri = 0
        for obj in tmd:
            batches = mesh_batches(obj, needs_cutout)
            mb += struct.pack('<I', len(batches))
            for bt in batches:
                mb += struct.pack('<III6f', bt.alpha, len(bt.verts), len(bt.idx), *bt.lo, *bt.hi)
                for (p, uv, col) in bt.verts:
                    mb += struct.pack('<5f4B', *p, *uv, *col)
                mb += struct.pack('<%dH' % len(bt.idx), *bt.idx)
                if len(bt.idx) % 2:
                    mb += b'\0\0'
                ntri += len(bt.idx) // 3
        mb += struct.pack('<I', len(clips))
        for cl in clips:
            mb += struct.pack('<fI', cl['length'], len(cl['tracks']))
            for path, keys in cl['tracks']:
                pb = path.encode()
                mb += struct.pack('<I', len(pb)) + pb + b'\0' * ((4 - len(pb) % 4) % 4)
                mb += struct.pack('<I', len(keys))
                for t, p, r, s in keys:
                    mb += struct.pack('<11f', t, *p, *r, *s)
        model_report.append(dict(name=m['name'], meshes=len(tmd), tris=ntri,
                                 clips=[round(c['length'], 2) for c in clips]))
    with open(os.path.join(out_dir, 'models.bin'), 'wb') as f:
        f.write(mb)

    # ---- entities.bin v2
    name_offs = [strings.add(n['name']) for n in nodes]
    eb = bytearray(b'LSDE' + struct.pack('<I', 2))
    name = (dream.get('Name') or '').encode('utf-8')[:63]
    eb += name.ljust(64, b'\0')
    sb = bytes(strings.buf) + b'\0' * ((4 - len(strings.buf) % 4) % 4)
    eb += struct.pack('<I', len(sb)) + sb
    eb += struct.pack('<I', len(ints)) + struct.pack('<%di' % len(ints), *ints)
    eb += struct.pack('<I', len(nodes))
    for n, noff in zip(nodes, name_offs):
        flags = (1 if n['active'] else 0) | ((n['renderer'] & 3) << 1)
        eb += struct.pack('<iI3f4f3fIi', n['parent'], noff, *n['pos'], *n['rot'], *n['scale'], flags, n['mesh'])
    eb += struct.pack('<I', len(out_comps))
    for t, ni, idoff, en, fmt, words in out_comps:
        rec = struct.pack('<IiII', t, ni, idoff, en) + struct.pack('<' + fmt, *words)
        rec += b'\0' * (72 - len(rec))
        assert len(rec) == 72, (t, len(rec))
        eb += rec
    with open(os.path.join(out_dir, 'entities.bin'), 'wb') as f:
        f.write(eb)

    counts = {}
    for t, *_ in out_comps:
        counts[t] = counts.get(t, 0) + 1
    names = {v: k for k, v in SCRIPT_TYPES.items()}
    report.update(nodes=len(nodes), components={names[k]: v for k, v in sorted(counts.items())},
                  models=model_report, scripts=sorted(used_scripts), bytes=len(eb), model_bytes=len(mb),
                  mesh_name_mismatch=[n['name'] for n in nodes if n.get('mesh_name_check') is False])
    with open(os.path.join(out_dir, 'scene.json'), 'w') as f:
        json.dump(report, f, indent=1)
    return report


def write_includes(project, journal_asset, data_root):
    proj = os.path.abspath(project)
    inc = os.path.join(proj, 'Assets/Resources/Original/LuaInclude')
    d = os.path.join(data_root, 'scripts', 'include')
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(inc):
        if f.endswith('.lua'):
            with open(os.path.join(d, f), 'w', encoding='utf-8') as o:
                o.write(luafix.convert(open(os.path.join(inc, f), encoding='utf-8').read()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--dream', required=True)
    ap.add_argument('--journal', default='Assets/Resources/Original/OriginalDreams.asset')
    ap.add_argument('--data', required=True)
    a = ap.parse_args()
    write_includes(a.project, a.journal, a.data)
    r = export(a.project, a.dream, a.journal, a.data)
    print(json.dumps({k: r[k] for k in ('dream', 'nodes', 'components', 'bytes', 'model_bytes', 'unknown_scripts',
                                        'warnings', 'mesh_name_mismatch')})[:2000])


if __name__ == '__main__':
    main()
