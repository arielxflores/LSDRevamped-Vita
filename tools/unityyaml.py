"""Minimal reader for Unity's text-serialised YAML assets (scenes, prefabs,
ScriptableObjects, .meta files)."""
import os
import re
import yaml

_HDR = re.compile(r'^--- !u!(\d+) &(-?\d+)( stripped)?\s*$')


class _Loader(yaml.SafeLoader):
    pass


def _ignore_unknown(loader, suffix, node):
    if isinstance(node, yaml.MappingNode):
        return loader.construct_mapping(node)
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node)
    return loader.construct_scalar(node)


_Loader.add_multi_constructor('', _ignore_unknown)
# Unity writes booleans as 0/1: a text such as Yes, No, On or Off must stay a string
_Loader.yaml_implicit_resolvers = {
    k: [(tag, rx) for tag, rx in v if tag != 'tag:yaml.org,2002:bool']
    for k, v in yaml.SafeLoader.yaml_implicit_resolvers.items()}


class Doc:
    __slots__ = ('cls', 'fid', 'stripped', 'type', 'body')

    def __repr__(self):
        return '<%s %d &%d>' % (self.type, self.cls, self.fid)


def load(path):
    """Return {fileID: Doc} for a Unity YAML file."""
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    docs = {}
    cur = None
    buf = []

    def flush():
        if cur is None:
            return
        body = yaml.load(''.join(buf), Loader=_Loader) or {}
        if isinstance(body, dict) and len(body) == 1:
            cur.type, cur.body = next(iter(body.items()))
        else:
            cur.type, cur.body = '?', body
        if cur.body is None:
            cur.body = {}
        docs[cur.fid] = cur

    for line in text.splitlines(keepends=True):
        m = _HDR.match(line)
        if m:
            flush()
            cur = Doc()
            cur.cls = int(m.group(1))
            cur.fid = int(m.group(2))
            cur.stripped = bool(m.group(3))
            buf = []
        elif cur is not None:
            buf.append(line)
    flush()
    return docs


def load_single(path):
    """Load a ScriptableObject asset (one MonoBehaviour document)."""
    docs = load(path)
    for d in docs.values():
        if d.type == 'MonoBehaviour':
            return d.body
    raise ValueError('no MonoBehaviour in ' + path)


class GuidIndex:
    """guid -> asset path, built from a 'guid path' text index."""

    def __init__(self, index_file, project_root):
        self.root = project_root
        self.map = {}
        with open(index_file) as f:
            for line in f:
                g, p = line.rstrip('\n').split(' ', 1)
                self.map[g] = p

    def path(self, guid):
        p = self.map.get(guid)
        return None if p is None else os.path.join(self.root, p)

    def name(self, guid):
        p = self.map.get(guid)
        return None if p is None else os.path.basename(p)


def build_guid_index(project_root, out_file):
    with open(out_file, 'w') as out:
        for base in ('Assets', 'Packages'):
            for dp, _dn, fn in os.walk(os.path.join(project_root, base)):
                for f in fn:
                    if not f.endswith('.meta'):
                        continue
                    mp = os.path.join(dp, f)
                    with open(mp, encoding='utf-8', errors='replace') as m:
                        for line in m:
                            if line.startswith('guid: '):
                                rel = os.path.relpath(mp[:-5], project_root)
                                out.write('%s %s\n' % (line[6:].strip(), rel))
                                break


# ------------------------------------------------------------------ nested prefabs

_MASK = 0x7FFFFFFFFFFFFFFF


def _remap_refs(obj, idmap):
    """copy of a YAML body with {fileID: n} references (internal, no guid) renumbered"""
    if isinstance(obj, dict):
        if 'fileID' in obj and 'guid' not in obj and len(obj) <= 2:
            fid = int(obj.get('fileID') or 0)
            return {'fileID': idmap.get(fid, fid)}
        return {k: _remap_refs(v, idmap) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_remap_refs(v, idmap) for v in obj]
    return obj


def _set_path(body, path, value):
    """Unity property path ('m_AnchorMax.y', 'm_Options.m_Options.Array.data[2].m_Text', '...Array.size')"""
    parts = path.split('.')
    cur = body
    i = 0
    while i < len(parts):
        p = parts[i]
        last = i == len(parts) - 1
        if p == 'Array' and i + 1 < len(parts):
            nxt = parts[i + 1]
            if not isinstance(cur, list):
                return
            if nxt == 'size':
                n = int(value)
                while len(cur) < n:
                    cur.append({} if not cur or isinstance(cur[0], dict) else None)
                del cur[n:]
                return
            m = re.match(r'data\[(\d+)\]$', nxt)
            if not m:
                return
            k = int(m.group(1))
            while len(cur) <= k:
                cur.append({} if not cur or isinstance(cur[0], dict) else None)
            if i + 1 == len(parts) - 1:
                cur[k] = value
                return
            if not isinstance(cur[k], (dict, list)):
                cur[k] = {}
            cur = cur[k]
            i += 2
            continue
        if not isinstance(cur, dict):
            return
        if last:
            cur[p] = value
            return
        if not isinstance(cur.get(p), (dict, list)):
            # the next part says whether this is a list
            cur[p] = [] if parts[i + 1] == 'Array' else {}
        cur = cur[p]
        i += 1


def _scalar(v):
    if isinstance(v, str):
        try:
            return int(v)
        except ValueError:
            try:
                return float(v)
            except ValueError:
                return v
    return v


def load_resolved(path, guids, _depth=0):
    """load() with nested prefab instances expanded the way Unity instantiates them: the source
    prefab's objects get fileID (source ^ instance) & 0x7FFF..., its modifications are applied, it is
    parented under m_TransformParent, and objects the outer file added to it are attached."""
    docs = load(path)
    if _depth > 8:
        return docs
    for pi_fid, pi in [(f, d) for f, d in docs.items() if d.type == 'PrefabInstance']:
        src = (pi.body.get('m_SourcePrefab') or {})
        sp = guids.path(src.get('guid', ''))
        if not sp or not os.path.exists(sp):
            continue
        sdocs = load_resolved(sp, guids, _depth + 1)
        idmap = {f: (f ^ pi_fid) & _MASK for f in sdocs}
        imported = {}
        for f, d in sdocs.items():
            nd = Doc()
            nd.cls, nd.fid, nd.stripped, nd.type = d.cls, idmap[f], False, d.type
            nd.body = _remap_refs(d.body, idmap)
            imported[nd.fid] = nd
        mod = pi.body.get('m_Modification') or {}
        for m in mod.get('m_Modifications') or []:
            t = m.get('target') or {}
            tf = idmap.get(int(t.get('fileID') or 0))
            if tf is None or tf not in imported:
                continue
            ref = m.get('objectReference') or {}
            if int(ref.get('fileID') or 0) != 0:
                val = {'fileID': int(ref['fileID'])} if 'guid' not in ref else ref
            else:
                val = _scalar(m.get('value'))
            _set_path(imported[tf].body, m.get('propertyPath', ''), val)
        for rc in mod.get('m_RemovedComponents') or []:
            cf = idmap.get(int((rc or {}).get('fileID') or 0))
            if cf is None:
                continue
            imported.pop(cf, None)
            for d in imported.values():
                if d.type == 'GameObject':
                    d.body['m_Component'] = [c for c in d.body.get('m_Component') or []
                                             if int(c['component']['fileID']) != cf]
        # the instance's root transform goes under m_TransformParent
        parent = int((mod.get('m_TransformParent') or {}).get('fileID') or 0)
        for d in imported.values():
            if d.type in ('RectTransform', 'Transform') and int((d.body.get('m_Father') or {}).get('fileID') or 0) == 0:
                d.body['m_Father'] = {'fileID': parent}
        for f, d in imported.items():
            if f not in docs or docs[f].stripped:
                docs[f] = d
    # objects the outer file added to instantiated ones: components and child transforms
    for f, d in docs.items():
        if d.stripped or not isinstance(d.body, dict):
            continue
        go = int((d.body.get('m_GameObject') or {}).get('fileID') or 0)
        if go and go in docs and docs[go].type == 'GameObject':
            comps = docs[go].body.setdefault('m_Component', [])
            if not any(int(c['component']['fileID']) == f for c in comps):
                comps.append({'component': {'fileID': f}})
        if d.type in ('RectTransform', 'Transform'):
            fa = int((d.body.get('m_Father') or {}).get('fileID') or 0)
            if fa and fa in docs and docs[fa].type in ('RectTransform', 'Transform'):
                ch = docs[fa].body.setdefault('m_Children', [])
                if not any(int(c['fileID']) == f for c in ch):
                    ch.append({'fileID': f})
    return docs
