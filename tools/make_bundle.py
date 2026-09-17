#!/usr/bin/env python3
"""
Pack a whole logger setup into ONE file for the SD card.

    python3 tools/make_bundle.py --dbc can1.dbc --dbc2 can2.dbc \\
                                 --cfg dash.cfg --name-max 32 -o logger.bundle

A setup is three files that must agree: /frames.dbc, /frames2.dbc and
/dash.cfg. The layout names signals as "Message.Signal", so a map whose names
were shortened, beside a layout that was not rewritten to match, comes up as a
dashboard of "unknown" cells. One file cannot drift apart from itself.

The logger unpacks /logger.bundle at the next boot, writes the three files,
and sets the bundle aside as /logger.applied. Its name_max sizes the frame map
tables from then on (see src/bundle.h).

THE FORMAT - byte counts, not delimiters, because a .dbc may contain any line:

    #DCLB1 name_max=32
    #FILE frames.dbc 8106
    <8106 bytes>
    #FILE frames2.dbc 48456
    <48456 bytes>
    #FILE dash.cfg 3011
    <3011 bytes>
    #END

ON THE WAY IN, names too long for name_max are ABBREVIATED (tools/dbc_abbrev.py)
and the layout is rewritten to match, each bus against its own map. Nothing is
cut short, so no two signals can end up in one CSV column.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dbc_abbrev                                            # noqa: E402

MAGIC = "#DCLB1"
NAME_MAX_CHOICES = (16, 32, 64)
FILES = ("frames.dbc", "frames2.dbc", "dash.cfg")


# ---------------------------------------------------------------------------
#  the container
# ---------------------------------------------------------------------------
def pack(files, name_max):
    """{name: text} -> bundle bytes. Only frames.dbc, frames2.dbc and dash.cfg,
    in that order; an empty or missing one is left out.

    Exactly one newline follows every payload. It is a separator, not part of
    the count, and it is never conditional - an optional separator is how a
    reader loses track of where the next header starts."""
    out = [("%s name_max=%d\n" % (MAGIC, name_max)).encode()]
    for name in FILES:
        body = files.get(name)
        if not body:
            continue
        data = body.encode("utf-8") if isinstance(body, str) else body
        out.append(("#FILE %s %d\n" % (name, len(data))).encode())
        out.append(data)
        out.append(b"\n")
    out.append(b"#END\n")
    return b"".join(out)


def unpack(data):
    """bundle bytes -> (name_max, {name: text}). Raises ValueError if it is not
    one, the way src/bundle.cpp refuses it."""
    if not data.startswith(MAGIC.encode()):
        raise ValueError("not a setup bundle (no %s header)" % MAGIC)
    nl = data.index(b"\n")
    m = re.search(rb"name_max=(\d+)", data[:nl])
    name_max = int(m.group(1)) if m else 0
    pos, files = nl + 1, {}
    while pos < len(data):
        end = data.find(b"\n", pos)
        line = data[pos:end if end >= 0 else len(data)].rstrip(b"\r")
        pos = (end + 1) if end >= 0 else len(data)
        if line.startswith(b"#END"):
            return name_max, files
        if not line.startswith(b"#FILE "):
            continue
        parts = line.split()
        if len(parts) != 3:
            raise ValueError("bad #FILE line")
        name, size = parts[1].decode(), int(parts[2])
        if pos + size > len(data):
            raise ValueError("truncated in %s" % name)
        files[name] = data[pos:pos + size].decode("utf-8", "replace")
        pos += size
        if data[pos:pos + 1] in (b"\n", b"\r"):
            pos += 1
    raise ValueError("no #END - the bundle was cut short")


# ---------------------------------------------------------------------------
#  the layout, per bus
# ---------------------------------------------------------------------------
def line_bus(line):
    """1 or 2 - the bus a layout line is about. Absent means 1, as in dash.cpp."""
    m = re.search(r"\bbus=(\d+)", line)
    b = int(m.group(1)) if m else 1
    return b if b in (1, 2) else 1


def rewrite_cfg(cfg, maps):
    """The layout with its references pointed at the shortened names.

    maps: {bus: (msg_map, sig_map, node_map)}. Each line is rewritten against
    the map of the bus IT names, never a union - CAN2's map shortening a name
    must not rename a CAN1 cell that happens to use the same one."""
    out = []
    for line in cfg.splitlines(keepends=True):
        s = line.lstrip()
        mm, sm, nm = maps.get(line_bus(line), ({}, {}, {}))
        if s.startswith(("cell ", "send ")):
            line = re.sub(
                r"(\bsig=\"?)([A-Za-z_]\w*)\.([A-Za-z_]\w*)",
                lambda x: "%s%s.%s" % (x.group(1), mm.get(x.group(2), x.group(2)),
                                       sm.get(x.group(3), x.group(3))), line)
            line = re.sub(r"(\bmsel=)([A-Za-z_]\w*)",
                          lambda x: x.group(1) + sm.get(x.group(2), x.group(2)),
                          line)
        elif s.startswith(("role ", "node ")):
            line = re.sub(r'^(\s*\w+\s+)("?)([A-Za-z_]\w*)("?)',
                          lambda x: x.group(1) + x.group(2)
                          + nm.get(x.group(3), x.group(3)) + x.group(4), line)
        out.append(line)
    return "".join(out)


def cfg_messages(cfg, bus):
    """Messages the layout uses on one bus - the ones pruning must keep:
    every dashboard cell and every sendable value that names a signal. A
    message is kept whole, so a setpoint still has every signal it shares a
    frame with, and a multiplexed one keeps its selector."""
    keep = set()
    for line in cfg.splitlines():
        if line.lstrip().startswith(("cell ", "send ")) and line_bus(line) == bus:
            keep.update(re.findall(r'\bsig="?([A-Za-z_]\w*)\.', line))
    return keep


# ---------------------------------------------------------------------------
#  pruning a map
# ---------------------------------------------------------------------------
def messages(text):
    """-> [(name, signals, start, end)] in file order. A message's block runs
    from its BO_ line to the next line that is not one of its SG_ lines."""
    lines = text.splitlines(keepends=True)
    offs, pos = [], 0
    for ln in lines:
        offs.append(pos)
        pos += len(ln)
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"\s*BO_\s+\d+\s+([A-Za-z_]\w*)\s*:", lines[i])
        if not m:
            i += 1
            continue
        j = i + 1
        while j < len(lines) and re.match(r"\s*SG_\s", lines[j]):
            j += 1
        out.append((m.group(1), j - i - 1, offs[i], offs[j] if j < len(lines) else pos))
        i = j
    return out


REF_LINE = re.compile(r"^\s*(?:VAL_|SIG_VALTYPE_|SG_MUL_VAL_|BO_TX_BU_|"
                      r"CM_\s+(?:SG_|BO_)|BA_\s+\"[^\"]*\"\s+(?:SG_|BO_))"
                      r"\s+(\d+)\b")


def drop_messages(text, names):
    """The map without the messages in `names`: their BO_ blocks, and every
    VAL_, CM_, BA_ and SIG_VALTYPE_ line about them. One pass."""
    ids, out, skipping = set(), [], False
    lines = text.splitlines(keepends=True)
    for ln in lines:
        m = re.match(r"\s*BO_\s+(\d+)\s+([A-Za-z_]\w*)\s*:", ln)
        if m and m.group(2) in names:
            ids.add(m.group(1))
    for ln in lines:
        m = re.match(r"\s*BO_\s+(\d+)\s+([A-Za-z_]\w*)\s*:", ln)
        if m:
            skipping = m.group(2) in names
            if skipping:
                continue
        elif skipping and re.match(r"\s*SG_\s", ln):
            continue
        else:
            skipping = False
        r = REF_LINE.match(ln)
        if r and r.group(1) in ids:
            continue
        out.append(ln)
    return "".join(out)


def labels_per_message(text):
    """{message id text: value labels} - what dropping a message saves."""
    per = {}
    for ln in text.splitlines():
        m = re.match(r"\s*VAL_\s+(\d+)\s", ln)
        if m:
            per[m.group(1)] = per.get(m.group(1), 0) + len(re.findall(r'"[^"]*"', ln))
    return per


def prune_maps(texts, keeps, fits, counts, only=None):
    """Drop the largest messages the layout does not use, from any of the
    maps, until fits(counts) is true.

    texts, keeps, counts: one entry per bus - the map text, the messages the
    layout uses there (dashboard cells and sendable values), and its
    (messages, signals, value labels). `only`: the buses that may be trimmed;
    None means all of them.

    -> (texts, [(bus, message, signals)]). The largest go first, whichever map
    they are in, because each buys the most memory for one name lost. A message
    the layout uses is never a candidate. Nothing is lost from the RECORDING:
    frames of a message the map no longer holds are still written whole, as
    raw bytes, and decode offline against the full .dbc."""
    cands = []
    for b, text in enumerate(texts):
        if not text or (only is not None and b not in only):
            continue
        labels = labels_per_message(text)
        for name, sigs, start, _ in messages(text):
            if name in keeps[b]:
                continue
            mid = re.match(r"\s*BO_\s+(\d+)", text[start:start + 40]).group(1)
            cands.append((sigs, labels.get(mid, 0), b, name))
    cands.sort(key=lambda c: (-c[0], -c[1]))
    dropped = []
    c = [tuple(x) if x else None for x in counts]
    for sigs, vals, b, name in cands:
        if fits(c):
            break
        m, s, v = c[b]
        c[b] = (m - 1, s - sigs, v - vals)
        dropped.append((b, name, sigs))
    texts = list(texts)
    for b in range(len(texts)):
        names = {n for bb, n, _ in dropped if bb == b}
        if names:
            texts[b] = drop_messages(texts[b], names)
    return texts, dropped


def prune(text, keep, fits, counts):
    """prune_maps() for one map: fits takes that map's counts alone.
    -> (text, [(message, signals)])"""
    texts, dropped = prune_maps([text], [keep], lambda c: fits(c[0]), [counts])
    return texts[0], [(n, s) for _, n, s in dropped]


# ---------------------------------------------------------------------------
#  putting it together
# ---------------------------------------------------------------------------
def build(dbc1="", dbc2="", cfg="", name_max=64):
    """Texts in, (bundle bytes, report) out. Touches no files.

    report: name_max, and per bus the shortened names; texts and cfg as they
    went into the bundle, for a caller that wants to show the result."""
    if name_max not in NAME_MAX_CHOICES:
        raise ValueError("name_max is 16, 32 or 64")
    rep = {"name_max": name_max, "abbrev": {}, "texts": {}}
    maps = {}
    for bus, label, text in ((1, "frames.dbc", dbc1), (2, "frames2.dbc", dbc2)):
        if not text:
            continue
        short, mm, sm, nm, notes = dbc_abbrev.abbreviate(text, name_max)
        maps[bus] = (mm, sm, nm)
        rep["abbrev"][label] = notes
        rep["texts"][label] = short
    cfg_out = rewrite_cfg(cfg, maps) if cfg else ""
    rep["cfg"] = cfg_out
    files = dict(rep["texts"])
    files["dash.cfg"] = cfg_out
    return pack(files, name_max), rep


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--dbc", help="CAN1's frame map")
    ap.add_argument("--dbc2", help="CAN2's frame map")
    ap.add_argument("--cfg", help="the dashboard layout (dash.cfg)")
    ap.add_argument("--name-max", type=int, default=64, choices=NAME_MAX_CHOICES)
    ap.add_argument("-o", "--out", default="logger.bundle")
    a = ap.parse_args()

    def read(p):
        return Path(p).read_text(encoding="utf-8", errors="replace") if p else ""

    data, rep = build(read(a.dbc), read(a.dbc2), read(a.cfg), a.name_max)
    Path(a.out).write_bytes(data)
    print("wrote %s (%d bytes, name_max %d)" % (a.out, len(data), a.name_max))
    for label, notes in rep["abbrev"].items():
        print("  %-12s %d name(s) shortened" % (label, len(notes)))
    print("Copy it to the root of the SD card as /logger.bundle, or Import it "
          "from the dashboard; the logger unpacks it at the next start.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
