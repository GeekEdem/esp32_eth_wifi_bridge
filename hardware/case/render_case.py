"""Images of the case (called by build_case.py --render): 3D views through
OpenSCAD (needs openscad; headless: xvfb-run) and cross-sections through
matplotlib. The real WT32-ETH01 from the STEP is shown when available."""
import os
import shutil
import subprocess

import numpy as np
import trimesh


SCENE = '''
module m(n) import(str("%(c)s/", n, ".stl"));
%(case)s
'''
INSIDE = 'color("#d0d0d0") m("usb"); color("#202020") m("switch"); color("#3050c0") m("oled");'


def scad(path, board, case, out, camera, size=(1400, 1000)):
    with open(path, 'w') as f:
        f.write(SCENE % dict(c=os.path.dirname(path), case=case))
    cmd = ['openscad', '-q', '--imgsize=%d,%d' % size,
           '--camera=' + camera, '--colorscheme=Tomorrow', '-o', out, path]
    if not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a'] + cmd
    subprocess.run(cmd, check=True, timeout=600)


def sections(here, out, step):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import build_case as B
    c = os.path.join(here, '.cache')
    load = lambda n: trimesh.load(os.path.join(c, n + '.stl'))
    case = {'base': load('base_asm'), 'lid': load('lid_asm'), 'button': load('button_asm')}
    inner = {'WT32' + (' (STEP)' if step else ''): load('wt32_step' if step else 'wt32'),
             'OLED': load('oled'), 'USB-C': load('usb'), 'switch': load('switch')}
    colors = {'base': '#4a6fa5', 'lid': '#7fa3d0', 'button': '#e08a2e', 'OLED': '#3050c0',
              'USB-C': '#888888', 'switch': '#222222'}
    cuts = [('x', B.XC, 'lengthwise section through the middle (x = %.1f)' % B.XC),
            ('y', (B.RJ['y0'] + B.RJ['y1']) / 2, 'cross-section through the RJ45 and USB-C'),
            ('y', B.OLED_Y0 + B.OLED['h'] / 2, 'cross-section through the display')]
    fig, axes = plt.subplots(len(cuts), 1, figsize=(12, 13), gridspec_kw=dict(height_ratios=[1, 1, 1]))
    for ax, (axis, v, title) in zip(axes, cuts):
        normal = [1, 0, 0] if axis == 'x' else [0, 1, 0]
        origin = [v, 0, 0] if axis == 'x' else [0, v, 0]
        for name, mesh in list(case.items()) + list(inner.items()):
            sec = mesh.section(plane_origin=origin, plane_normal=normal)
            if sec is None:
                continue
            for poly in sec.discrete:
                pts = poly[:, [1, 2]] if axis == 'x' else poly[:, [0, 2]]
                col = colors.get(name, '#2e8b57')
                ax.fill(pts[:, 0], pts[:, 1], color=col, alpha=0.85 if name in case else 0.6, lw=0.3, ec='k')
        ax.set_title(title)
        ax.set_aspect('equal')
        ax.grid(alpha=0.3)
        ax.set_xlabel('y, mm' if axis == 'x' else 'x, mm')
        ax.set_ylabel('z, mm')
    handles = [plt.Rectangle((0, 0), 1, 1, color=colors.get(n, '#2e8b57')) for n in list(case) + list(inner)]
    fig.legend(handles, list(case) + list(inner), loc='upper right')
    fig.tight_layout()
    fig.savefig(out, dpi=90)


def render(here, step):
    c = os.path.join(here, '.cache')
    img = os.path.join(here, 'img')
    os.makedirs(img, exist_ok=True)
    if step:
        import build_case as B
        trimesh.util.concatenate(B.load_step()).export(os.path.join(c, 'wt32_step.stl'))
    board = 'color("#2e8b57") m("%s");' % ('wt32_step' if step else 'wt32')
    closed = board + INSIDE + 'color("#4a6fa5") m("base_asm"); color("#4a6fa5") m("lid_asm"); color("#e08a2e") m("button_asm");'
    ghost = board + INSIDE + 'color("#4a6fa5", 0.35) m("base_asm"); color("#e08a2e") m("button_asm"); color("#7fa3d0", 0.2) m("lid_asm");'
    # base without the lid: the board, USB-C and the switch in its post
    open_ = board + 'color("#d0d0d0") m("usb"); color("#202020") m("switch"); color("#4a6fa5") m("base_asm");'
    # the lid turned over: the OLED on its pegs and the plunger
    lid = 'rotate([180, 0, 0]) { color("#7fa3d0") m("lid_asm"); color("#3050c0") m("oled"); color("#e08a2e") m("button_asm"); }'
    views = [('outside', closed, '75,-80,70,0,-5,2'),
             ('rj45_end', closed, '-30,-110,25,0,-5,2'),
             ('inside', ghost, '80,-60,90,0,0,0'),
             ('open', open_, '-60,-40,100,0,0,-3'),
             ('lid_underside', lid, '70,-60,45,0,-5,-17')]
    for name, case, cam in views:
        scad(os.path.join(c, 'scene_%s.scad' % name), board, case, os.path.join(img, name + '.png'), cam)
    sections(here, os.path.join(img, 'sections.png'), step)
