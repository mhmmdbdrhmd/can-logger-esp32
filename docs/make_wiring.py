#!/usr/bin/env python3
"""Generate docs/wiring.svg.

Coordinates are computed, never hand-typed. Every pin row on a peer board is
placed at the *same* y as the ESP32 pin it connects to, so every wire is a
straight horizontal line: no crossings, no elbows, nothing to overlap.
"""
from pathlib import Path

W, H = 1420, 680

BG      = "#0D1117"
PANEL   = "#161B22"
EDGE    = "#30363D"
INK     = "#E6EDF3"
MUTED   = "#8B949E"
FAINT   = "#6E7681"

NET = {                       # net -> colour
    "3V3":  "#F85149",
    "GND":  "#6E7681",
    "CS":   "#F59E0B",
    "INT":  "#A78BFA",
    "SCK":  "#58A6FF",
    "MISO": "#3FB950",
    "MOSI": "#FF7B72",
    "CAN":  "#2DD4BF",
}

# (peer pin, esp32 pin, net)
CAN_LINKS = [("VCC", "3V3", "3V3"), ("GND", "GND", "GND"), ("CS", "D5", "CS"),
             ("INT", "D17", "INT"), ("SCK", "D18", "SCK"), ("MISO", "D19", "MISO"),
             ("MOSI", "D23", "MOSI")]
SD_LINKS  = [("VCC", "3V3", "3V3"), ("GND", "GND", "GND"), ("CS", "D4", "CS"),
             ("SCK", "D14", "SCK"), ("MISO", "D27", "MISO"), ("MOSI", "D13", "MOSI")]

PITCH   = 34
LEFT_Y0 = 196                              # first MCP2515 / ESP32-left row
RIGHT_Y0 = LEFT_Y0 + PITCH // 2 + 17       # SD / ESP32-right rows, offset half a pitch

BUS_X0 = 78                                # CAN stub start, well clear of the edge
# Heights are sized to the pin rows they contain (last row + 36 px padding),
# so no board carries dead space at the bottom.
MCP  = (250, 160, 250, 276)                # x, y, w, h
ESP  = (640, 150, 260, 286)
SD   = (1040, 178, 250, 241)

def y_left(i):  return LEFT_Y0 + i * PITCH
def y_right(i): return RIGHT_Y0 + i * PITCH

out = []
add = out.append

add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
    f'height="{H}" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">')
add('<defs>'
    '<style>'
    '.t{fill:#E6EDF3}.m{fill:#8B949E}.f{fill:#6E7681}'
    '.pin{font-size:13px}.lbl{font-size:12px}'
    '.board{fill:#161B22;stroke:#30363D;stroke-width:1.5;rx:10}'
    '.wire{stroke-width:2.4;fill:none;stroke-linecap:round}'
    '</style></defs>')
add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')

# ── title ────────────────────────────────────────────────────────────────────
add(f'<text x="46" y="56" class="t" font-size="21" font-weight="600">'
    f'ESP32 CAN logger — wiring</text>')
add(f'<text x="46" y="80" class="m" font-size="13">'
    f'MCP2515 on VSPI · SD card on HSPI · two independent buses, so SD writes '
    f'never stall CAN reception</text>')
add(f'<line x1="46" y1="98" x2="{W-46}" y2="98" stroke="{EDGE}" stroke-width="1"/>')

def board(x, y, w, h, title, sub):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" class="board"/>')
    add(f'<text x="{x + w/2}" y="{y + 26}" class="t" font-size="14.5" '
        f'font-weight="600" text-anchor="middle">{title}</text>')
    add(f'<text x="{x + w/2}" y="{y + 45}" class="f" font-size="11" '
        f'text-anchor="middle">{sub}</text>')

board(*MCP, "MCP2515 + TJA1050", "CAN controller")
board(*ESP, "ESP32 DevKit v1", "30-pin")
board(*SD,  "Micro-SD module", "SPI, 3V3 logic")

def pin(x, y, text, inward, colour):
    """Dot sits on the board edge; the label sits INSIDE the board.

    ``inward`` is +1 when the board body lies to the right of ``x``, -1 when it
    lies to the left. Keeping every label inside its own board leaves the gaps
    free for wires, so nothing can overlap text.
    """
    add(f'<circle cx="{x}" cy="{y}" r="4.2" fill="{colour}"/>')
    anchor = "start" if inward > 0 else "end"
    add(f'<text x="{x + inward * 13}" y="{y + 4.5}" class="pin t" '
        f'text-anchor="{anchor}">{text}</text>')

def wire(x0, y, x1, colour):
    add(f'<path d="M {x0} {y} H {x1}" class="wire" stroke="{colour}"/>')

# ── MCP2515 ↔ ESP32 (left bank) ──────────────────────────────────────────────
mx = MCP[0] + MCP[2]
ex = ESP[0]
for i, (p, e, net) in enumerate(CAN_LINKS):
    y = y_left(i)
    c = NET[net]
    wire(mx, y, ex, c)
    pin(mx, y, p, -1, c)          # label inside MCP2515
    pin(ex, y, e, +1, c)          # label inside ESP32

# ── ESP32 ↔ SD (right bank) ──────────────────────────────────────────────────
ex2 = ESP[0] + ESP[2]
sx = SD[0]
for i, (p, e, net) in enumerate(SD_LINKS):
    y = y_right(i)
    c = NET[net]
    wire(ex2, y, sx, c)
    pin(ex2, y, e, -1, c)         # label inside ESP32
    pin(sx, y, p, +1, c)          # label inside SD module

# ── CAN bus stub ─────────────────────────────────────────────────────────────
c = NET["CAN"]
yh, yl = y_left(2), y_left(3)
for y, lab in ((yh, "CAN_H"), (yl, "CAN_L")):
    wire(BUS_X0, y, MCP[0], c)
    pin(MCP[0], y, lab, +1, c)                     # label inside MCP2515
add(f'<path d="M {BUS_X0} {yh} V {yl}" class="wire" stroke="{c}" '
    f'stroke-dasharray="5 4" opacity="0.8"/>')
add(f'<text x="{BUS_X0 + 10}" y="{(yh + yl) / 2 + 4.5}" class="pin" fill="{c}">120 Ω</text>')
add(f'<text x="{BUS_X0 - 14}" y="{yh - 30}" class="lbl t" font-weight="600">to vehicle bus</text>')
add(f'<text x="{BUS_X0 - 14}" y="{yh - 12}" class="lbl f">CAN_H / CAN_L</text>')
add(f'<text x="{BUS_X0 - 14}" y="{yl + 36}" class="lbl f">terminate only if the logger</text>')
add(f'<text x="{BUS_X0 - 14}" y="{yl + 52}" class="lbl f">sits at the end of the bus</text>')

# ── bus annotations under each board ─────────────────────────────────────────
add(f'<text x="{MCP[0] + MCP[2]/2}" y="{MCP[1] + MCP[3] + 26}" class="lbl m" '
    f'text-anchor="middle">VSPI · GPIO 18/19/23</text>')
add(f'<text x="{SD[0] + SD[2]/2}" y="{SD[1] + SD[3] + 26}" class="lbl m" '
    f'text-anchor="middle">HSPI · GPIO 14/27/13</text>')

# ── legend ───────────────────────────────────────────────────────────────────
LY = 556
add(f'<line x1="46" y1="{LY - 30}" x2="{W-46}" y2="{LY - 30}" stroke="{EDGE}" stroke-width="1"/>')
order = ["3V3", "GND", "CS", "INT", "SCK", "MISO", "MOSI", "CAN"]
x = 46
for net in order:
    col = NET[net]
    add(f'<line x1="{x}" y1="{LY}" x2="{x + 26}" y2="{LY}" class="wire" stroke="{col}"/>')
    add(f'<text x="{x + 34}" y="{LY + 4.5}" class="lbl m">{net}</text>')
    x += 34 + 10 + len(net) * 7.6 + 26

# ── notes ────────────────────────────────────────────────────────────────────
notes = [
    ("Check the MCP2515 crystal.", "8 MHz or 16 MHz — set CAN_CRYSTAL_MHZ in src/config.h. "
     "The wrong value reports “NO CAN TRAFFIC” on a perfectly healthy bus."),
    ("Card format.", "Micro-SD must be FAT32. Cards over 32 GB usually ship as exFAT "
     "and have to be reformatted."),
]
ny = LY + 42
for head, body in notes:
    add(f'<text x="46" y="{ny}" class="lbl t" font-weight="600">{head}</text>')
    add(f'<text x="{46 + len(head) * 7.3 + 8}" y="{ny}" class="lbl m">{body}</text>')
    ny += 22

add('</svg>')

path = Path(__file__).with_name("wiring.svg")
path.write_text("\n".join(out))
print(f"wrote {path} ({path.stat().st_size} bytes)")
