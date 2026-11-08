#!/usr/bin/env python3
# Makes the face avatar's gaming hands' own keyboard, mouse, mouse mat, game
# pad and pad stick (data/face/gaming/*.glb) from plain shapes - our own
# design, no letters on the keys. Each in metres, as gamehands.hpp has
# them: x its left (as it faces the character), y up, z away from the
# character; each made round where gamehands.cpp puts it.
#
#   python3 tools/make-gaming-props.py [out_dir]
#
# Needs numpy. The same files every run.
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'data', 'face', 'gaming')

U = 0.01905  # a key's width (19.05 mm)


def srgb(hex_colour):
    # A colour as people pick it, made linear (glTF's way).
    c = [int(hex_colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]


class Mesh:
    """Triangles by material: each a list of points, normals and corners."""

    def __init__(self):
        self.parts = {}

    def add(self, material, pos, nrm, tris):
        pos = np.asarray(pos, np.float32).reshape(-1, 3)
        nrm = np.asarray(nrm, np.float32).reshape(-1, 3)
        tris = np.asarray(tris, np.int64).reshape(-1, 3)
        p = self.parts.setdefault(material, [[], [], [], 0])
        p[0].append(pos)
        p[1].append(nrm)
        p[2].append(tris + p[3])
        p[3] += len(pos)


def unit(v):
    v = np.asarray(v, np.float64)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-12)


def grid_tris(rows, cols, flip=False):
    t = []
    for r in range(rows - 1):
        for c in range(cols - 1):
            a, b, d, e = r * cols + c, r * cols + c + 1, (r + 1) * cols + c, (r + 1) * cols + c + 1
            if flip:
                t += [(a, b, d), (b, e, d)]
            else:
                t += [(a, d, b), (b, d, e)]
    return t


def face_normals_fix(pos, tris, nrm):
    # Turns each triangle to face the way its points' normals do (its
    # corners anticlockwise seen from outside, as glTF has them).
    out = []
    for a, b, c in tris:
        n = np.cross(pos[b] - pos[a], pos[c] - pos[a])
        if np.dot(n, nrm[a] + nrm[b] + nrm[c]) < 0:
            out.append((a, c, b))
        else:
            out.append((a, b, c))
    return out


def rounded_box(mesh, material, centre, size, radius, arc=3, taper=1.0, skip=(), split=None):
    """A box with rounded edges: `size` across (x, y, z), `radius` round its
    edges; `taper` its top that much as wide and deep as its bottom; faces
    in `skip` ('-y' its bottom...) left out. `split(point)` -> material
    picks each triangle's material by its middle."""
    h = np.array(size, np.float64) / 2
    r = min(radius, *h)
    inner = h - r
    centre = np.array(centre, np.float64)

    def coords(i):
        ks = [r * math.tan((k + 1) / arc * math.pi / 4) for k in range(arc)]
        neg = [-inner[i] - k for k in reversed(ks)]
        return neg + [-inner[i], inner[i]] + [inner[i] + k for k in ks]

    for axis in range(3):
        for sign in (-1, 1):
            name = ('+' if sign > 0 else '-') + 'xyz'[axis]
            if name in skip:
                continue
            a, b = [i for i in range(3) if i != axis]
            ca, cb = coords(a), coords(b)
            pts, nrms = [], []
            for va in ca:
                for vb in cb:
                    p = np.zeros(3)
                    p[axis] = sign * (inner[axis] + r)
                    p[a], p[b] = va, vb
                    q = np.clip(p, -inner, inner)
                    n = unit(p - q)
                    at = q + n * r
                    pts.append(at)
                    nrms.append(n)
            pts = np.array(pts)
            nrms = np.array(nrms)
            if taper != 1.0:
                t = (pts[:, 1] + h[1]) / (2 * h[1])
                k = 1 + (taper - 1) * t
                pts[:, 0] *= k
                pts[:, 2] *= k
            pts += centre
            tris = face_normals_fix(pts, grid_tris(len(ca), len(cb)), nrms)
            if split is None:
                mesh.add(material, pts, nrms, tris)
            else:
                by = {}
                for tri in tris:
                    m = split(pts[list(tri)].mean(axis=0)) or material
                    by.setdefault(m, []).append(tri)
                for m, ts in by.items():
                    mesh.add(m, pts, nrms, ts)


def lathe(mesh, material, profile, centre=(0, 0, 0), axis_turn=None, seg=32, split=None):
    """A shape turned round its y axis: `profile` (radius, height) from its
    bottom middle up to its top middle; turned by the 3x3 `axis_turn`, then
    moved to `centre`."""
    prof = np.array(profile, np.float64)
    # Each point's normal: across its profile's way there.
    tang = np.zeros_like(prof)
    tang[1:-1] = prof[2:] - prof[:-2]
    tang[0] = prof[1] - prof[0]
    tang[-1] = prof[-1] - prof[-2]
    n2 = unit(np.stack([tang[:, 1], -tang[:, 0]], axis=1))
    pts, nrms = [], []
    for i in range(seg + 1):
        a = 2 * math.pi * i / seg
        c, s = math.cos(a), math.sin(a)
        for (rad, y), (nr, ny) in zip(prof, n2):
            pts.append((rad * c, y, rad * s))
            nrms.append((nr * c, ny, nr * s))
    pts = np.array(pts)
    nrms = unit(np.array(nrms))
    if axis_turn is not None:
        R = np.array(axis_turn, np.float64)
        pts = pts @ R.T
        nrms = nrms @ R.T
    pts += np.array(centre, np.float64)
    tris = face_normals_fix(pts, grid_tris(seg + 1, len(prof)), nrms)
    if split is None:
        mesh.add(material, pts, nrms, tris)
    else:
        by = {}
        for tri in tris:
            m = split(pts[list(tri)].mean(axis=0)) or material
            by.setdefault(m, []).append(tri)
        for m, ts in by.items():
            mesh.add(m, pts, nrms, ts)


def turn_to(direction):
    """A 3x3 turn taking +y to `direction`."""
    d = unit(direction)
    y = np.array([0, 1.0, 0])
    v = np.cross(y, d)
    c = float(np.dot(y, d))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else np.diag([1, -1, -1.0])
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


def rounded_slab(mesh, material, centre, w, d, thick, corner, top_material=None, seg=8):
    """A flat slab, its corners rounded in plan (`corner`), its top at
    centre.y + thick/2."""
    outline = []
    for cx, cz, a0 in ((w / 2 - corner, d / 2 - corner, 0), (-w / 2 + corner, d / 2 - corner, 90),
                       (-w / 2 + corner, -d / 2 + corner, 180), (w / 2 - corner, -d / 2 + corner, 270)):
        for k in range(seg + 1):
            a = math.radians(a0 + 90 * k / seg)
            outline.append((cx + corner * math.cos(a), cz + corner * math.sin(a)))
    cx, cy, cz = centre
    top, bot = cy + thick / 2, cy - thick / 2
    n = len(outline)
    # Top and bottom: fans from the middle.
    for y, ny, mat in ((top, 1, top_material or material), (bot, -1, material)):
        pts = [(cx, y, cz)] + [(cx + x, y, cz + z) for x, z in outline]
        nrm = [(0, ny, 0)] * (n + 1)
        tris = [(0, 1 + i, 1 + (i + 1) % n) for i in range(n)]
        mesh.add(mat, pts, nrm, face_normals_fix(np.array(pts), tris, np.array(nrm, np.float64)))
    # Its edge.
    pts, nrm = [], []
    for i in range(n + 1):
        x, z = outline[i % n]
        px, pz = outline[(i - 1) % n]
        qx, qz = outline[(i + 1) % n]
        e = unit([qz - pz, 0, -(qx - px)])
        if np.dot(e, [x, 0, z]) < 0:
            e = -e
        pts += [(cx + x, top, cz + z), (cx + x, bot, cz + z)]
        nrm += [tuple(e), tuple(e)]
    tris = []
    for i in range(n):
        a, b, c, d2 = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
        tris += [(a, b, c), (c, b, d2)]
    mesh.add(material, pts, nrm, face_normals_fix(np.array(pts), tris, np.array(nrm, np.float64)))
    return outline


def ring_strip(mesh, material, outline, centre, y, inset, width):
    """A thin band on a flat top, `inset` in from its outline."""
    cx, cz = centre[0], centre[2]
    o = np.array(outline)
    m = o.mean(axis=0)
    pts, nrm = [], []
    for x, z in o:
        v = unit([x - m[0], z - m[1]])
        p_out = np.array([x, z]) - v * inset
        p_in = p_out - v * width
        pts += [(cx + p_out[0], y, cz + p_out[1]), (cx + p_in[0], y, cz + p_in[1])]
        nrm += [(0, 1, 0), (0, 1, 0)]
    n = len(o)
    tris = []
    for i in range(n):
        a, b, c, d = 2 * i, 2 * i + 1, 2 * ((i + 1) % n), 2 * ((i + 1) % n) + 1
        tris += [(a, b, c), (c, b, d)]
    mesh.add(material, pts, nrm, face_normals_fix(np.array(pts), tris, np.array(nrm, np.float64)))


# ---- Materials ------------------------------------------------------------------

ACCENT = srgb('#2fd6e6')


def mat(name, colour, rough=0.6, metal=0.0, glow=0.0):
    m = {'name': name, 'pbrMetallicRoughness': {'baseColorFactor': colour + [1.0], 'metallicFactor': metal,
                                                 'roughnessFactor': rough}}
    if glow > 0:
        m['emissiveFactor'] = [c * glow for c in colour]
    return m


MATERIALS = {
    'case': mat('case', srgb('#1d2127'), rough=0.45, metal=0.55),
    'body': mat('body', srgb('#16191e'), rough=0.55),
    'grip': mat('grip', srgb('#101215'), rough=0.85),
    'keys': mat('keys', srgb('#2a2e35'), rough=0.62),
    'keys_side': mat('keys_side', srgb('#22262c'), rough=0.62),
    'keys_mark': mat('keys_mark', srgb('#3c4450'), rough=0.55),
    'buttons': mat('buttons', srgb('#0d0f12'), rough=0.35),
    'mat_top': mat('mat_top', srgb('#15171b'), rough=0.9),
    'mat_base': mat('mat_base', srgb('#0b0c0e'), rough=0.95),
    'accent': mat('accent', ACCENT, rough=0.4, glow=0.85),
    'groove': mat('groove', srgb('#08090b'), rough=0.8),
    'wheel': mat('wheel', srgb('#3a3f47'), rough=0.5, metal=0.3),
}


def write_glb(mesh, path, name):
    bin_parts, views, accessors, prims = [], [], [], []
    offset = 0
    materials = []

    def add_view(data, target):
        nonlocal offset
        pad = (-len(data)) % 4
        bin_parts.append(data + b'\0' * pad)
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data), 'target': target})
        offset += len(data) + pad
        return len(views) - 1

    for mname in sorted(mesh.parts):
        pos_l, nrm_l, tri_l, _ = mesh.parts[mname]
        pos = np.concatenate(pos_l).astype(np.float32)
        nrm = unit(np.concatenate(nrm_l)).astype(np.float32)
        tris = np.concatenate(tri_l)
        big = len(pos) > 65535
        idx = tris.astype(np.uint32 if big else np.uint16).reshape(-1)
        pv = add_view(pos.tobytes(), 34962)
        nv = add_view(nrm.tobytes(), 34962)
        iv = add_view(idx.tobytes(), 34963)
        accessors.append({'bufferView': pv, 'componentType': 5126, 'count': len(pos), 'type': 'VEC3',
                          'min': [float(x) for x in pos.min(axis=0)], 'max': [float(x) for x in pos.max(axis=0)]})
        accessors.append({'bufferView': nv, 'componentType': 5126, 'count': len(nrm), 'type': 'VEC3'})
        accessors.append({'bufferView': iv, 'componentType': 5125 if big else 5123, 'count': len(idx),
                          'type': 'SCALAR'})
        materials.append(MATERIALS[mname])
        prims.append({'attributes': {'POSITION': len(accessors) - 3, 'NORMAL': len(accessors) - 2},
                      'indices': len(accessors) - 1, 'material': len(materials) - 1})
    blob = b''.join(bin_parts)
    doc = {
        'asset': {'version': '2.0', 'generator': 'DireBot for OBS tools/make-gaming-props.py',
                  'copyright': 'DireBot for OBS (GPL-2.0-or-later)'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'name': name, 'mesh': 0}],
        'meshes': [{'name': name, 'primitives': prims}],
        'materials': materials,
        'buffers': [{'byteLength': len(blob)}],
        'bufferViews': views,
        'accessors': accessors,
    }
    js = json.dumps(doc, separators=(',', ':')).encode()
    js += b' ' * ((-len(js)) % 4)
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js
    out += struct.pack('<II', len(blob), 0x004E4942) + blob
    with open(path, 'wb') as f:
        f.write(out)
    return len(out)


# ---- The keyboard: a tenkeyless one, its middle on the desk at 0 -------------------

def keyboard():
    m = Mesh()
    # Its keys, row by row from the far one (the function keys) to the
    # space bar, each (width in keys, gap before it): its left (+x) first.
    rows = [
        [(1, 0), (1, 1), (1, 0), (1, 0), (1, 0), (1, 0.5), (1, 0), (1, 0), (1, 0), (1, 0.5), (1, 0), (1, 0), (1, 0)],
        [(1, 0)] * 13 + [(2, 0)],
        [(1.5, 0)] + [(1, 0)] * 12 + [(1.5, 0)],
        [(1.75, 0)] + [(1, 0)] * 11 + [(2.25, 0)],
        [(2.25, 0)] + [(1, 0)] * 10 + [(2.75, 0)],
        [(1.25, 0)] * 3 + [(6.25, 0)] + [(1.25, 0)] * 4,
    ]
    nav = [[1, 1, 1], [1, 1, 1], [1, 1, 1], None, [None, 1, None], [1, 1, 1]]
    width = 18.5 * U + 0.018  # its case
    depth = 6.25 * U + 0.018
    case_h = 0.022
    # The case: a low slab, its top a little higher at the back.
    rounded_box(m, 'case', (0, case_h / 2, 0), (width, case_h, depth), 0.004, arc=3)
    # A glowing band along its near edge.
    rounded_box(m, 'accent', (0, 0.0062, -depth / 2 - 0.0002), (width - 0.024, 0.0022, 0.0012), 0.0005, arc=1)
    top = case_h
    left = width / 2 - 0.009  # its left edge, +x
    far = depth / 2 - 0.009
    cap_h = 0.0105
    cap = U * 0.94

    def key(x0, row, w, marked=False):
        # x0: from its left edge, in keys.
        z = far - (row + (0.25 if row > 0 else 0) + 0.5) * U
        x = left - (x0 + w / 2) * U
        # Rows nearer it sit a little lower (a gentle slope).
        rise = 0.0012 * (5 - row) / 5
        rounded_box(m, 'keys_mark' if marked else 'keys', (x, top + rise + cap_h / 2, z),
                    (cap + (w - 1) * U, cap_h, cap), 0.0022, arc=1, taper=0.8, skip=('-y',))

    for r, row in enumerate(rows):
        x = 0.0
        for i, (w, gap) in enumerate(row):
            x += gap
            # W, A, S and D marked.
            marked = (r == 2 and i == 2) or (r == 3 and i in (1, 2, 3))
            key(x, r, w, marked)
            x += w
    for r, cols in enumerate(nav):
        if cols is None:
            continue
        for c, k in enumerate(cols):
            if k:
                key(15.5 + c, r, 1)
    return m


# ---- The mouse: its middle on the desk at 0, its front away -------------------------

def mouse():
    m = Mesh()
    a, b, H = 0.032, 0.062, 0.039  # half its width, half its length, its height

    def hump(z):
        # Highest a little behind its middle, lower at its front.
        t = z / b
        return 0.55 + 0.45 * math.exp(-((t + 0.25) / 0.6) ** 2)

    def width_at(z):
        t = z / b
        return 1.0 - 0.10 * math.exp(-((t - 0.1) / 0.35) ** 2)  # a little waist for the thumb

    nu, nv = 72, 24
    pts, nrm = [], []
    e = 2 / 2.8
    for i in range(nu + 1):
        u = 2 * math.pi * i / nu
        cu, su = math.cos(u), math.sin(u)
        sx = math.copysign(abs(cu) ** e, cu)
        sz = math.copysign(abs(su) ** e, su)
        for j in range(nv + 1):
            v = (math.pi / 2) * j / nv
            cv = math.cos(v) ** 0.55
            sv = math.sin(v) ** 0.9
            z = b * sz * cv
            x = a * sx * cv * width_at(z)
            y = 0.004 + (H - 0.004) * hump(z) * sv if j > 0 else 0.004
            pts.append((x, y, z))
    pts = np.array(pts)
    rows, cols = nu + 1, nv + 1
    P = pts.reshape(rows, cols, 3)
    # Normals from its surface's ways.
    du = np.roll(P, -1, axis=0) - np.roll(P, 1, axis=0)
    du[0] = P[1] - P[-2]
    du[-1] = du[0]
    dv = np.zeros_like(P)
    dv[:, 1:-1] = P[:, 2:] - P[:, :-2]
    dv[:, 0] = P[:, 1] - P[:, 0]
    dv[:, -1] = P[:, -1] - P[:, -2]
    N = unit(np.cross(dv, du))
    centre = P.reshape(-1, 3).mean(axis=0)
    out = (P - centre)
    flip = (N * out).sum(axis=-1) < 0
    N[flip] *= -1
    N[:, -1] = (0, 1, 0)
    pts = P.reshape(-1, 3)
    nrm = N.reshape(-1, 3)
    tris = face_normals_fix(pts, grid_tris(rows, cols), nrm)
    by = {}
    for tri in tris:
        p = pts[list(tri)].mean(axis=0)
        name = 'body'
        if p[2] > 0.004 and abs(p[0]) < 0.0012 and p[1] > 0.02:
            name = 'groove'  # between its buttons
        elif p[2] > 0.004 and p[1] > 0.026:
            name = 'buttons'
        elif p[1] < 0.0085:
            name = 'accent'  # its glowing foot
        by.setdefault(name, []).append(tri)
    for name, ts in by.items():
        m.add(name, pts, nrm, ts)
    # Its bottom.
    ring = [(pts[i * cols][0], pts[i * cols][2]) for i in range(nu)]
    bp = [(0, 0.004, 0)] + [(x, 0.004, z) for x, z in ring]
    bn = [(0, -1, 0)] * len(bp)
    bt = [(0, 1 + i, 1 + (i + 1) % nu) for i in range(nu)]
    m.add('grip', bp, bn, face_normals_fix(np.array(bp, np.float64), bt, np.array(bn, np.float64)))
    # Its wheel, between its buttons.
    zc = 0.026
    y_top = 0.004 + (H - 0.004) * hump(zc)
    prof = [(0.0, -0.004), (0.010, -0.004), (0.011, -0.003), (0.011, 0.003), (0.010, 0.004), (0.0, 0.004)]
    side = np.array([[0, 1.0, 0], [-1.0, 0, 0], [0, 0, 1.0]])  # its y along x
    lathe(m, 'wheel', prof, centre=(0, y_top - 0.007, zc), axis_turn=side, seg=28)
    return m


# ---- The mat: its middle at 0, its top just over the desk ---------------------------

def mat_prop():
    m = Mesh()
    w, d, t = 0.30, 0.24, 0.003
    outline = rounded_slab(m, 'mat_base', (0, -t / 2, 0), w, d, t, 0.014, top_material='mat_top')
    ring_strip(m, 'accent', outline, (0, 0, 0), 0.0002, 0.003, 0.0016)
    return m


# ---- The pad: its middle at 0 (x its left, y its face's up, z away) -----------------

def pad():
    m = Mesh()
    # Its body, and its two grips reaching back towards the hands, down.
    rounded_box(m, 'body', (0, 0.002, 0.004), (0.112, 0.030, 0.064), 0.013, arc=4)
    for s in (1, -1):
        direction = unit([0.32 * s, -0.30, -1.0])
        length_, r0, r1 = 0.060, 0.021, 0.017
        prof = []
        for k in range(9):
            a = -math.pi / 2 + (math.pi / 2) * k / 8
            prof.append((r0 * math.cos(a), r0 * math.sin(a)))
        for k in range(1, 10):
            prof.append((r0 + (r1 - r0) * k / 10, length_ * k / 10))
        for k in range(9):
            a = (math.pi / 2) * k / 8
            prof.append((r1 * math.cos(a), length_ + r1 * math.sin(a)))
        prof.append((0, length_ + r1))
        prof = [(max(0.0, r), y) for r, y in prof]
        lathe(m, 'grip', prof, centre=(0.040 * s, -0.002, -0.004), axis_turn=turn_to(direction), seg=32)
    top = 0.017
    # A raised ring round each stick (the sticks are their own file).
    for x, z in ((0.040, 0.012), (-0.025, -0.018)):
        lathe(m, 'buttons', [(0.0098, top - 0.004), (0.0098, top + 0.0005), (0.0118, top + 0.0012),
                             (0.0140, top + 0.0004), (0.0145, top - 0.002)], centre=(x, 0, z), seg=32)
    # Its d-pad (lower left) and face buttons (upper right).
    dx, dz = 0.025, -0.018
    rounded_box(m, 'buttons', (dx, top, dz), (0.021, 0.004, 0.0072), 0.0015, arc=2)
    rounded_box(m, 'buttons', (dx, top, dz), (0.0072, 0.004, 0.021), 0.0015, arc=2)
    fx, fz, gap = -0.040, 0.012, 0.0098
    for ox, oz in ((0, -gap), (gap, 0), (-gap, 0), (0, gap)):
        lathe(m, 'buttons', [(0, top - 0.002), (0.0046, top - 0.002), (0.0048, top + 0.0015), (0.0040, top + 0.0024),
                             (0, top + 0.0027)], centre=(fx + ox, 0, fz + oz), seg=24)
    # Its small middle buttons, and a glowing one at the top.
    for ox in (0.011, -0.011):
        rounded_box(m, 'buttons', (ox, top - 0.0005, 0.0), (0.0065, 0.003, 0.0042), 0.0015, arc=2)
    lathe(m, 'accent', [(0, top - 0.002), (0.0058, top - 0.002), (0.0060, top + 0.0008), (0.0050, top + 0.0016),
                        (0, top + 0.0018)], centre=(0, 0, 0.022), seg=28)
    # Its bumpers and triggers along its far edge.
    for s in (1, -1):
        rounded_box(m, 'buttons', (0.040 * s, 0.010, 0.0365), (0.036, 0.0075, 0.010), 0.0032, arc=2)
        rounded_box(m, 'body', (0.040 * s, -0.004, 0.034), (0.022, 0.018, 0.016), 0.006, arc=3, taper=1.0)
    return m


# ---- A stick: its root at 0 (where it turns), up its y ------------------------------

def stick():
    m = Mesh()
    prof = [(0.0, 0.0), (0.0045, 0.0), (0.0045, 0.010), (0.0060, 0.0115), (0.0098, 0.0125), (0.0102, 0.0140),
            (0.0099, 0.0158), (0.0090, 0.0166), (0.0060, 0.0158), (0.0, 0.0155)]
    lathe(m, 'buttons', prof, seg=32)
    # Its grippy rim.
    lathe(m, 'grip', [(0.0103, 0.0135), (0.0105, 0.0148), (0.0101, 0.0159)], seg=32)
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    made = {'keyboard': keyboard(), 'mouse': mouse(), 'mat': mat_prop(), 'pad': pad(), 'stick': stick()}
    for name, mesh in made.items():
        size = write_glb(mesh, os.path.join(OUT, name + '.glb'), name)
        print(f'{name}.glb: {size} bytes')


if __name__ == '__main__':
    main()
