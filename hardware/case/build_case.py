#!/usr/bin/env python3
"""WT32 case: base and lid, built with manifold3d (CSG), plus
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
# Only its pegs, shield tabs and pins go below the board, all behind y = RJ_LOW_Y0; the part in front
# of the board, through the end wall, ends at the board top.
RJ_FRONT_Z0, RJ_LOW_Y0 = 1.5, -26.9
MODULE = dict(x0=-8.0, x1=9.0, y0=-1.0, y1=24.0, z1=3.6)             # WT32-S1 with its shield; antenna at +y
PARTS_Z1 = 3.3                                                       # other top-side parts
HDR_X = (0.7 - 11.43, 0.7 + 11.43)                                   # 2 x 13 pins, 22.86 mm apart
HDR_Y = (-15.36 - 1.27, 15.12 + 1.27)
# Pin headers, if soldered: plastic 2.5 mm under the board, pins 6 mm beyond it,
# pin ends ~1.4 mm above the top. The board is also sold without them.
HDR_BELOW, HDR_ABOVE = 8.5, 1.4
RJ_PINS_Z0 = -3.5                                                   # RJ45 pins under the board

# ---------------------------------------------------------------- OLED 0.96"
# Typical 4-pin I2C module: board 27.3 x 27.8 (w along the text, h across it, the pin pads at its
# top edge), holes 2 mm from the edges, active area 21.74 x 10.86. MEASURE: board, holes, glass
# thickness, where the active area sits. It lies turned by 90°: the text runs along the case,
# towards the button (+y), the pin pads face -x.
OLED = dict(w=27.3, h=27.8, t=1.2, hole_d=2.0, hole_in=2.0,
            glass=1.5,            # glass + tape: from the glass face to the module board
            back=1.3,             # parts on the back of the module
            win_w=24.0, win_h=13.0,
            win_dy=2.8)           # window centre from the module centre, across the text (+: towards the pins); measured:
                                  # the visible area is ~4.2 mm from the pin edge, ~9.8 mm from the other one
OLED_PINS = (7.5, 20.0)           # along the text: the pin pads are here (no ledge under them, a recess in the lid over them)
OLED_HDR = dict(edge=1.5, pitch=2.54, sq=0.64)    # its 4 header pins, clipped as tall as the glass: centred along the
                                                  # text, this far from the pin edge (MEASURE)

# ---------------------------------------------------------------- button, USB-C
# 12x12 tactile switch, 8 mm to the top of its stem, with a round cap on it: 13 mm to the top of the
# cap, which comes out through the lid. MEASURE: body height, cap diameter, height with the cap.
SW = dict(body=12.0, body_h=3.8, stem=8.0, h=13.0, cap_d=11.4, legs_x=6.25, legs_y=2.5)
BTN_HOLE = 12.0                   # the cap's hole in the lid
CAP_PROUD = 1.2                   # the cap's top above the lid (sets the height of the switch post)
USB = dict(w=15.0, d=12.0, t=1.6, z=3.3,                             # MEASURE: breakout board, receptacle centre above the floor
           open_w=9.4, open_h=3.8, bore_w=12.8, bore_h=7.0, bore_left=0.8)

# ---------------------------------------------------------------- case
WALL, FLOOR, LID = 2.4, 2.0, 2.9      # lid: room for the countersunk screw heads (below), 1.04 mm of plate under them
WIN_LAND = 0.8               # straight part of the window edge under the outside chamfer
GLASS_IN = 0.6               # the glass face this far above the lid underside: sets the display's height (its ledges)
OLED_POCKET = 0.8            # pocket for the glass in the lid underside, 0.2 mm above it and around it: it cannot slide
LIP = dict(h=0.6, w=1.2, gap=0.2)    # lips under the lid along the long walls, 0.2 mm inside them, triangular in
                                     # section (h tall, w at the root): the lid cannot shift
LEDGE = dict(len=6.0, t=1.2, clear=0.25)    # ledges on the side walls carry the display's edges at both ends;
                                            # they stay clear of the WT32's outline, which goes in from the top
UNDER = 10.0                 # board bottom above the floor: room for the header pins and wires
RJ_RECESS = 1.25             # RJ45 front behind the outer face: it rests against 1.2 mm lips at the sides of the opening
RJ_LIP = 0.6                 # how much of the jack's face the lips cover at each side (its port and LEDs stay clear)
END_STOP = dict(gap=0.5, bump=0.7, hook=1.0, clear=0.1, t=1.6, w=3.2)
# end stops: plates (t thick, w wide) 0.5 mm past the board's far end, at its corners beside the WT32-S1 module. A bump
# on each, under the board's top, reaches 0.2 mm into it: the plates flex and press the jack against its lips. A hook
# above it reaches `hook` over the board's top, `clear` above it: the board snaps in under the hooks and cannot lift.
DISP_GAP = 0.25              # display edges to their stops: along the case (on the ledges) and across it (wall ribs)
TOP_GAP = 0.8                # RJ45 top to the lid
# Lid screws: ISO 10642 M3 x 8, hex socket countersunk (e.g. "K3x8 ISO 10642 A2"); the length
# includes the head. They cut their own thread in the plastic of the bosses.
SCREW = dict(d=3.0, head_d=6.72, head_k=1.86, l=8.0)
SCREW_PILOT, SCREW_CLEAR = 2.6, 3.4
CSK_D = SCREW['head_d'] + 0.4    # countersink at the lid surface: the head sits flush
CSK_EDGE = 1.0                   # countersink to the outer faces
BOSS_R = 2.5                     # 1.2 mm around the pilot
BOSS_IN = CSK_D / 2 + CSK_EDGE - WALL    # boss centre from the inner walls (keeps clear of the board's corners)
SEG = 48

# derived
XC = (PCB['x0'] + PCB['x1']) / 2
CZ0 = -UNDER
CZ1 = RJ['z1'] + TOP_GAP
OX0 = XC - OLED['h'] / 2                       # display module in the case (turned: h across the case)
OX1 = OX0 + OLED['h']
OY1 = PCB['y1'] + 0.5
OY0 = OY1 - OLED['w']
OZ1 = CZ1 + GLASS_IN - OLED['glass']           # module board top
OZ0 = OZ1 - OLED['t']                          # its bottom, on the ledges
CX0 = min(PCB['x0'], OX0) - 0.65
CX1 = max(PCB['x1'], OX1) + 0.65
CY0 = RJ['y0'] - RJ_RECESS + WALL
SW_Z0 = CZ1 + LID + CAP_PROUD - SW['h']        # switch bottom (top of its post), from where its cap must end
BTN_X = XC
BTN_Y = OY1 + 0.4 + 1.2 + 0.2 + SW['body'] / 2     # past the display: holding wall, 0.2 mm around the body
CY1 = BTN_Y + SW['body'] / 2 + 0.2 + 1.2
BOSSES = [(CX0 + BOSS_IN, CY0 + BOSS_IN), (CX1 - BOSS_IN, CY0 + BOSS_IN),
          (CX0 + BOSS_IN, CY1 - BOSS_IN), (CX1 - BOSS_IN, CY1 - BOSS_IN)]
POSTS = [(XC - 6, -12.0, 4), (XC + 6, -12.0, 4), (XC - 6, 20.0, 4), (XC + 6, 20.0, 4),
         (PCB['x0'] + 1.7, PCB['y0'] + 1.9, 3), (PCB['x1'] - 1.7, PCB['y0'] + 1.9, 3)]   # (x, y, d); the last two under the
                                                                                        # corners at the RJ45 end
END_X = [(MODULE['x0'] - 0.5 - END_STOP['w'], MODULE['x0'] - 0.5), (MODULE['x1'] + 0.5, MODULE['x1'] + 0.5 + END_STOP['w'])]
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


def section(pts):
    """A polygon as a CrossSection, turned counter-clockwise if needed (or the section is empty)."""
    area = sum(ax * by - bx * ay for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]))
    return CrossSection([pts[::-1] if area < 0 else pts])


def om(x0, x1, y0, y1):
    """A rectangle in the display module's own frame (x along the text, y across it, y = 0 at the
    edge away from the pins) -> (x0, x1, y0, y1) in the case, where the module lies turned by 90°."""
    return OX1 - y1, OX1 - y0, OY0 + x0, OY0 + x1


OLED_GLASS = (1.2, OLED['w'] - 1.3, 6.8, OLED['h'] - 4.0)    # glass in the module's frame (measured on a photo)


def ledge(x_wall, x_in, y0, y1):
    """A ledge from the wall to x_in, top at OZ0, with a 45° corbel under it (printable without
    supports); one profile, extruded along y."""
    zt, w = OZ0 - LEDGE['t'], abs(x_in - x_wall)
    pts = [(x_wall, zt - w), (x_in, zt), (x_in, OZ0), (x_wall, OZ0)]
    return Manifold.extrude(section(pts), y1 - y0).rotate([90, 0, 0]).translate([0, y1, 0])


def ledges():
    """Under the display's edges at both of its ends, on both side walls (not under its pin pads), with
    stops past the display's ends and ribs on the walls, DISP_GAP from its edges: it cannot slide."""
    lx0, lx1 = PCB['x0'] - LEDGE['clear'], PCB['x1'] + LEDGE['clear']       # outside the WT32's outline
    out = []
    for ya, yb, sa, sb in ((OY0 - DISP_GAP - 1.2, OY0 + LEDGE['len'], OY0 - DISP_GAP - 1.2, OY0 - DISP_GAP),
                           (OY1 - LEDGE['len'], OY1 + DISP_GAP + 1.2, OY1 + DISP_GAP, OY1 + DISP_GAP + 1.2)):
        for xw, xi, xr in ((CX0 - 0.3, lx0, OX0 - DISP_GAP), (CX1 + 0.3, lx1, OX1 + DISP_GAP)):
            out += [ledge(xw, xi, ya - 0.1, yb + 0.1),                                # overlaps, no shared faces
                    box(min(xw, xr), max(xw, xr), ya, yb, OZ0 - 0.3, OZ1),          # rib on the wall
                    box(min(xw, xi), max(xw, xi), sa, sb, OZ0 - 0.3, OZ1)]          # stop past the display's end
    return union(out)


def end_stops():
    """Two plates past the board's far end, from the floor, at its corners beside the WT32-S1 module (which reaches
    that end). On each, a bump with 45° faces under the board's top presses into its end, and a hook above it reaches
    over the board's top, with a 45° lead-in: the plates flex outwards as the board goes in, then hold it down."""
    s = END_STOP
    f = PCB['y1'] + s['gap']                           # plate face
    a = f - s['bump']                                  # bump apex: into the board, it pushes back
    zt, zb = PCB['t'] - 0.5, -0.5                      # the apex spans zb..zt
    zc = PCB['t'] + s['clear']                         # the hook's catch face, over the board's top
    tip, back = PCB['y1'] - s['hook'], f + s['t']
    top = zc + 1.2 + (back - 1.2 - tip)                # the lead-in ends 1.2 mm from the back: no knife edge
    prof = [(back, CZ0 - 0.3), (back, top), (back - 1.2, top), (tip, zc + 1.2), (tip, zc), (a + zc - zt, zc),
            (a, zt), (a, zb), (f, zb - s['bump']), (f, CZ0 - 0.3)]
    out = []
    for x0, x1 in END_X:
        m = Manifold.extrude(section(prof), x1 - x0).rotate([90, 0, 0]).rotate([0, 0, 90])   # (y, z, x)
        out.append(m.translate([x0, 0, 0]))
    return union(out)


def end_grip():
    """Where the end stops hold the board: the bumps reach into its end, the hooks lie 0.1 mm over its top
    (excluded from the clearance checks, checked on their own)."""
    return union([box(x0 - 0.1, x1 + 0.1, PCB['y1'] - END_STOP['hook'] - 0.2, PCB['y1'] + END_STOP['gap'], CZ0,
                      PCB['t'] + END_STOP['clear'] + 1.5) for x0, x1 in END_X])


def lips():
    """Under the lid, along both long walls, LIP['gap'] inside them: a triangle in section, its vertical face to
    the wall and a sloped one inside, so they do not snap off. They run between the corner bosses and end
    LIP['gap'] from them, so the lid cannot shift lengthwise either."""
    g, w, h = LIP['gap'], LIP['w'], LIP['h']
    y0, y1 = CY0 + BOSS_IN, CY1 - BOSS_IN                  # boss centre to boss centre, then cut around the bosses
    out = []
    for xo, xi in ((CX0 + g, CX0 + g + w), (CX1 - g, CX1 - g - w)):
        pts = [(xo, CZ1 - h), (xi, CZ1), (xi, CZ1 + 0.3), (xo, CZ1 + 0.3)]     # the top 0.3 mm goes into the plate
        out.append(Manifold.extrude(section(pts), y1 - y0).rotate([90, 0, 0]).translate([0, y1, 0]))
    return union(out) - union([cyl(x, y, CZ1 - h - 1, CZ1 + 1, 2 * (BOSS_R + g)) for x, y in BOSSES])


VENT_Z1 = OZ0 - LEDGE['t'] - (PCB['x0'] - LEDGE['clear'] - CX0 + 0.3) - 0.8      # vents stay under the corbels


# ---------------------------------------------------------------- parts
def base():
    ox0, ox1, oy0, oy1 = CX0 - WALL, CX1 + WALL, CY0 - WALL, CY1 + WALL
    shell = Manifold.extrude(rrect(ox1 - ox0, oy1 - oy0, OUTER_R), CZ1 - (CZ0 - FLOOR)) \
        .translate([(ox0 + ox1) / 2, (oy0 + oy1) / 2, CZ0 - FLOOR])
    solid = shell - box(CX0, CX1, CY0, CY1, CZ0, CZ1 + 1)
    add = [cyl(x, y, CZ0, CZ1, 2 * BOSS_R) for x, y in BOSSES]
    add += [cyl(x, y, CZ0, 0, d) for x, y, d in POSTS]                              # under the board
    for y0 in (-26.0, 18.0):                                                        # side guides, clear of the headers
        add += [box(CX0, PCB['x0'] - 0.3, y0, y0 + 4, -2, PCB['t'] + 1.2),
                box(PCB['x1'] + 0.3, CX1, y0, y0 + 4, -2, PCB['t'] + 1.2)]
    add += [end_stops()]
    # switch post: narrower than the switch across its legs (they hang down past the post, to solder wires
    # on), with two low walls on the other sides that hold the switch body
    px, hy = SW['legs_x'] - 0.65, SW['body'] / 2 + 0.2
    add += [box(BTN_X - px, BTN_X + px, BTN_Y - hy - 1.2, CY1 + 0.3, CZ0, SW_Z0)]            # into the end wall
    add += [box(BTN_X - px, BTN_X + px, BTN_Y + hy, CY1 + 0.3, SW_Z0, SW_Z0 + 2.0),
            box(BTN_X - px, BTN_X + px, BTN_Y - hy - 1.2, BTN_Y - hy, SW_Z0, SW_Z0 + 2.0)]
    add += [ledges()]
    uy1 = CY0 + USB['d']                                                            # USB-C breakout guides
    add += [box(XC - USB['w'] / 2 - 1.5, XC - USB['w'] / 2 - 0.3, CY0, uy1, CZ0, CZ0 + 2),
            box(XC + USB['w'] / 2 + 0.3, XC + USB['w'] / 2 + 1.5, CY0, uy1, CZ0, CZ0 + 2),
            box(XC - USB['w'] / 2, XC + USB['w'] / 2, uy1 + 0.3, uy1 + 1.5, CZ0, CZ0 + 2)]
    solid = solid + union(add)

    cut = [cyl(x, y, CZ1 + LID - SCREW['l'] - 1.3, CZ1 + 1, SCREW_PILOT) for x, y in BOSSES]    # 1.3 mm past the tip
    # RJ45: open to the top (the lid closes it), down to just under its front part; in front of the jack
    # the opening is narrower: the lips it rests against
    cut += [box(RJ['x0'] - 0.4, RJ['x1'] + 0.4, RJ['y0'] - 0.05, CY0 + 1, RJ_FRONT_Z0 - 0.4, CZ1 + 1),     # 0.05 mm to the lips
            box(RJ['x0'] + RJ_LIP, RJ['x1'] - RJ_LIP, CY0 - WALL - 1, RJ['y0'] + 0.01, RJ_FRONT_Z0 - 0.4, CZ1 + 1)]
    # USB-C: recess for the plug outside, the receptacle opening through the rest
    uz = CZ0 + USB['z']
    cut += [slot_y(XC, uz, USB['bore_w'], USB['bore_h'], 2.5, CY0 - WALL - 1, CY0 - USB['bore_left']),
            slot_y(XC, uz, USB['open_w'], USB['open_h'], 1.5, CY0 - WALL - 1, CY0 + 1)]
    # vents in both long walls, above the board
    for y in np.arange(-8, 21, 4.5):
        for x0 in (CX0 - WALL - 1, CX1 - 1):
            cut += [box(x0, x0 + WALL + 2, y, y + 1.6, PCB['t'] + 1.5, VENT_Z1)]
    return solid - union(cut)


def lid():
    ox0, ox1, oy0, oy1 = CX0 - WALL, CX1 + WALL, CY0 - WALL, CY1 + WALL
    plate = Manifold.extrude(rrect(ox1 - ox0, oy1 - oy0, OUTER_R), LID).translate([(ox0 + ox1) / 2, (oy0 + oy1) / 2, CZ1])
    add = [box(XC - 5, XC + 5, RJ['y0'] + 4, RJ['y0'] + 14, RJ['z1'] + 0.2, CZ1)]     # holds the RJ45 down
    solid = plate + union(add + [lips()])
    # window over the visible area, turned with the display; straight above the glass pocket, then a chamfer
    wx, wy, ww, wh = window_rect()
    z_ch = CZ1 + OLED_POCKET + WIN_LAND
    ch = CZ1 + LID - z_ch
    window = box(wx - ww / 2, wx + ww / 2, wy - wh / 2, wy + wh / 2, CZ1 - 1, CZ1 + LID + 1) + \
        Manifold.extrude(CrossSection.square([ww, wh], True), ch + 0.01,
                         scale_top=((ww + 2 * ch) / ww, (wh + 2 * ch) / wh)).translate([wx, wy, z_ch])
    gx0, gx1, gy0, gy1 = om(*OLED_GLASS)
    pocket = box(gx0 - 0.2, gx1 + 0.2, gy0 - 0.2, gy1 + 0.2, CZ1 - 1, CZ1 + OLED_POCKET)    # the glass cannot slide
    # the pocket goes on over the header pins, between the glass and the pin edge, out to the lip (which pauses there)
    px0, px1, py0, py1 = om(OLED_PINS[0], OLED_PINS[1], OLED_GLASS[3], OLED['h'])
    pins = box(CX0 + LIP['gap'] - 0.1, px1 + 0.01, py0, py1, CZ1 - 1, CZ1 + OLED_POCKET)     # past the lip's face
    cut = [window, pocket, pins, cyl(BTN_X, BTN_Y, CZ1 - 1, CZ1 + LID + 1, BTN_HOLE)]
    top, csk = CZ1 + LID, (CSK_D - SCREW_CLEAR) / 2                                  # 90° countersink
    cut += [cyl(x, y, CZ1 - 1, top + 1, SCREW_CLEAR) for x, y in BOSSES]
    cut += [cyl(x, y, top - csk, top + 0.5, SCREW_CLEAR, CSK_D + 1.0) for x, y in BOSSES]
    return solid - union(cut)


def window_rect():
    """Centre and size (along x, along y) of the window: the visible area, turned with the module."""
    return (OX1 - (OLED['h'] / 2 + OLED['win_dy']), OY0 + OLED['w'] / 2, OLED['win_h'], OLED['win_w'])


# ---------------------------------------------------------------- stand-ins of the parts inside
def wt32(headers=True):
    p = [box(PCB['x0'], PCB['x1'], PCB['y0'], PCB['y1'], 0, PCB['t']),
         box(RJ['x0'], RJ['x1'], RJ['y0'], RJ['y1'], RJ_FRONT_Z0, RJ['z1']),
         box(RJ['x0'], RJ['x1'], RJ_LOW_Y0, RJ['y1'], RJ['z0'], RJ_FRONT_Z0 + 0.01),
         box(RJ['x0'] + 1, RJ['x1'] - 1, PCB['y0'] + 0.5, RJ['y1'], RJ_PINS_Z0, 0.01),
         box(MODULE['x0'], MODULE['x1'], MODULE['y0'], MODULE['y1'], PCB['t'], MODULE['z1']),
         box(PCB['x0'] + 1, PCB['x1'] - 1, RJ['y1'], MODULE['y0'], PCB['t'], PARTS_Z1)]
    if headers:
        for x in HDR_X:
            p += [box(x - 1.27, x + 1.27, HDR_Y[0], HDR_Y[1], -2.5, 0.01),
                  box(x - 0.4, x + 0.4, HDR_Y[0] + 0.9, HDR_Y[1] - 0.9, -HDR_BELOW, PCB['t'] + HDR_ABOVE)]
    return union(p)


def oled_board():
    pcb = box(*om(0, OLED['w'], 0, OLED['h']), OZ0, OZ1)
    for hx in (OLED['hole_in'], OLED['w'] - OLED['hole_in']):
        for hy in (OLED['hole_in'], OLED['h'] - OLED['hole_in']):
            pcb -= cyl(OX1 - hy, OY0 + hx, OZ0 - 1, OZ1 + 1, OLED['hole_d'])
    return pcb


def oled_pins():
    """The 4 header pins on the module, clipped as tall as the glass."""
    y, s = OLED['h'] - OLED_HDR['edge'], OLED_HDR['sq'] / 2
    return union([box(*om(x - s, x + s, y - s, y + s), OZ1 - 0.01, OZ1 + OLED['glass'])
                  for x in (OLED['w'] / 2 + (i - 1.5) * OLED_HDR['pitch'] for i in range(4))])


def oled():
    """On the ledges, the glass up in the lid's pocket."""
    glass = box(*om(*OLED_GLASS), OZ1, OZ1 + OLED['glass'])
    back = box(*om(4, OLED['w'] - 4, 4, OLED['h'] - 5), OZ0 - OLED['back'], OZ0)
    return union([oled_board(), glass, back, oled_pins()])


def usb():
    y0 = CY0
    return union([box(XC - USB['w'] / 2, XC + USB['w'] / 2, y0, y0 + USB['d'], CZ0, CZ0 + USB['t']),
                  box(XC - 4.47, XC + 4.47, y0, y0 + 7.4, CZ0 + USB['z'] - 1.63, CZ0 + USB['z'] + 1.63)])


def switch_legs():
    legs = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = BTN_X + sx * SW['legs_x'], BTN_Y + sy * SW['legs_y']
            legs.append(box(x - 0.15, x + 0.15, y - 0.35, y + 0.35, SW_Z0 - 3.5, SW_Z0 + 1.0))
    return union(legs)


def switch_body():
    b = SW['body'] / 2
    return box(BTN_X - b, BTN_X + b, BTN_Y - b, BTN_Y + b, SW_Z0, SW_Z0 + SW['body_h'])


def switch_cap():
    """The round cap on the switch's stem (its skirt 2 mm down the stem), through its hole in the lid."""
    return cyl(BTN_X, BTN_Y, SW_Z0 + SW['stem'] - 2.0, SW_Z0 + SW['h'], SW['cap_d'])


def switch(cap=True):
    stem = cyl(BTN_X, BTN_Y, SW_Z0 + SW['body_h'] - 0.01, SW_Z0 + SW['stem'] - 2.0 + 0.01, 3.5)
    return union([switch_body(), stem, switch_legs()] + ([switch_cap()] if cap else []))


def screws():
    """ISO 10642 M3 x 8 in place, the head flush with the lid surface (0.05 mm under size)."""
    top, k = CZ1 + LID, SCREW['head_k']
    return union([cyl(x, y, top - k, top, SCREW['d'] - 0.1, SCREW['head_d'] - 0.1) for x, y in BOSSES] +
                 [cyl(x, y, top - SCREW['l'], top - k + 0.01, SCREW['d'] - 0.1) for x, y in BOSSES])


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

    parts = {'base': base(), 'lid': lid()}
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
            if pn == 'base' and sn == 'screws':
                continue            # screws cut their own thread
            if pn == 'base' and sn.startswith('wt32'):
                v = overlap(p - end_grip(), s)                 # the end-stop bumps press into it on purpose
                check(v < 0.01, '%s / %s apart from the end-stop bumps and hooks: overlap %.3f mm³' % (pn, sn, v))
                continue
            v = overlap(p, s)
            check(v < 0.01, '%s / %s: overlap %.3f mm³' % (pn, sn, v))
    for a in inside:
        for b in inside:
            if a < b:
                check(overlap(inside[a], inside[b]) < 0.01, 'inside: %s / %s do not overlap' % (a, b))

    # 3. clearances that matter
    w = wt32(True)
    rj = box(RJ['x0'], RJ['x1'], RJ['y0'], RJ['y1'], RJ_FRONT_Z0, RJ['z1']) + \
        box(RJ['x0'], RJ['x1'], RJ_LOW_Y0, RJ['y1'], RJ['z0'], RJ_FRONT_Z0 + 0.01)
    lip_zone = box(RJ['x0'] - 0.5, RJ['x1'] + 0.5, CY0 - WALL - 0.5, RJ['y0'], RJ['z0'] - 3, CZ1 + 1)
    no_lips = parts['base'] - lip_zone - end_grip()
    g = min_gap(no_lips, rj)
    check(g >= 0.3, 'RJ45 to the base apart from the lips it rests against: gap %.2f mm' % g)
    g = min_gap(parts['base'] ^ lip_zone, rj, 2)
    check(g <= 0.06, 'RJ45 front rests against the lips (%.2f mm), %.2f mm behind the outer face; they cover %.1f mm of its face '
          'at each side' % (g, RJ_RECESS, RJ_LIP))
    g = min_gap(parts['base'] ^ box(RJ['x0'] - 2, RJ['x1'] + 2, RJ['y0'] + 0.01, CY0 + 0.1, RJ['z0'] - 3, RJ_FRONT_Z0), rj, 5)
    check(g <= 0.45, 'RJ45 opening in the end wall ends under the front of the jack: %.2f mm below it' % g)
    boss_gap = PCB['y0'] - (CY0 + BOSS_IN + BOSS_R)
    press = overlap(parts['base'] ^ end_grip(), box(PCB['x0'], PCB['x1'], PCB['y0'], PCB['y1'], 0, PCB['t']))
    module = box(MODULE['x0'], MODULE['x1'], MODULE['y0'], MODULE['y1'], PCB['t'], MODULE['z1'])
    g = min_gap(parts['base'], module, 2)
    check(boss_gap >= 0.8 and press > 0.1 and g >= -0.001,
          'board lengthwise (%.2f mm with the RJ45): the end-stop bumps press %.1f mm into its far end (%.1f mm³), the stops '
          'clear the WT32-S1 module (%.2f mm), its edge %.2f mm from the bosses at the RJ45 end'
          % (PCB['y1'] - RJ['y0'], END_STOP['bump'] - END_STOP['gap'], press, g, boss_gap))
    pcb = box(PCB['x0'], PCB['x1'], PCB['y0'], PCB['y1'], 0, PCB['t'])
    hooks = parts['base'] ^ box(PCB['x0'], PCB['x1'], PCB['y1'] - 3, PCB['y1'] + 0.01, PCB['t'] + 0.01, PCB['t'] + 3)
    g = min_gap(hooks, pcb, 1)
    over = END_STOP['hook'] - (END_STOP['bump'] - END_STOP['gap'])
    under = all(PCB['x0'] <= x - d / 2 and x + d / 2 <= PCB['x1'] and PCB['y0'] <= y - d / 2 for x, y, d in POSTS)
    check(abs(g - END_STOP['clear']) <= 0.02 and over >= 0.6 and hooks.volume() > 1 and under,
          'board held down: the end-stop hooks reach %.1f mm over its top at the far corners, %.2f mm above it (the plates flex '
          '%.1f mm as it snaps in); %d posts under it, two at the RJ45 end' % (over, g, END_STOP['hook'], len(POSTS)))
    k, csk = SCREW['head_k'], (CSK_D - SCREW_CLEAR) / 2
    grip = CZ1 - (CZ1 + LID - SCREW['l'])
    edge = WALL + BOSS_IN - CSK_D / 2
    seat = LID - csk
    check(csk >= k and grip >= 4.0 and edge >= CSK_EDGE - 0.01 and seat >= 1.0,
          'lid screws ISO 10642 M3 x %g: countersink %.2f mm for a %.2f mm head (flush), %.2f mm of lid under it on the boss, '
          '%.1f mm of thread in the boss, countersink %.2f mm from the outer faces' % (SCREW['l'], csk, k, seat, grip, edge))
    g = min_gap(parts['lid'], rj)
    check(g >= 0.15, 'RJ45 to the lid: gap %.2f mm' % g)
    no_posts = no_lips - union([cyl(x, y, CZ0 - 1, 0.001, d + 0.2) for x, y, d in POSTS])
    g = min_gap(no_posts, w, 5)
    check(g >= 0.25, 'WT32 (with headers) to the base apart from its posts, the RJ45 lips and the end stops: gap %.2f mm' % g)
    g = min_gap(parts['lid'], w, 20)
    check(g >= 0.15, 'WT32 to the lid: gap %.2f mm' % g)
    g = min_gap(oled(), w, 20)
    check(g >= 2.0, 'OLED (back) above the WT32 parts: gap %.2f mm (room for its wires)' % g)
    g = min_gap(usb(), w, 10)
    check(g >= 0.8, 'USB-C breakout under the WT32 (RJ45 pins, headers): gap %.2f mm' % g)
    # display: on the ledges, glass in the lid's pocket; the WT32 still goes in past the ledges
    g = min_gap(oled_board(), parts['base'], 1)
    under = min(PCB['x0'] - LEDGE['clear'] - OX0, OX1 - PCB['x1'] - LEDGE['clear'])
    pins_free = LEDGE['len'] < OLED_PINS[0] and OLED['w'] - LEDGE['len'] > OLED_PINS[1]
    check(g <= 0.01 and under >= 0.8 and pins_free,
          'display board on the ledges (gap %.2f mm), %.2f mm of each ledge under it, its pin pads clear' % (g, under))
    g = min_gap(oled(), parts['lid'], 1)
    above = CZ1 + OLED_POCKET - (OZ1 + OLED['glass'])
    check(GLASS_IN > 0 and abs(above - 0.2) <= 0.01 and abs(g - 0.2) <= 0.02,
          'display glass %.1f mm into the lid pocket (%.1f mm deep): %.2f mm above it, %.2f mm nearest' % (GLASS_IN, OLED_POCKET, above, g))
    g = min_gap(oled_pins(), parts['lid'], 2)
    check(g >= 0.15, 'display header pins (as tall as the glass) in their recess of the lid, %.1f mm deep: %.2f mm above them'
          % (OLED_POCKET, g))
    g = min_gap(lips(), parts['base'], 1)
    gd = min_gap(lips(), oled(), 2)
    check(abs(g - LIP['gap']) <= 0.02 and gd >= 0.25,
          'lid lips (%.1f mm tall, triangular) %.2f mm inside the long walls and from the corner bosses, %.2f mm above the display'
          % (LIP['h'], g, gd))
    g = min_gap(oled_board(), parts['base'] ^ box(CX0 - 1, CX1 + 1, CY0 - 1, CY1 + 1, OZ0 + 0.01, OZ1 + 1), 2)
    check(abs(g - DISP_GAP) <= 0.02, 'display held on the ledges by end stops and wall ribs: %.2f mm around it' % g)
    column = box(PCB['x0'], PCB['x1'], PCB['y0'], PCB['y1'], CZ0, CZ1 + 1) + box(RJ['x0'], RJ['x1'], RJ['y0'], RJ['y1'], CZ0, CZ1 + 1)
    g = min_gap(no_lips ^ box(CX0 - 5, CX1 + 5, CY0 - 5, CY1 + 5, MODULE['z1'] + 1, CZ1 + 1), column, 3)
    check(g >= LEDGE['clear'] - 0.01, 'WT32 goes in from the top past the ledges and bosses: %.2f mm' % g)
    # button: the switch's own cap through the lid
    g = min_gap(switch_cap(), parts['lid'], 3)
    check(g >= 0.05, 'button cap in its %.1f mm lid hole: radial gap %.2f mm' % (BTN_HOLE, g))
    g = min_gap(switch_body(), parts['lid'], 3)
    proud = SW_Z0 + SW['h'] - (CZ1 + LID)
    check(g >= 0.15 and proud >= 0.5, 'switch body under the lid: %.2f mm; the cap stands %.1f mm above the lid' % (g, proud))
    walls = parts['base'] ^ box(BTN_X - 9, BTN_X + 9, BTN_Y - 9, BTN_Y + 9, SW_Z0 + 0.01, SW_Z0 + 3)
    g = min_gap(walls, switch(False), 2)
    check(0.05 <= g <= 0.4, 'switch body between the walls on its post: gap %.2f mm' % g)
    g = min_gap(parts['base'], switch_legs(), 2)
    check(g >= 0.3, 'switch legs past the sides of the post: gap %.2f mm' % g)

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
                    v = overlap(p - end_grip() if pn == 'base' else p, sm)
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
        g = min_gap(no_posts, allstep, 5)
        check(g >= 0.25, 'STEP board to the base apart from its posts and the RJ45 lips: gap %.2f mm' % g)
        g = min_gap(parts['lid'], allstep, 5)
        check(g >= 0.15, 'STEP board to the lid: gap %.2f mm' % g)

    # 5. printability in print orientation
    tb = to_trimesh(parts['base'])
    tl = to_trimesh(parts['lid'].rotate([180, 0, 0]))            # lid printed top face down: (x, y, z) -> (x, -y, -z)
    uz = CZ0 + USB['z']
    wall_scan(tb, 'base', [('wall in front of the USB-C receptacle',
                            (np.array([XC - 7, CY0 - WALL - 0.1, uz - 4.5]), np.array([XC + 7, CY0 + 0.1, uz + 4.5])))])
    # lid in print orientation (turned over): the thin land under the window chamfer
    lz0, lz1 = -(CZ1 + LID) - 0.1, -CZ1 + 0.1
    wx, wy, ww, wh = window_rect()
    wy = -wy
    lid_ok = [('window edge under the chamfer', (np.array([wx - ww / 2 - LID - 0.5, wy - wh / 2 - LID - 0.5, lz0]),
                                                  np.array([wx + ww / 2 + LID + 0.5, wy + wh / 2 + LID + 0.5, lz1])))]
    # the rim between a countersink and the outer faces, at the lid surface (CSK_EDGE)
    lid_ok += [('countersink rim at the surface', (np.array([x - CSK_D / 2 - CSK_EDGE - 0.1, -y - CSK_D / 2 - CSK_EDGE - 0.1, lz0]),
                                                    np.array([x + CSK_D / 2 + CSK_EDGE + 0.1, -y + CSK_D / 2 + CSK_EDGE + 0.1, lz0 + 0.5])))
               for x, y in BOSSES]
    # the plate under the screw heads, around the clearance hole (pressed onto the bosses)
    r = SCREW_CLEAR / 2 + 0.4
    lid_ok += [('plate under the screw heads (%.2f mm)' % seat, (np.array([x - r, -y - r, -CZ1 - 0.1]), np.array([x + r, -y + r, -CZ1 + 0.1])))
               for x, y in BOSSES]
    # the lips: triangular, thin towards their edge
    lw = LIP['gap'] + LIP['w'] + 0.1
    lid_ok += [('lips along the long walls', (np.array([x0, -CY1, -CZ1 - 0.1]), np.array([x0 + lw, -CY0, -CZ1 + LIP['h'] + 0.1])))
               for x0 in (CX0, CX1 - lw)]
    wall_scan(tl, 'lid', lid_ok)
    overhangs(tb, 'base')
    overhangs(tl, 'lid')

    # export in print orientation
    os.makedirs(os.path.join(HERE, 'stl'), exist_ok=True)
    for n, t in (('base', tb), ('lid', tl)):
        t = t.copy()
        t.apply_translation(-np.array([t.bounds[0][0], t.bounds[0][1], t.bounds[0][2]]))
        t.export(os.path.join(HERE, 'stl', 'wt32_case_%s.stl' % n))
    # the printed parts, put back into place, are the assembled ones (catches mirrored exports)
    for n, back in (('base', np.eye(4)), ('lid', trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))):
        t = trimesh.load(os.path.join(HERE, 'stl', 'wt32_case_%s.stl' % n))
        t.apply_transform(back)
        a = parts[n]
        t.apply_translation(np.array(a.bounding_box()[:3]) - t.bounds[0])
        common = (from_trimesh(t) ^ a).volume()
        check(abs(common - a.volume()) < 0.5, '%s: printed STL turned back into place matches the assembly (%.1f of %.1f mm³)'
              % (n, common, a.volume()))
    # stand-ins for previews
    os.makedirs(os.path.join(HERE, '.cache'), exist_ok=True)
    for n, m in (('wt32', wt32(True)), ('oled', oled()), ('usb', usb()), ('switch', switch(False)), ('cap', switch_cap()),
                 ('base_asm', parts['base']), ('lid_asm', parts['lid'])):
        to_trimesh(m).export(os.path.join(HERE, '.cache', n + '.stl'))

    size = np.array([CX1 - CX0 + 2 * WALL, CY1 - CY0 + 2 * WALL, CZ1 + LID - (CZ0 - FLOOR)])
    REPORT.insert(0, 'case outside: %.1f x %.1f x %.1f mm (with the lid)' % tuple(size))
    report = '\n'.join(REPORT)
    print(report)
    with open(os.path.join(HERE, 'check_report.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(report + '\n')
    if args.render:
        import render_case
        render_case.render(HERE, args.step)
    return 0 if all(not r.startswith('FAIL') for r in REPORT) else 1


if __name__ == '__main__':
    sys.exit(main())
