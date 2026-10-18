#!/usr/bin/env python3
"""LSD MML/MOM/MOS/TOD reader and a re-implementation of LSDR's MOM import
(MOMImporter.cs / MOMHelper.cs): builds the TOD object hierarchy, bakes each TOD
into keyframe tracks exactly like the importer, and leaves objects posed in the
first frame of the first animation.

Unity conventions: positions (x, -y, z) / 2048, scales / 4096, rotations in
1/4096 degree, quaternions (x, y, z, w), Hamilton product, Rotate() in self space.
"""
import math
import struct

import lsdfmt

MML_MAGIC = 0x204C4D4D
MOM_MAGIC = 0x204D4F4D
MOS_MAGIC = 0x20534F4D

T_ATTRIBUTE, T_COORD, T_TMDID, T_PARENT, T_MATRIX, T_TMDDATA, T_LIGHT, T_CAMERA, T_OBJCTL = range(9)


class Packet:
    __slots__ = ('obj', 'type', 'flag', 'data')


def _read_packet(data, p):
    obj, tf, _ln = struct.unpack_from('<HBB', data, p)
    p += 4
    pk = Packet()
    pk.obj, pk.type, pk.flag = obj, tf & 0xF, (tf >> 4) & 0xF
    t, f = pk.type, pk.flag
    if t == T_ATTRIBUTE:
        pk.data = struct.unpack_from('<II', data, p)
        p += 8
    elif t == T_COORD:
        d = {'diff': f & 1}
        if (f >> 1) & 1:
            d['rot'] = struct.unpack_from('<iii', data, p)
            p += 12
        if (f >> 2) & 1:
            d['scale'] = struct.unpack_from('<hhh', data, p)
            p += 8
        if (f >> 3) & 1:
            d['trans'] = struct.unpack_from('<iii', data, p)
            p += 12
        pk.data = d
    elif t in (T_TMDID, T_PARENT):
        pk.data = struct.unpack_from('<H', data, p)[0]
        p += 4
    elif t == T_MATRIX:
        p += 32
        pk.data = None
    elif t == T_TMDDATA:
        raise ValueError('TOD TMDData packet not supported')
    elif t == T_LIGHT:
        p += 16
        pk.data = None
    elif t == T_CAMERA:
        cam_type = f & 1
        n = 0
        if cam_type == 0:
            if (f >> 2) & 1:
                n += 24
            if (f >> 3) & 1:
                n += 4
        else:
            if (f >> 2) & 1:
                n += 12
            if (f >> 3) & 1:
                n += 12
        p += n
        pk.data = None
    elif t == T_OBJCTL:
        pk.data = f & 1  # 0 create, 1 kill
    else:
        raise ValueError('TOD packet type %d not supported' % t)
    return pk, p


def read_tod(data, p):
    ident, _ver, resolution, nframes = struct.unpack_from('<BBHI', data, p)
    if ident != 0x50:
        raise ValueError('bad TOD magic')
    p += 8
    frames = []
    for _ in range(max(0, nframes - 1)):      # libLSD reads NumberOfFrames - 1 frames
        _size, npk, _frame_no = struct.unpack_from('<HHI', data, p)
        p += 8
        pks = []
        for _ in range(npk):
            pk, p = _read_packet(data, p)
            pks.append(pk)
        frames.append(pks)
    return dict(resolution=resolution, frames=frames)


def read_mos(data, top):
    magic, n = struct.unpack_from('<II', data, top)
    if magic != MOS_MAGIC:
        raise ValueError('bad MOS magic')
    offs = struct.unpack_from('<%dI' % n, data, top + 8)
    return [read_tod(data, top + o) for o in offs]


def read_mom(data, top):
    magic, _length, tmd_off = struct.unpack_from('<III', data, top)
    if magic != MOM_MAGIC:
        raise ValueError('bad MOM magic')
    tods = read_mos(data, top + 12)
    tmd = lsdfmt.read_tmd(data, top + tmd_off)
    return dict(tods=tods, tmd=tmd)


def read_mml(data, top):
    magic, n = struct.unpack_from('<II', data, top)
    if magic != MML_MAGIC:
        raise ValueError('bad MML magic')
    offs = struct.unpack_from('<%dI' % n, data, top + 8)
    return [read_mom(data, top + o) for o in offs]


def lbd_moms(lbd_bytes):
    hdr = struct.unpack_from('<HHIIIII', lbd_bytes, 0)
    has_mml, mml_off = hdr[1] == 1, hdr[5]
    if not has_mml:
        return []
    return read_mml(lbd_bytes, mml_off)

# ------------------------------------------------------------------ Unity maths


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_axis(deg, axis):
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


RIGHT, UP, FWD = (1, 0, 0), (0, 1, 0), (0, 0, 1)


class Node:
    def __init__(self, name):
        self.name = name
        self.pos = (0.0, 0.0, 0.0)
        self.rot = (0.0, 0.0, 0.0, 1.0)
        self.scale = (1.0, 1.0, 1.0)
        self.children = []
        self.tparent = None       # actual transform parent
        self.aparent = None       # AnimationObject.Parent (only set by ParentObjectID packets)
        self.mesh = -1

    def set_tparent(self, p):
        if self.tparent is not None:
            self.tparent.children.remove(self)
        self.tparent = p
        p.children.append(self)

    def path(self):
        return self.name if self.aparent is None else self.aparent.path() + '/' + self.name


def apply_coord(node, d):
    if 'scale' in d:
        s = tuple(v / 4096.0 for v in d['scale'])
        node.scale = s if not d['diff'] else tuple(a * b for a, b in zip(node.scale, s))
    if 'trans' in d:
        tx, ty, tz = d['trans']
        t = (tx / 2048.0, -ty / 2048.0, tz / 2048.0)
        node.pos = t if not d['diff'] else tuple(a + b for a, b in zip(node.pos, t))
    if 'rot' in d:
        rx, ry, rz = d['rot']
        pitch, yaw, roll = -rx / 4096.0, ry / 4096.0, -rz / 4096.0
        if not d['diff']:
            node.rot = q_mul(q_mul(q_axis(pitch, RIGHT), q_axis(yaw, UP)), q_axis(roll, FWD))
        else:
            node.rot = q_mul(node.rot, q_axis(pitch, RIGHT))
            node.rot = q_mul(node.rot, q_axis(yaw, UP))
            node.rot = q_mul(node.rot, q_axis(roll, FWD))


def import_mom(mom):
    """Returns (root, clips). root is the MOM root node (named 'MOM'), with the TOD
    hierarchy (or bare meshes) under it, posed as the importer leaves it.
    clips: list of dict(length, tracks=[(path, [(t, pos, rot, scale)])])."""
    root = Node('MOM')
    tods = mom['tods']
    nmesh = len(mom['tmd'])
    has_anim = len(tods) > 0 and len(tods[0]['frames']) > 0
    if not has_anim:
        for i in range(nmesh):
            m = Node(str(i))
            m.mesh = i
            m.set_tparent(root)
        return root, []
    anim_root = Node('0')
    anim_root.set_tparent(root)
    table = {0: anim_root}
    first = tods[0]['frames'][0]
    # CreateAnimationObjectHierarchy
    for pk in first:
        if pk.type == T_OBJCTL and pk.data == 0:
            n = Node(str(pk.obj))
            n.set_tparent(anim_root)
            table[pk.obj] = n
        elif pk.type == T_TMDID:
            j = pk.data - 1
            m = Node(str(j))
            m.mesh = j
            m.set_tparent(anim_root)
            m.set_tparent(table[pk.obj])
        elif pk.type == T_PARENT:
            n = table[pk.obj]
            p = table[pk.data]
            n.aparent = p
            n.set_tparent(p)
    clips = []
    for tod in tods:
        per_obj = {}
        order = []
        for fno, frame in enumerate(tod['frames']):
            for pk in frame:
                if pk.type == T_COORD:
                    o = table[pk.obj]
                    if o not in per_obj:
                        per_obj[o] = []
                    per_obj[o].append((fno, pk.data))
        ft = tod['resolution'] / 60.0
        if ft == 0:
            ft = 4 / 60.0
        tracks = []
        length = 0.0
        for o in table.values():      # dictionary order = creation order, as in C#
            if o not in per_obj:
                continue
            keys = []
            for fno, d in per_obj[o]:
                apply_coord(o, d)
                t = fno * ft
                keys.append([t, o.pos, o.rot, o.scale])
                length = max(length, t)
            # AnimationClip.EnsureQuaternionContinuity
            for i in range(1, len(keys)):
                a, b = keys[i - 1][2], keys[i][2]
                if a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0:
                    keys[i][2] = tuple(-c for c in b)
            tracks.append((o.path(), [tuple(k) for k in keys]))
        clips.append(dict(length=length, frame_time=ft, tracks=tracks))
    # PoseObjectInFirstFrame
    for pk in first:
        if pk.type == T_COORD:
            apply_coord(table[pk.obj], pk.data)
    return root, clips


def dump(node, depth=0):
    print('  ' * depth, node.name, 'mesh' if node.mesh >= 0 else '', [round(c, 4) for c in node.pos],
          [round(c, 4) for c in node.rot], [round(c, 4) for c in node.scale])
    for c in node.children:
        dump(c, depth + 1)
