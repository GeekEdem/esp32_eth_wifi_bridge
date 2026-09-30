#!/usr/bin/env python3
"""WT32 case: base, lid and button plunger, built with manifold3d (CSG), plus
checks against stand-ins of the parts inside and against the real 3D model of
the WT32-ETH01.

    python3 build_case.py              # STLs into stl/, checks, report
    python3 build_case.py --step       # also check against the WT32-ETH01 STEP
    python3 build_case.py --render     # also images into img/ (openscad + matplotlib)

Needs: manifold3d, numpy, trimesh (+ cadquery-ocp for --step, matplotlib and
openscad for --render).

Coordinates are those of the WT32-ETH01 STEP model (egnor/wt32-eth01, see
STEP_URL): x across the board, y along it (RJ45 at -y, antenna at +y), z up,
z = 0 at the bottom of the board. The WT32 numbers below were measured on that
model; the OLED, button and USB-C numbers are typical values: measure yours.
"""
import argparse
import math
import os
import sys
import urllib.request

import numpy as np
import trimesh
from manifold3d import CrossSection, JoinType, Manifold, Mesh

HERE = os.path.dirname(os.path.abspath(__file__))
STEP_URL = ('https://raw.githubusercontent.com/egnor/wt32-eth01/'
            '4c2ebbc32b5d96d36a8ee9a332b3fd501148caa5/WT32-ETH01.step')

# ---------------------------------------------------------------- WT32-ETH01
# From the STEP model (and the KiCad footprint of the same repository).
PCB = dict(x0=-12.0, x1=13.4, y0=-29.26, y1=24.0, t=1.6)
RJ = dict(x0=-7.52, x1=8.52, y0=-36.3, y1=-15.0, z0=-1.5, z1=15.05)   # overhangs the board by 7 mm
MODULE = dict(x0=-8.0, x1=9.0, y0=-1.0, y1=24.0, z1=3.6)             # WT32-S1 with its shield; antenna at +y
PARTS_Z1 = 3.3                                                       # other top-side parts
HDR_X = (0.7 - 11.43, 0.7 + 11.43)                                   # 2 x 13 pins, 22.86 mm apart
HDR_Y = (-15.36 - 1.27, 15.12 + 1.27)
# Pin headers, if soldered: plastic 2.5 mm under the board, pins 6 mm beyond it,
# pin ends ~1.4 mm above the top. The board is also sold without them.
HDR_BELOW, HDR_ABOVE = 8.5, 1.4
RJ_PINS_Z0 = -3.5                                                   # RJ45 pins under the board

# ---------------------------------------------------------------- OLED 0.96"
# Typical 4-pin I2C module: board 27.3 x 27.8, holes 2 mm from the edges,
# active area 21.74 x 10.86. MEASURE: board, holes, glass thickness, where the
# active area sits.
OLED = dict(w=27.3, h=27.8, t=1.2, hole_d=2.0, hole_in=2.0,
            glass=1.5,            # glass + tape: lid underside to the module board
            back=1.3,             # parts on the back of the module
            win_w=24.0, win_h=13.0,
            win_dy=-1.0)          # window centre from the module centre, along y (-: away from the header)

# ---------------------------------------------------------------- button, USB-C
SW = dict(body=6.0, body_h=3.5, h=5.0, act_d=3.5)                    # 6x6 tactile, 5 mm tall
BTN = dict(d=5.6, hole=6.2, flange_d=9.0, flange_t=1.2, proud=1.5, gap=0.3)
USB = dict(w=15.0, d=12.0, t=1.6, z=3.3,                             # MEASURE: breakout board, receptacle centre above the floor
           open_w=9.4, open_h=3.8, bore_w=12.8, bore_h=7.0, bore_left=0.8)

# ---------------------------------------------------------------- case
WALL, FLOOR, LID = 2.4, 2.0, 2.0
WIN_LAND = 0.8               # straight part of the window edge under the outside chamfer
OLED_STAKE = 1.2             # pegs stick out of the OLED board this much: melt them over with a soldering iron
UNDER = 10.0                 # board bottom above the floor: room for the header pins and wires
RJ_RECESS = 0.3              # RJ45 front behind the outer face
TOP_GAP = 0.8                # RJ45 top to the lid
BTN_ZONE = 12.0              # room for the button at the +y end
SCREW_PILOT, SCREW_CLEAR, BOSS_R, BOSS_IN = 2.5, 3.4, 3.2, 1.0    # M3 self-tapping, pan head
SEG = 48

# derived
OLED_X0 = (PCB['x0'] + PCB['x1']) / 2 - OLED['w'] / 2
OLED_Y1 = PCB['y1'] + 0.5
OLED_Y0 = OLED_Y1 - OLED['h']
CX0 = min(PCB['x0'], OLED_X0) - 0.65
CX1 = max(PCB['x1'], OLED_X0 + OLED['w']) + 0.65
CY0 = RJ['y0'] - RJ_RECESS + WALL
CY1 = PCB['y1'] + 0.5 + BTN_ZONE
CZ0 = -UNDER
CZ1 = RJ['z1'] + TOP_GAP
XC = (PCB['x0'] + PCB['x1']) / 2
BTN_X, BTN_Y = XC, (OLED_Y1 + CY1) / 2 + 0.5
SW_TOP = CZ1 - BTN['flange_t'] - BTN['gap']
BOSSES = [(CX0 + BOSS_IN, CY0 + BOSS_IN), (CX1 - BOSS_IN, CY0 + BOSS_IN),
          (CX0 + BOSS_IN, CY1 - BOSS_IN), (CX1 - BOSS_IN, CY1 - BOSS_IN)]
POSTS = [(XC - 6, -12.0), (XC + 6, -12.0), (XC - 6, 20.0), (XC + 6, 20.0)]
OUTER_R = 2.0


# ---------------------------------------------------------------- helpers
def box(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl(x, y, z0, z1, d, d2=None, seg=SEG):
    return Manifold.cylinder(z1 - z0, d / 2, (d2 if d2 is not None else d) / 2, seg).translate([x, y, z0])


def rrect(w, h, r):
    return CrossSection.square([w - 2 * r, h - 2 * r], True).offset(r, JoinType.Round, circular_segments=SEG)


def union(parts):
    return Manifold.batch_boolean(list(parts), 0) if parts else Manifold()


def slot_y(xc, zc, w, h, r, y0, y1):
    """Rounded opening through a wall normal to y, centred at (xc, zc)."""
    return Manifold.extrude(rrect(w, h, r), y1 - y0).rotate([-90, 0, 0]).translate([xc, y0, zc])


# ---------------------------------------------------------------- parts
def base():
    ox0, ox1, oy0, oy1 = CX0 - WALL, CX1 + WALL, CY0 - WALL, CY1 + WALL
    shell = Manifold.extrude(rrect(ox1 - ox0, oy1 - oy0, OUTER_R), CZ1 - (CZ0 - FLOOR)) \
        .translate([(ox0 + ox1) / 2, (oy0 + oy1) / 2, CZ0 - FLOOR])
    solid = shell - box(CX0, CX1, CY0, CY1, CZ0, CZ1 + 1)
    add = [cyl(x, y, CZ0, CZ1, 2 * BOSS_R) for x, y in BOSSES]
    add += [cyl(x, y, CZ0, 0, 4) for x, y in POSTS]                               # under the board
    for y0 in (-26.0, 18.0):                                                        # side guides, clear of the headers
        add += [box(CX0, PCB['x0'] - 0.3, y0, y0 + 4, -2, PCB['t'] + 1.2),
                box(PCB['x1'] + 0.3, CX1, y0, y0 + 4, -2, PCB['t'] + 1.2)]
    add += [box(XC + dx - 1.5, XC + dx + 1.5, PCB['y1'] + 0.3, PCB['y1'] + 1.5, CZ0, PCB['t'] + 1.0) for dx in (-8, 8)]
    # switch post: narrower than the switch across its legs (they hang down past the post, to solder wires
    # on), with two low walls on the other sides that hold the switch body
    pz = SW_TOP - SW['h']
    add += [box(BTN_X - 2.7, BTN_X + 2.7, BTN_Y - 4.4, BTN_Y + 4.4, CZ0, pz)]
    for y0 in (BTN_Y + 3.2, BTN_Y - 4.4):
        add += [box(BTN_X - 2.7, BTN_X + 2.7, y0, y0 + 1.2, pz, pz + 2.0)]
    uy1 = CY0 + USB['d']                                                            # USB-C breakout guides
    add += [box(XC - USB['w'] / 2 - 1.5, XC - USB['w'] / 2 - 0.3, CY0, uy1, CZ0, CZ0 + 2),
            box(XC + USB['w'] / 2 + 0.3, XC + USB['w'] / 2 + 1.5, CY0, uy1, CZ0, CZ0 + 2),
            box(XC - USB['w'] / 2, XC + USB['w'] / 2, uy1 + 0.3, uy1 + 1.5, CZ0, CZ0 + 2)]
    solid = solid + union(add)

    cut = [cyl(x, y, CZ1 - 10, CZ1 + 1, SCREW_PILOT) for x, y in BOSSES]
    # RJ45: open to the top (the lid closes it)
    cut += [box(RJ['x0'] - 0.4, RJ['x1'] + 0.4, CY0 - WALL - 1, CY0 + 1, RJ['z0'] - 0.4, CZ1 + 1)]
    # USB-C: recess for the plug outside, the receptacle opening through the rest
    uz = CZ0 + USB['z']
    cut += [slot_y(XC, uz, USB['bore_w'], USB['bore_h'], 2.5, CY0 - WALL - 1, CY0 - USB['bore_left']),
            slot_y(XC, uz, USB['open_w'], USB['open_h'], 1.5, CY0 - WALL - 1, CY0 + 1)]
    # vents in both long walls, above the board
    for y in np.arange(-8, 21, 4.5):
        for x0 in (CX0 - WALL - 1, CX1 - 1):
            cut += [box(x0, x0 + WALL + 2, y, y + 1.6, PCB['t'] + 1.5, CZ1 - 2.5)]
    return solid - union(cut)


def lid():
    ox0, ox1, oy0, oy1 = CX0 - WALL, CX1 + WALL, CY0 - WALL, CY1 + WALL
    plate = Manifold.extrude(rrect(ox1 - ox0, oy1 - oy0, OUTER_R), LID).translate([(ox0 + ox1) / 2, (oy0 + oy1) / 2, CZ1])
    add = []
    for hx in (OLED['hole_in'], OLED['w'] - OLED['hole_in']):
        for hy in (OLED['hole_in'], OLED['h'] - OLED['hole_in']):
            x, y = OLED_X0 + hx, OLED_Y0 + hy
            add += [cyl(x, y, CZ1 - OLED['glass'], CZ1, 4.0),
                    cyl(x, y, CZ1 - OLED['glass'] - OLED['t'] - OLED_STAKE, CZ1, OLED['hole_d'] - 0.3)]
    add += [box(XC - 5, XC + 5, RJ['y0'] + 4, RJ['y0'] + 14, RJ['z1'] + 0.2, CZ1)]     # holds the RJ45 down
    solid = plate + union(add)
    wx, wy = XC, OLED_Y0 + OLED['h'] / 2 + OLED['win_dy']
    ww, wh, ch = OLED['win_w'], OLED['win_h'], LID - WIN_LAND
    window = box(wx - ww / 2, wx + ww / 2, wy - wh / 2, wy + wh / 2, CZ1 - 1, CZ1 + LID + 1) + \
        Manifold.extrude(CrossSection.square([ww, wh], True), ch + 0.01,
                         scale_top=((ww + 2 * ch) / ww, (wh + 2 * ch) / wh)).translate([wx, wy, CZ1 + WIN_LAND])
    cut = [window, cyl(BTN_X, BTN_Y, CZ1 - 1, CZ1 + LID + 1, BTN['hole'])]
    cut += [cyl(x, y, CZ1 - 1, CZ1 + LID + 1, SCREW_CLEAR) for x, y in BOSSES]
    return solid - union(cut)


def button():
    """Assembled position: the flange under the lid."""
    z = CZ1 - BTN['flange_t']
    return cyl(BTN_X, BTN_Y, z, z + BTN['flange_t'], BTN['flange_d']) + \
        cyl(BTN_X, BTN_Y, z + BTN['flange_t'] - 0.01, CZ1 + LID + BTN['proud'], BTN['d'])


# ---------------------------------------------------------------- stand-ins of the parts inside
def wt32(headers=True):
    p = [box(PCB['x0'], PCB['x1'], PCB['y0'], PCB['y1'], 0, PCB['t']),
         box(RJ['x0'], RJ['x1'], RJ['y0'], RJ['y1'], RJ['z0'], RJ['z1']),
         box(RJ['x0'] + 1, RJ['x1'] - 1, PCB['y0'] + 0.5, RJ['y1'], RJ_PINS_Z0, 0.01),
         box(MODULE['x0'], MODULE['x1'], MODULE['y0'], MODULE['y1'], PCB['t'], MODULE['z1']),
         box(PCB['x0'] + 1, PCB['x1'] - 1, RJ['y1'], MODULE['y0'], PCB['t'], PARTS_Z1)]
    if headers:
        for x in HDR_X:
            p += [box(x - 1.27, x + 1.27, HDR_Y[0], HDR_Y[1], -2.5, 0.01),
                  box(x - 0.4, x + 0.4, HDR_Y[0] + 0.9, HDR_Y[1] - 0.9, -HDR_BELOW, PCB['t'] + HDR_ABOVE)]
    return union(p)


def oled():
    z1 = CZ1 - OLED['glass']
    pcb = box(OLED_X0, OLED_X0 + OLED['w'], OLED_Y0, OLED_Y1, z1 - OLED['t'], z1)
    for hx in (OLED['hole_in'], OLED['w'] - OLED['hole_in']):
        for hy in (OLED['hole_in'], OLED['h'] - OLED['hole_in']):
            pcb -= cyl(OLED_X0 + hx, OLED_Y0 + hy, z1 - 3, z1 + 1, OLED['hole_d'])
    glass = box(OLED_X0 + 0.3, OLED_X0 + OLED['w'] - 0.3, OLED_Y0 + 4.0, OLED_Y0 + 4.0 + 19.3, z1, CZ1)
    back = box(OLED_X0 + 4, OLED_X0 + OLED['w'] - 4, OLED_Y0 + 4, OLED_Y1 - 5, z1 - OLED['t'] - OLED['back'], z1 - OLED['t'])
    return union([pcb, glass, back])


def usb():
    y0 = CY0
    return union([box(XC - USB['w'] / 2, XC + USB['w'] / 2, y0, y0 + USB['d'], CZ0, CZ0 + USB['t']),
                  box(XC - 4.47, XC + 4.47, y0, y0 + 7.4, CZ0 + USB['z'] - 1.63, CZ0 + USB['z'] + 1.63)])


def switch_legs():
    z0 = SW_TOP - SW['h']
    legs = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            x = BTN_X + sx * 3.25
            legs.append(box(x - 0.15, x + 0.15, BTN_Y + sy * 2.25 - 0.35, BTN_Y + sy * 2.25 + 0.35, z0 - 3.5, z0 + 1.0))
    return union(legs)


def switch():
    z0 = SW_TOP - SW['h']
    return union([box(BTN_X - 3, BTN_X + 3, BTN_Y - 3, BTN_Y + 3, z0, z0 + SW['body_h']),
                  cyl(BTN_X, BTN_Y, z0 + SW['body_h'] - 0.01, SW_TOP, SW['act_d']), switch_legs()])


def screws():
    return union([cyl(x, y, CZ1 - 8, CZ1 + LID, 2.9) for x, y in BOSSES])     # M3 x 10 shank (pan head on the lid)


# ---------------------------------------------------------------- export and checks
def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(np.asarray(mesh.vert_properties)[:, :3], np.asarray(mesh.tri_verts), process=True)


def from_trimesh(t):
    return Manifold(Mesh(vert_properties=np.asarray(t.vertices, np.float32), tri_verts=np.asarray(t.faces, np.uint32)))


REPORT = []


def check(ok, what):
    REPORT.append(('ok  ' if ok else 'FAIL') + '  ' + what)
    return ok


def overlap(a, b):
    return (a ^ b).volume()


def min_gap(a, b, search=3.0):
    return a.min_gap(b, search)


def load_step():
    """WT32-ETH01 STEP -> list of trimesh solids (cached in a scratch folder)."""
    cache = os.path.join(HERE, '.cache')
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, 'WT32-ETH01.step')
    if not os.path.exists(path):
        urllib.request.urlretrieve(STEP_URL, path)
    from OCP.BRep import BRep_Tool
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TopAbs import TopAbs_FACE, TopAbs_SOLID
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopLoc import TopLoc_Location
    from OCP.TopoDS import TopoDS
    r = STEPControl_Reader()
    assert r.ReadFile(path) == 1
    r.TransferRoots()
    shape = r.OneShape()
    BRepMesh_IncrementalMesh(shape, 0.05, False, 0.3, True)
    out = []
    ex = TopExp_Explorer(shape, TopAbs_SOLID)
    while ex.More():
        V, F = [], []
        fx = TopExp_Explorer(TopoDS.Solid(ex.Current()), TopAbs_FACE)
        while fx.More():
            face = TopoDS.Face(fx.Current())
            loc = TopLoc_Location()
            tri = BRep_Tool.Triangulation_s(face, loc)
            if tri is not None:
                tr, base_i = loc.Transformation(), len(V)
                for i in range(1, tri.NbNodes() + 1):
                    p = tri.Node(i).Transformed(tr)
                    V.append((p.X(), p.Y(), p.Z()))
                rev = face.Orientation() == 1
                for i in range(1, tri.NbTriangles() + 1):
                    a, b, c = tri.Triangle(i).Get()
                    F.append((base_i + a - 1, base_i + c - 1, base_i + b - 1) if rev else (base_i + a - 1, base_i + b - 1, base_i + c - 1))
            fx.Next()
        if F:
            t = trimesh.Trimesh(np.array(V), np.array(F), process=True)
            t.merge_vertices()
            out.append(t)
        ex.Next()
    return out


def wall_scan(t, name, allowed=(), n=8000, limit=1.15):
    """Thinnest material: rays from surface samples inwards to the other side.
    `allowed`: (label, (lo, hi)) boxes where thin material is intended (knife
    edges of chamfers, the wall in front of the USB-C receptacle); the
    thinnest spot of each is reported."""
    pts, idx = trimesh.sample.sample_surface_even(t, n, seed=1)
    normals = t.face_normals[idx]
    origins = pts - normals * 1e-3
    locs, ray_i, _ = t.ray.intersects_location(origins, -normals, multiple_hits=False)
    d = np.full(len(pts), np.inf)
    d[ray_i] = np.linalg.norm(locs - origins[ray_i], axis=1)
    free = np.ones(len(pts), bool)
    for label, (lo, hi) in allowed:
        inside = np.all((pts >= lo) & (pts <= hi), axis=1)
        if inside.any():
            REPORT.append('      %s: %s — thinnest %.2f mm (intended)' % (name, label, d[inside].min()))
        free &= ~inside
    thin = free & (d < limit)
    where = '' if not thin.any() else ' at %s' % pts[thin][np.argmin(d[thin])].round(1).tolist()
    check(not thin.any(), '%s: walls and ribs at least %.1f mm (3 perimeters of 0.4; thinnest %.2f mm%s)'
          % (name, limit + 0.05, d[free].min(), where))


def overhangs(t, name, max_deg=45, max_span=13.0):
    """Down-facing surfaces flatter than max_deg from horizontal-down, not on
    the bed (print orientation), grouped; each group must be a short bridge or
    a small ledge: its narrow side at most max_span."""
    n = t.face_normals
    ang = np.degrees(np.arccos(np.clip(-n[:, 2], -1, 1)))          # 0 = facing straight down
    bad = np.where((ang < 90 - max_deg) & (t.triangles_center[:, 2] > t.bounds[0][2] + 0.05))[0]
    groups = trimesh.graph.connected_components(t.face_adjacency, nodes=bad) if len(bad) else []
    worst = 0.0
    for g in groups:
        tri = t.triangles[g].reshape(-1, 3)
        ext = tri.max(0) - tri.min(0)
        span = min(ext[0], ext[1])
        worst = max(worst, span)
        REPORT.append('      %s: overhang %.1f x %.1f mm at z %.1f (%.1f mm²)' % (name, ext[0], ext[1], tri[:, 2].mean() - t.bounds[0][2], t.area_faces[g].sum()))
    check(worst <= max_span, '%s: overhangs are bridges/ledges of at most %.0f mm (widest %.1f mm)' % (name, max_span, worst))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', action='store_true', help='check against the WT32-ETH01 STEP (downloads it)')
    ap.add_argument('--render', action='store_true', help='images into img/')
    args = ap.parse_args()

    parts = {'base': base(), 'lid': lid(), 'button': button()}
    inside = {'wt32 (with headers)': wt32(True), 'oled': oled(), 'usb-c': usb(), 'switch': switch(), 'screws': screws()}

    # 1. closed, single solids
    for n, m in parts.items():
        t = to_trimesh(m)
        check(m.status().name == 'NoError' and t.is_watertight and len(m.decompose()) == 1,
              '%s: one closed solid (%d triangles, %.0f mm³)' % (n, m.num_tri(), m.volume()))

    # 2. no overlaps: case parts with each other and with everything inside
    names = list(parts)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            v = overlap(parts[names[i]], parts[names[j]])
            check(v < 0.01, '%s / %s: overlap %.3f mm³' % (names[i], names[j], v))
    for pn, p in parts.items():
        for sn, s in inside.items():
            v = overlap(p, s)
            allowed = (pn == 'base' and sn == 'screws') or (pn == 'lid' and sn == 'oled')
            if allowed:
                continue            # screws cut their own thread; the OLED pegs go through its holes (checked below)
            check(v < 0.01, '%s / %s: overlap %.3f mm³' % (pn, sn, v))
    v = overlap(parts['lid'], oled())
    check(v < 0.01, 'lid / oled (pegs in the holes): overlap %.3f mm³' % v)
    for a in inside:
        for b in inside:
            if a < b:
                check(overlap(inside[a], inside[b]) < 0.01, 'inside: %s / %s do not overlap' % (a, b))

    # 3. clearances that matter
    w = wt32(True)
    rj = box(RJ['x0'], RJ['x1'], RJ['y0'], RJ['y1'], RJ['z0'], RJ['z1'])
    g = min_gap(parts['base'], rj)
    check(g >= 0.3, 'RJ45 to the base: gap %.2f mm' % g)
    g = min_gap(parts['lid'], rj)
    check(g >= 0.15, 'RJ45 to the lid: gap %.2f mm' % g)
    no_posts = parts['base'] - union([cyl(x, y, CZ0 - 1, 0.001, 4.2) for x, y in POSTS])
    g = min_gap(no_posts, w, 5)
    check(g >= 0.25, 'WT32 (with headers) to the base apart from its posts: gap %.2f mm' % g)
    g = min_gap(parts['lid'], w, 20)
    check(g >= 0.15, 'WT32 to the lid: gap %.2f mm' % g)
    g = min_gap(oled(), w, 20)
    check(g >= 2.0, 'OLED (back) above the WT32 parts: gap %.2f mm (room for its wires)' % g)
    g = min_gap(usb(), w, 10)
    check(g >= 0.8, 'USB-C breakout under the WT32 (RJ45 pins, headers): gap %.2f mm' % g)
    shaft = cyl(BTN_X, BTN_Y, CZ1 + 0.05, CZ1 + LID + BTN['proud'], BTN['d'])
    g = min_gap(shaft, parts['lid'], 3)
    check(g >= 0.2, 'plunger in its lid hole: radial gap %.2f mm' % g)
    pz = SW_TOP - SW['h']
    walls = parts['base'] ^ box(BTN_X - 5, BTN_X + 5, BTN_Y - 5, BTN_Y + 5, pz + 0.01, pz + 3)
    g = min_gap(walls, switch(), 2)
    check(0.05 <= g <= 0.4, 'switch body between the walls on its post: gap %.2f mm' % g)
    g = min_gap(parts['base'], switch_legs(), 2)
    check(g >= 0.3, 'switch legs past the sides of the post: gap %.2f mm' % g)
    g = min_gap(parts['button'], switch(), 2)
    check(0.1 <= g <= 0.5, 'plunger above the switch actuator: %.2f mm' % g)

    # 4. real board (STEP)
    if args.step:
        solids = load_step()
        bad = 0
        for s in solids:
            try:
                sm = from_trimesh(s)
                if sm.is_empty() or sm.status().name != 'NoError':
                    raise ValueError
                for pn, p in parts.items():
                    v = overlap(p, sm)
                    if v > 0.01:
                        bad += 1
                        REPORT.append('      STEP solid %s overlaps %s: %.3f mm³' % (np.round(s.bounds, 2).tolist(), pn, v))
            except (ValueError, RuntimeError):
                for pn, p in parts.items():                   # not closed: sample its surface instead
                    pts = s.sample(2000)
                    inside_n = to_trimesh(p).contains(pts).sum()
                    if inside_n:
                        bad += 1
                        REPORT.append('      STEP solid %s: %d surface points inside %s' % (np.round(s.bounds, 2).tolist(), inside_n, pn))
        lo = np.min([s.bounds[0] for s in solids], 0)
        hi = np.max([s.bounds[1] for s in solids], 0)
        check(bad == 0, 'STEP WT32-ETH01 (%d solids, %s .. %s): no overlap with the case' % (len(solids), lo.round(2).tolist(), hi.round(2).tolist()))
        allstep = union([from_trimesh(s) for s in solids if s.is_watertight])
        g = min_gap(parts['base'] - union([cyl(x, y, CZ0 - 1, 0.001, 4.2) for x, y in POSTS]), allstep, 5)
        check(g >= 0.25, 'STEP board to the base apart from its posts: gap %.2f mm' % g)
        g = min_gap(parts['lid'], allstep, 5)
        check(g >= 0.15, 'STEP board to the lid: gap %.2f mm' % g)

    # 5. printability in print orientation
    tb = to_trimesh(parts['base'])
    tl = to_trimesh(parts['lid'].rotate([180, 0, 0]))            # lid printed top face down: (x, y, z) -> (x, -y, -z)
    tbu = to_trimesh(parts['button'])
    uz = CZ0 + USB['z']
    wall_scan(tb, 'base', [('wall in front of the USB-C receptacle',
                            (np.array([XC - 7, CY0 - WALL - 0.1, uz - 4.5]), np.array([XC + 7, CY0 + 0.1, uz + 4.5])))])
    # lid in print orientation (turned over): the thin land under the window chamfer
    lz0, lz1 = -(CZ1 + LID) - 0.1, -CZ1 + 0.1
    wx, wy = XC, -(OLED_Y0 + OLED['h'] / 2 + OLED['win_dy'])
    lid_ok = [('window edge under the chamfer', (np.array([wx - OLED['win_w'] / 2 - LID - 0.5, wy - OLED['win_h'] / 2 - LID - 0.5, lz0]),
                                                  np.array([wx + OLED['win_w'] / 2 + LID + 0.5, wy + OLED['win_h'] / 2 + LID + 0.5, lz1])))]
    wall_scan(tl, 'lid', lid_ok)
    wall_scan(tbu, 'button')
    overhangs(tb, 'base')
    overhangs(tl, 'lid')
    overhangs(tbu, 'button')

    # export in print orientation
    os.makedirs(os.path.join(HERE, 'stl'), exist_ok=True)
    for n, t in (('base', tb), ('lid', tl), ('button', tbu)):
        t = t.copy()
        t.apply_translation(-np.array([t.bounds[0][0], t.bounds[0][1], t.bounds[0][2]]))
        t.export(os.path.join(HERE, 'stl', 'wt32_case_%s.stl' % n))
    # the printed parts, put back into place, are the assembled ones (catches mirrored exports)
    for n, back in (('base', np.eye(4)), ('lid', trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0])),
                    ('button', np.eye(4))):
        t = trimesh.load(os.path.join(HERE, 'stl', 'wt32_case_%s.stl' % n))
        t.apply_transform(back)
        a = parts[n]
        t.apply_translation(np.array(a.bounding_box()[:3]) - t.bounds[0])
        common = (from_trimesh(t) ^ a).volume()
        check(abs(common - a.volume()) < 0.5, '%s: printed STL turned back into place matches the assembly (%.1f of %.1f mm³)'
              % (n, common, a.volume()))
    # stand-ins for previews
    os.makedirs(os.path.join(HERE, '.cache'), exist_ok=True)
    for n, m in (('wt32', wt32(True)), ('oled', oled()), ('usb', usb()), ('switch', switch()),
                 ('base_asm', parts['base']), ('lid_asm', parts['lid']), ('button_asm', parts['button'])):
        to_trimesh(m).export(os.path.join(HERE, '.cache', n + '.stl'))

    size = np.array([CX1 - CX0 + 2 * WALL, CY1 - CY0 + 2 * WALL, CZ1 + LID - (CZ0 - FLOOR)])
    REPORT.insert(0, 'case outside: %.1f x %.1f x %.1f mm (with the lid)' % tuple(size))
    report = '\n'.join(REPORT)
    print(report)
    with open(os.path.join(HERE, 'check_report.txt'), 'w') as f:
        f.write(report + '\n')
    if args.render:
        import render_case
        render_case.render(HERE, args.step)
    return 0 if all(not r.startswith('FAIL') for r in REPORT) else 1


if __name__ == '__main__':
    sys.exit(main())
