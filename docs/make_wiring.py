#!/usr/bin/env python3
"""Generate docs/wiring.svg.

Coordinates are computed, never hand-typed. Every pin row on a peer board is
placed at the *same* y as the ESP32 pin it connects to, so the run between them
is a straight horizontal line.

With two MCP2515s that rule needs two additions.

Both controllers sit on the left and both talk to the same three ESP32 pins, so
a shared net would need two wires ending in one place. Shared nets are drawn as
one horizontal run to the ESP32 plus a vertical trunk with a junction dot,
which is what the breadboard actually looks like.

Those trunks stand in the way of other rows' horizontal runs, and a wire laid
across another with nothing to say so is the one ambiguity a wiring diagram
cannot afford - it reads as a join. So every crossing is drawn as a **hop**: a
little semicircle over the trunk. Junction dot = connected, hop = passes over.
That is the ordinary schematic convention and it is applied here by computing
the intersections, not by eye.

Nothing below is positioned by hand. Boards are sized from the pin rows they
contain, the footer is placed under whatever the drawing turned out to be, and
the canvas grows to fit. A pin that moves cannot push text off the edge.

The link tables mirror src/config.h and are the single source of truth for this
drawing. If a pin moves there, move it here.
"""
from pathlib import Path

W = 1520                                   # height is computed at the end

BG      = "#0D1117"
EDGE    = "#30363D"
MUTED   = "#8B949E"
FAINT   = "#6E7681"

NET = {                       # net -> colour
    "3V3":  "#F85149",
    "5V":   "#F0883E",
    "GND":  "#6E7681",
    "CS":   "#F59E0B",
    "INT":  "#A78BFA",
    "SCK":  "#58A6FF",
    "MISO": "#3FB950",
    "MOSI": "#FF7B72",
    "CAN1": "#2DD4BF",
    "CAN2": "#E879F9",
}

# (peer pin, esp32 pin, net). Order is the order the rows are drawn in.
CAN1_LINKS = [("VCC", "3V3", "3V3"), ("GND", "GND", "GND"), ("CS", "D22", "CS"),
              ("INT", "D21", "INT"), ("SCK", "D18", "SCK"), ("MISO", "D19", "MISO"),
              ("MOSI", "D23", "MOSI")]
CAN2_LINKS = [("VCC", "3V3", "3V3"), ("GND", "GND", "GND"), ("CS", "D5", "CS"),
              ("INT", "D17", "INT"), ("SCK", "D18", "SCK"), ("MISO", "D19", "MISO"),
              ("MOSI", "D23", "MOSI")]
SD_LINKS   = [("VCC", "VIN (5V)", "5V"), ("GND", "GND", "GND"), ("CS", "D4", "CS"),
              ("SCK", "D14", "SCK"), ("MISO", "D27", "MISO"), ("MOSI", "D13", "MOSI")]

# Nets both controllers share. Only CS and INT are unique per chip, which is the
# whole reason the second bus costs two pins rather than seven.
SHARED = ("3V3", "GND", "SCK", "MISO", "MOSI")

PITCH    = 34
TOP_PAD  = 62                              # board top edge -> first pin row
ESP_PAD  = 78                              # the ESP32 needs room for its notes
BOT_PAD  = 30
GAP      = 120                             # blank space between the two modules

LEFT_Y0  = 224                             # first CAN1 pin row
RIGHT_Y0 = 300                             # first SD row
BUS_X0   = 78                              # CAN stub start, well clear of the edge


def y1(i):      return LEFT_Y0 + i * PITCH
def y_right(i): return RIGHT_Y0 + i * PITCH


# -- board rectangles, sized to the rows they hold ---------------------------
def span(rows, top_pad=TOP_PAD):
    """(y, height) for a board whose pin rows are `rows`."""
    return rows[0] - top_pad, (rows[-1] - rows[0]) + top_pad + BOT_PAD


CAN1_Y, CAN1_H = span([y1(i) for i in range(len(CAN1_LINKS))])
CAN1 = (270, CAN1_Y, 250, CAN1_H)

CAN2_Y = CAN1_Y + CAN1_H + GAP


def y2(i):      return CAN2_Y + TOP_PAD + i * PITCH


CAN2 = (270, CAN2_Y, 250, (len(CAN2_LINKS) - 1) * PITCH + TOP_PAD + BOT_PAD)
SD_Y, SD_H = span([y_right(i) for i in range(len(SD_LINKS))])
SD = (1230, SD_Y, 250, SD_H)

out = []
add = out.append


def board(x, y, w, h, title, sub, pad=TOP_PAD):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" class="board"/>')
    add(f'<text x="{r(x + w/2)}" y="{y + 26}" class="t" font-size="14.5" '
        f'font-weight="600" text-anchor="middle">{title}</text>')
    add(f'<text x="{r(x + w/2)}" y="{y + 45}" class="f" font-size="11" '
        f'text-anchor="middle">{sub}</text>')


def r(v):
    """Round for output: SVG full of 243.79999999999998 is nobody's friend."""
    return int(v) if float(v).is_integer() else round(v, 1)


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace("—", "&#8212;").replace("’", "&#8217;")
             .replace("“", "&#8220;").replace("”", "&#8221;")
             .replace("·", "&#183;").replace("Ω", "&#937;"))


# -- geometry, all of it, before a single wire is drawn ----------------------
mx = CAN1[0] + CAN1[2]                 # right edge of both MCP2515 boards
ex = CAN1[0] + CAN1[2] + 240           # left edge of the ESP32
ESP_X = 760

# Each shared net gets its own vertical trunk, spaced so two trunks never sit on
# top of each other. Unique nets need none - they run straight across.
trunk_x = {}
tx = mx + 40
for net in SHARED:
    trunk_x[net] = tx
    tx += 28

# The ESP32 row each pin is drawn on. A shared net appears once, on the row CAN1
# established for it; CAN2 taps that row rather than claiming its own.
esp_y = {}
for i, (p, e, net) in enumerate(CAN1_LINKS):
    esp_y[e] = y1(i)
for i, (p, e, net) in enumerate(CAN2_LINKS):
    esp_y.setdefault(e, y2(i))

# The ESP32 box has to contain every row that lands on it - CAN1's seven, the
# two that are CAN2's alone, and the SD card's six. Computed, because the last
# time this was a constant the D5 and D17 pins ended up outside the board.
esp_rows = sorted(set(esp_y.values()) |
                  {y_right(i) for i in range(len(SD_LINKS))})
ESP_Y, ESP_H = span(esp_rows, ESP_PAD)
ESP = (ESP_X, ESP_Y, 280, ESP_H)

# Every vertical run, so the horizontal ones can hop over them.
VSEGS = []                             # (x, y_top, y_bottom)
for i, (p, e, net) in enumerate(CAN2_LINKS):
    if net in SHARED:
        VSEGS.append((trunk_x[net], esp_y[e], y2(i)))

# -- document ----------------------------------------------------------------
add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {{H}}" width="{W}" '
    f'height="{{H}}" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">')
add('<defs>'
    '<style>'
    '.t{fill:#E6EDF3}.m{fill:#8B949E}.f{fill:#6E7681}'
    '.pin{font-size:13px}.lbl{font-size:12px}'
    '.board{fill:#161B22;stroke:#30363D;stroke-width:1.5;rx:10}'
    '.wire{stroke-width:2.4;fill:none;stroke-linecap:round}'
    '</style></defs>')
add(f'<rect width="{W}" height="{{H}}" fill="{BG}"/>')

add('<text x="46" y="56" class="t" font-size="21" font-weight="600">'
    'ESP32 dual-CAN logger &#8212; wiring</text>')
add('<text x="46" y="80" class="m" font-size="13">'
    'Two MCP2515 controllers share VSPI (own CS + INT each) &#183; SD card alone '
    'on HSPI, so SD writes never stall CAN reception</text>')
add(f'<line x1="46" y1="98" x2="{W-46}" y2="98" stroke="{EDGE}" stroke-width="1"/>')

board(*CAN1, "MCP2515 #1 &#8212; CAN1", "CAN controller &#183; VSPI")
board(*CAN2, "MCP2515 #2 &#8212; CAN2", "CAN controller &#183; VSPI")
board(*ESP,  "ESP32 DevKit v1", "30-pin", ESP_PAD)
board(*SD,   "Micro-SD module", "SPI, 3V3 logic, 5V power")


def pin(x, y, text, inward, colour):
    """Dot sits on the board edge; the label sits INSIDE the board.

    ``inward`` is +1 when the board body lies to the right of ``x``, -1 when it
    lies to the left. Keeping every label inside its own board leaves the gaps
    free for wires, so nothing can overlap text.
    """
    add(f'<circle cx="{r(x)}" cy="{r(y)}" r="4.2" fill="{colour}"/>')
    anchor = "start" if inward > 0 else "end"
    add(f'<text x="{r(x + inward * 13)}" y="{r(y + 4.5)}" class="pin t" '
        f'text-anchor="{anchor}">{text}</text>')


HOP = 6.0


def wire(x0, y, x1, colour):
    """Horizontal run, hopping over every vertical trunk it merely crosses.

    A crossing that is a real connection is excluded by the strict comparison:
    a junction sits at a trunk's endpoint, never strictly inside it.
    """
    xs = sorted(x for (x, ya, yb) in VSEGS
                if min(ya, yb) < y < max(ya, yb) and min(x0, x1) < x < max(x0, x1))
    d = f"M {r(x0)} {r(y)}"
    for xc in xs:
        d += f" H {r(xc - HOP)} A {HOP} {HOP} 0 0 1 {r(xc + HOP)} {r(y)}"
    d += f" H {r(x1)}"
    add(f'<path d="{d}" class="wire" stroke="{colour}"/>')


def vwire(x, ya, yb, colour):
    add(f'<path d="M {r(x)} {r(ya)} V {r(yb)}" class="wire" stroke="{colour}"/>')


def junction(x, y, colour):
    add(f'<circle cx="{r(x)}" cy="{r(y)}" r="4.6" fill="{colour}"/>')


# -- CAN1 <-> ESP32 ----------------------------------------------------------
for i, (p, e, net) in enumerate(CAN1_LINKS):
    y = y1(i)
    c = NET[net]
    pin(mx, y, p, -1, c)
    wire(mx, y, ESP_X, c)
    pin(ESP_X, y, e, +1, c)

# -- CAN2 <-> ESP32 ----------------------------------------------------------
for i, (p, e, net) in enumerate(CAN2_LINKS):
    y = y2(i)
    c = NET[net]
    pin(mx, y, p, -1, c)
    if net in SHARED:
        # Out to this net's trunk, up to the row CAN1 already owns, and a
        # junction dot to say they are the same wire. That dot is the whole
        # point of the drawing: it is what "shared bus" means in copper.
        wire(mx, y, trunk_x[net], c)
        vwire(trunk_x[net], y, esp_y[e], c)
        junction(trunk_x[net], esp_y[e], c)
    else:
        wire(mx, y, ESP_X, c)
        pin(ESP_X, y, e, +1, c)

# Which controller each ESP32 pin serves, tagged ON THE ROW.
#
# This used to be two block labels, "CAN1 only" above the upper group and
# "CAN2 only" above the lower one. That was wrong and it was wrong in the
# direction that costs somebody an evening: five of the seven rows in the upper
# group are the SHARED nets, and labelling the block "CAN1 only" says the exact
# opposite of what the junction dots next to it say. Only D22 and D21 are
# CAN1's alone.
#
# Derived from the link tables, so a pin that changes bus cannot keep a stale
# tag.
users = {}
for links, name in ((CAN1_LINKS, "CAN1"), (CAN2_LINKS, "CAN2")):
    for p_, e_, net_ in links:
        users.setdefault(e_, []).append(name)

TAG_X = ESP_X + 72
for e_, who in users.items():
    tag = "CAN1 + CAN2" if len(who) > 1 else who[0] + " only"
    fill = MUTED if len(who) > 1 else FAINT
    add(f'<text x="{TAG_X}" y="{r(esp_y[e_] + 4)}" font-size="10.5" '
        f'fill="{fill}">{tag}</text>')

# -- ESP32 <-> SD (right bank) -----------------------------------------------
ex2 = ESP[0] + ESP[2]
sx = SD[0]
for i, (p, e, net) in enumerate(SD_LINKS):
    y = y_right(i)
    c = NET[net]
    wire(ex2, y, sx, c)
    pin(ex2, y, e, -1, c)         # label inside ESP32
    pin(sx, y, p, +1, c)          # label inside SD module

# -- the two CAN bus stubs ---------------------------------------------------
# Separate buses, separately terminated: the logger is often a stub on one
# already-terminated bus and the end node on the other.
for rect, ys, net, num in ((CAN1, y1, "CAN1", 1), (CAN2, y2, "CAN2", 2)):
    c = NET[net]
    yh, yl = ys(2), ys(3)
    for y, lab in ((yh, "CAN_H"), (yl, "CAN_L")):
        wire(BUS_X0, y, rect[0], c)
        pin(rect[0], y, lab, +1, c)
    add(f'<path d="M {BUS_X0} {yh} V {yl}" class="wire" stroke="{c}" '
        f'stroke-dasharray="5 4" opacity="0.8"/>')
    add(f'<text x="{BUS_X0 + 10}" y="{r((yh + yl) / 2 + 4.5)}" class="pin" '
        f'fill="{c}">120 &#937;</text>')
    add(f'<text x="{BUS_X0 - 14}" y="{yh - 30}" class="lbl t" '
        f'font-weight="600">to bus {num}</text>')
    add(f'<text x="{BUS_X0 - 14}" y="{yh - 12}" class="lbl f">CAN_H / CAN_L / GND</text>')
    if num == 1:
        tail = ("terminate only if the logger", "sits at the end of this bus")
    else:
        tail = ("separate bus, separate", "termination decision")
    add(f'<text x="{BUS_X0 - 14}" y="{yl + 36}" class="lbl f">{tail[0]}</text>')
    add(f'<text x="{BUS_X0 - 14}" y="{yl + 52}" class="lbl f">{tail[1]}</text>')

# -- captions under each board -----------------------------------------------
CAP1 = CAN2[1] + CAN2[3] + 26
add(f'<text x="{r(CAN2[0] + CAN2[2]/2)}" y="{CAP1}" class="lbl m" '
    f'text-anchor="middle">shared VSPI &#183; GPIO 18/19/23 &#8212; only CS and '
    f'INT are unique per controller</text>')
add(f'<text x="{r(SD[0] + SD[2]/2)}" y="{SD[1] + SD[3] + 26}" class="lbl m" '
    f'text-anchor="middle">HSPI &#183; GPIO 14/27/13</text>')

# -- legend, placed under whatever the drawing turned out to be --------------
# Measured, not guessed: the divider used to be a constant and ended up drawn
# straight through the second MCP2515.
content_bottom = max(CAN1[1] + CAN1[3], CAN2[1] + CAN2[3], ESP[1] + ESP[3],
                     SD[1] + SD[3], CAP1)
LY = content_bottom + 66
add(f'<line x1="46" y1="{r(LY - 30)}" x2="{W-46}" y2="{r(LY - 30)}" '
    f'stroke="{EDGE}" stroke-width="1"/>')
order = ["3V3", "5V", "GND", "CS", "INT", "SCK", "MISO", "MOSI", "CAN1", "CAN2"]
x = 46
for net in order:
    col = NET[net]
    add(f'<line x1="{r(x)}" y1="{r(LY)}" x2="{r(x + 26)}" y2="{r(LY)}" '
        f'class="wire" stroke="{col}"/>')
    add(f'<text x="{r(x + 34)}" y="{r(LY + 4.5)}" class="lbl m">{net}</text>')
    x += 34 + 10 + len(net) * 7.6 + 26

# -- notes -------------------------------------------------------------------
notes = [
    ("Two MCP2515s on one SPI bus.", "MISO tri-states while CS is high, so both "
     "controllers share D18/D19/D23. Each needs its own CS and its own INT "
     "— and keep the shared stubs short."),
    ("A dot is a join, a hop is not.", "Where a wire arcs over another they are "
     "separate nets that merely cross on the page. Only the filled dots on the "
     "vertical runs are places the two controllers share one wire."),
    ("Check each crystal separately.", "8 MHz or 16 MHz, per board — two "
     "modules from one order can differ. Set CAN1_CRYSTAL_MHZ and "
     "CAN2_CRYSTAL_MHZ in src/config.h. The wrong value reports "
     "“NO CAN TRAFFIC” on a perfectly healthy bus."),
    ("Power the SD module from VIN.", "Almost every breakout carries its own 3V3 "
     "regulator and wants 5 V. On 3V3 it browns out under write current and "
     "looks exactly like an empty slot. Card must be FAT32."),
    ("D22 and D21 are free here.", "Neither is a strapping pin, and unlike "
     "GPIO16/17 neither is taken by PSRAM on a WROVER module. They are the "
     "ESP32’s default I2C pins, unused in this project."),
    ("There is no pin marked D17.", "This board labels UART2 by function "
     "— TX2 is GPIO17, RX2 is GPIO16. CAN2’s INT goes to the pin "
     "silkscreened TX2. USB upload and the serial monitor run on UART0 and are "
     "unaffected."),
    ("One module is a valid build.", "Fit CAN1 only and the logger runs as a "
     "single-bus logger: the missing controller is reported once and skipped. "
     "Set CAN2_ENABLED 0 in src/config.h to stop it being looked for at all."),
]

CH = 7.2                                   # advance of one 12px monospace glyph


def wrap(text, width_px):
    """Break on spaces to fit `width_px`, measuring the text as it renders."""
    limit = max(8, int(width_px / CH))
    lines, cur = [], ""
    for word in text.split():
        cand = word if not cur else cur + " " + word
        if len(cand) <= limit:
            cur = cand
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


ny = LY + 42
for head, body in notes:
    bx = 46 + len(head) * 7.3 + 8
    add(f'<text x="46" y="{r(ny)}" class="lbl t" font-weight="600">{esc(head)}</text>')
    for k, line in enumerate(wrap(body, W - 46 - bx)):
        add(f'<text x="{r(bx)}" y="{r(ny + k * 18)}" class="lbl m">{esc(line)}</text>')
        last = k
    ny += 22 + last * 18

H = int(ny + 24)
add('</svg>')

svg = "\n".join(out).replace("{H}", str(H))
path = Path(__file__).with_name("wiring.svg")
path.write_text(svg)
print(f"wrote {path} ({path.stat().st_size} bytes, {W}x{H})")
