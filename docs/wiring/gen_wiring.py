#!/usr/bin/env python3
"""Wiring diagrams (SVG) for docs/WIRING.md:

    python3 gen_wiring.py            -> programmer.svg, standalone.svg

Boards are drawn from the component side. WT32-ETH01 pin order from its
datasheet (v1.4, silkscreen on the bottom side) and egnor/wt32-eth01; ESP32-C3
SuperMini from its silkscreen. Pin positions are schematic, not to scale.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# WT32-ETH01, component side up, antenna end at the top, RJ45 at the bottom.
WT32_LEFT = ['EN', 'GND', '3V3', 'EN', 'IO32 (CFG)', 'IO33 (485_EN)', 'IO5 (RXD)', 'IO17 (TXD)',
             'GND', '3V3', 'GND', '5V', 'LINK']
WT32_RIGHT = ['IO1 (TXD0)', 'IO3 (RXD0)', 'IO0', 'GND', 'IO39', 'IO36', 'IO15', 'IO14', 'IO12',
              'IO35', 'IO4', 'IO2', 'GND']
# ESP32-C3 SuperMini, component side up, USB-C at the top.
C3_LEFT = ['5V', 'GND', '3V3', 'GPIO4', 'GPIO3', 'GPIO2', 'GPIO1', 'GPIO0']
C3_RIGHT = ['GPIO5', 'GPIO6', 'GPIO7', 'GPIO8', 'GPIO9', 'GPIO10', 'GPIO20', 'GPIO21']

PITCH = 26
FONT = 'font-family="DejaVu Sans, Arial, sans-serif"'
COL = dict(red='#d62728', black='#222222', green='#2ca02c', orange='#ff7f0e', blue='#1f77b4',
           purple='#9467bd', brown='#8c564b', teal='#17becf', grey='#7f7f7f')


class Svg:
    def __init__(self, w, h, title):
        self.w, self.h, self.items = w, h, []
        self.items.append('<rect width="%d" height="%d" fill="#ffffff"/>' % (w, h))
        self.text(w / 2, 30, title, 18, anchor='middle', weight='bold')

    def text(self, x, y, s, size=12, anchor='start', weight='normal', color='#222222'):
        s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        self.items.append('<text x="%.1f" y="%.1f" %s font-size="%d" text-anchor="%s" font-weight="%s" fill="%s">%s</text>'
                          % (x, y, FONT, size, anchor, weight, color, s))

    def rect(self, x, y, w, h, fill, stroke='#333333', rx=6):
        self.items.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%d" fill="%s" stroke="%s" stroke-width="1.5"/>'
                          % (x, y, w, h, rx, fill, stroke))

    def pin(self, x, y, used):
        self.items.append('<circle cx="%.1f" cy="%.1f" r="5" fill="%s" stroke="#333" stroke-width="1"/>'
                          % (x, y, '#f5c518' if used else '#d9d9d9'))

    def wire(self, pts, color):
        d = ' '.join(('M' if i == 0 else 'L') + '%.1f %.1f' % p for i, p in enumerate(pts))
        self.items.append('<path d="%s" fill="none" stroke="%s" stroke-width="3.5" stroke-linejoin="round" stroke-linecap="round"/>'
                          % (d, color))

    def save(self, name):
        body = '\n'.join(self.items)
        with open(os.path.join(HERE, name), 'w') as f:
            f.write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n%s\n</svg>\n'
                    % (self.w, self.h, self.w, self.h, body))


def board(svg, x, y, w, left, right, name, sub, fill, used, top_label, bottom_label):
    """Two header columns; returns {label: (x, y, side)} of the pins."""
    rows = max(len(left), len(right))
    h = rows * PITCH + 70
    svg.rect(x, y, w, h, fill)
    svg.text(x + w / 2, y + 22, name, 15, 'middle', 'bold', '#ffffff')
    svg.text(x + w / 2, y + 40, sub, 11, 'middle', color='#e8e8e8')
    svg.text(x + w / 2, y - 8, top_label, 11, 'middle', color='#555555')
    svg.text(x + w / 2, y + h + 16, bottom_label, 11, 'middle', color='#555555')
    pins = {}
    for i, lab in enumerate(left):
        py = y + 60 + i * PITCH
        svg.pin(x + 14, py, lab in used)
        svg.text(x + 26, py + 4, lab, 11, color='#ffffff')
        pins.setdefault(lab, (x + 14, py, 'L'))
        pins['L%d' % i] = (x + 14, py, 'L')
    for i, lab in enumerate(right):
        py = y + 60 + i * PITCH
        svg.pin(x + w - 14, py, lab in used)
        svg.text(x + w - 26, py + 4, lab, 11, 'end', color='#ffffff')
        pins.setdefault(lab, (x + w - 14, py, 'R'))
        pins['R%d' % i] = (x + w - 14, py, 'R')
    return pins, h


def legend(svg, x, y, rows):
    for i, (color, text) in enumerate(rows):
        yy = y + i * 22
        svg.wire([(x, yy), (x + 30, yy)], COL[color])
        svg.text(x + 40, yy + 4, text, 12)


def lanes_route(svg, a, b, lane, color, src_box, dst_box, top):
    """Orthogonal route: out of the pin sideways to a private vertical, up to a
    private lane above the boards, across, down a private vertical beside the
    destination column, into the pin."""
    (ax, ay, aside), (bx, by, bside) = a, b
    sx = (src_box[0] - 16 - lane * 9) if aside == 'L' else (src_box[1] + 16 + lane * 9)
    dx = (dst_box[0] - 16 - lane * 9) if bside == 'L' else (dst_box[1] + 16 + lane * 9)
    ly = top - lane * 9
    svg.wire([(ax, ay), (sx, ay), (sx, ly), (dx, ly), (dx, by), (bx, by)], color)


def programmer():
    svg = Svg(1060, 800, 'ESP32-C3 SuperMini programmer → WT32-ETH01')
    used_c3 = {'5V', 'GND', 'GPIO4', 'GPIO5', 'GPIO6', 'GPIO7'}
    used_wt = {'5V', 'EN', 'IO1 (TXD0)', 'IO3 (RXD0)', 'IO0'}
    top = 150
    c3, _ = board(svg, 110, top + 20, 210, C3_LEFT, C3_RIGHT, 'ESP32-C3 SuperMini', 'component side up',
                  '#2f4f7f', used_c3, 'USB-C (to the PC or a USB charger)', '')
    wt, wh = board(svg, 560, top + 20, 250, WT32_LEFT, WT32_RIGHT, 'WT32-ETH01', 'component side up',
                   '#1f5f3f', used_wt, 'antenna end', 'RJ45 end')
    cb, wb = (110, 320), (560, 810)
    lane_top = top - 20
    lanes_route(svg, c3['GPIO4'], wt['IO3 (RXD0)'], 0, COL['green'], cb, wb, lane_top)
    lanes_route(svg, c3['GPIO5'], wt['IO1 (TXD0)'], 1, COL['orange'], cb, wb, lane_top)
    lanes_route(svg, c3['GPIO6'], wt['L0'], 2, COL['blue'], cb, wb, lane_top)
    lanes_route(svg, c3['GPIO7'], wt['IO0'], 3, COL['purple'], cb, wb, lane_top)
    lanes_route(svg, c3['5V'], wt['5V'], 4, COL['red'], cb, wb, lane_top)
    lanes_route(svg, c3['GND'], wt['L1'], 5, COL['black'], cb, wb, lane_top)
    ly = top + 20 + wh + 50
    legend(svg, 60, ly, [('green', 'C3 GPIO4 (UART1 TX)  →  WT32 IO3 / RXD0'),
                         ('orange', 'C3 GPIO5 (UART1 RX)  ←  WT32 IO1 / TXD0'),
                         ('blue', 'C3 GPIO6  →  WT32 EN      (open-drain: reset)'),
                         ('purple', 'C3 GPIO7  →  WT32 IO0     (open-drain: boot mode)'),
                         ('red', 'C3 5V  →  WT32 5V   (flashing and the log only, see below)'),
                         ('black', 'GND  —  GND   (required)')])
    notes = ['Either EN pin of the WT32 works; any GND works.',
             'Pin names are printed on the bottom side of both boards:',
             'if they differ from the drawing, go by the printed names.',
             'Never connect 3V3 to 3V3.']
    for i, n in enumerate(notes):
        svg.text(600, ly + 4 + i * 22, n, 12)
    svg.text(600, ly + 4 + 4 * 22, 'The C3\'s 5V cannot run Wi-Fi + Ethernet (brownout resets):', 12, weight='bold')
    svg.text(600, ly + 4 + 5 * 22, 'give the WT32 its own 5 V supply and remove the red wire.', 12, weight='bold')
    svg.save('programmer.svg')


def standalone():
    svg = Svg(1060, 670, 'WT32-ETH01 in the case: power, display, button')
    used_wt = {'5V', 'GND', 'IO32 (CFG)', 'IO33 (485_EN)', 'IO4', '3V3'}
    wt, wh = board(svg, 400, 90, 250, WT32_LEFT, WT32_RIGHT, 'WT32-ETH01', 'component side up',
                   '#1f5f3f', used_wt, 'antenna end', 'RJ45 end (Ethernet cable to the device)')
    # USB-C breakout
    svg.rect(60, 340, 170, 90, '#555555')
    svg.text(145, 362, 'USB-C breakout', 13, 'middle', 'bold', '#ffffff')
    svg.text(145, 378, '5 V power only', 11, 'middle', color='#e8e8e8')
    svg.pin(215, 395, True); svg.text(203, 399, 'GND', 11, 'end', color='#ffffff')
    svg.pin(215, 418, True); svg.text(203, 422, 'VBUS / 5V', 11, 'end', color='#ffffff')
    svg.wire([(215, 418), (345, 418), (345, wt['5V'][1]), wt['5V'][:2]], COL['red'])
    svg.wire([(215, 395), (330, 395), (330, wt['L10'][1]), wt['L10'][:2]], COL['black'])
    # OLED
    svg.rect(60, 110, 200, 150, '#2b3a8f')
    svg.text(160, 132, 'OLED 0.96" SSD1306', 13, 'middle', 'bold', '#ffffff')
    svg.text(160, 148, 'I2C, address 0x3C / 0x3D', 11, 'middle', color='#e8e8e8')
    oled = {}
    for i, lab in enumerate(['GND', 'VCC', 'SCL', 'SDA']):
        py = 175 + i * 22
        svg.pin(245, py, True)
        svg.text(233, py + 4, lab, 11, 'end', color='#ffffff')
        oled[lab] = (245, py)
    svg.wire([oled['VCC'], (300, oled['VCC'][1]), (300, wt['L2'][1]), wt['L2'][:2]], COL['orange'])
    svg.wire([oled['GND'], (285, oled['GND'][1]), (285, wt['L1'][1] - 8), (380, wt['L1'][1] - 8), (380, wt['L1'][1]), wt['L1'][:2]], COL['black'])
    svg.wire([oled['SDA'], (360, oled['SDA'][1]), (360, wt['IO32 (CFG)'][1]), wt['IO32 (CFG)'][:2]], COL['teal'])
    svg.wire([oled['SCL'], (315, oled['SCL'][1]), (315, wt['IO33 (485_EN)'][1]), wt['IO33 (485_EN)'][:2]], COL['brown'])
    # button
    bx, by = 800, 400
    svg.rect(bx, by, 120, 80, '#444444')
    svg.text(bx + 60, by + 24, 'Button', 13, 'middle', 'bold', '#ffffff')
    svg.text(bx + 60, by + 40, '6x6 tactile', 11, 'middle', color='#e8e8e8')
    svg.pin(bx + 15, by + 60, True); svg.pin(bx + 105, by + 60, True)
    io4, gnd = wt['IO4'], wt['R12']
    svg.wire([io4[:2], (io4[0] + 60, io4[1]), (io4[0] + 60, by + 60), (bx + 15, by + 60)], COL['purple'])
    svg.wire([gnd[:2], (gnd[0] + 40, gnd[1]), (gnd[0] + 40, by + 100), (bx + 105, by + 100), (bx + 105, by + 60)], COL['black'])
    legend(svg, 60, 500, [('red', 'USB-C 5V  →  WT32 5V'),
                          ('black', 'GND  —  GND (any GND pin)'),
                          ('orange', 'OLED VCC  →  WT32 3V3   (not 5V)'),
                          ('teal', 'OLED SDA  →  WT32 IO32 (CFG)'),
                          ('brown', 'OLED SCL  →  WT32 IO33 (485_EN)'),
                          ('purple', 'Button  →  WT32 IO4 (other leg to GND)')])
    svg.text(700, 550, 'OLED pin order differs between modules', 12, weight='bold')
    svg.text(700, 572, '(GND-VCC-SCL-SDA or VCC-GND-SCL-SDA):', 12)
    svg.text(700, 594, 'wire by the names printed on yours.', 12)
    svg.text(700, 616, 'Display and button are optional;', 12)
    svg.text(700, 638, 'the firmware runs without them.', 12)
    svg.save('standalone.svg')


if __name__ == '__main__':
    programmer()
    standalone()
