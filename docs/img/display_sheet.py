#!/usr/bin/env python3
"""Display pages for the READMEs: the .pbm screens written by
wt32/test/disp_ui_test (run with a directory argument) -> docs/img/display.png.
Usage: display_sheet.py PBM_DIR   (needs Pillow)"""
import glob
import os
import sys
from PIL import Image, ImageDraw

PICK = ['client_1', 'client_2', 'client_3', 'client_4', 'router_1', 'router_2', 'ap_1', 'hold_6000']
SCALE, PAD, BEZEL = 3, 24, 10


def load(d, name):
    path = glob.glob(os.path.join(d, '*_%s.pbm' % name))[0]
    img = Image.open(path).convert('L')             # P4: 1 = black ink -> lit pixel on the OLED
    lit = img.point(lambda v: 255 if v == 0 else 0)
    oled = Image.new('RGB', img.size, (8, 10, 14))
    oled.paste((225, 240, 255), mask=lit)
    return oled.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)


def main(d):
    screens = [load(d, n) for n in PICK]
    w, h = screens[0].size
    cols = 4
    rows = (len(screens) + cols - 1) // cols
    cw, ch = w + 2 * BEZEL, h + 2 * BEZEL
    sheet = Image.new('RGB', (PAD + cols * (cw + PAD), PAD + rows * (ch + PAD)), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    for i, s in enumerate(screens):
        x = PAD + (i % cols) * (cw + PAD)
        y = PAD + (i // cols) * (ch + PAD)
        draw.rounded_rectangle((x, y, x + cw - 1, y + ch - 1), radius=8, fill=(30, 32, 38))
        sheet.paste(s, (x + BEZEL, y + BEZEL))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'display.png')
    sheet.save(out, optimize=True)
    print(out)


if __name__ == '__main__':
    main(sys.argv[1])
