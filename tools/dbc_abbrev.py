#!/usr/bin/env python3
"""
Shorten the names in a frame map to fit a name length, and keep them distinct.

    python3 tools/dbc_abbrev.py frames.dbc --name-max 16 -o frames_16.dbc

The logger stores every message, signal and node name in a slot of name_max
bytes, nul included, so the setup bundle's name_max decides what each one
costs. A shorter length leaves more memory for the web server - but the
firmware, given a name that does not fit, can only cut it off, and cutting is
the worst way to shorten a name. Names differ at the end more often than at the
start:

    Accelerometer_X   ->  Accelerometer_  (cut to 15)
    Accelerometer_Y   ->  Accelerometer_  the same name. One CSV column.

So this shortens a name by giving up what is least likely to be missed, and
stops the moment it fits:

  1. nothing, if it already fits
  2. the usual abbreviation of a word, longest word first -
     EngineCoolantTemperature -> EngineCoolTemp
  3. the word cut after its first syllable, longest first -
     GuidanceCurvatureCommand -> GuidanceCurvCmd
  4. the vowels after a word's first letter - Pump -> Pmp
  5. letters off the end of the longest word, never below two
  6. letters off the end of the name, as the firmware would

Words are split at underscores and at camelCase, and joined back the way they
came, so Engine_Speed stays underscored and EngineSpeed stays camel.

NO TWO NAMES MAY BECOME ONE. A shortened name that collides with one already
given out gets a number in place of its last characters. The same original
name always gets the same short one, so a signal name that a J1939 map repeats
across a hundred messages is still one name.

A map shortened on its own is half a job: the layout refers to signals as
"Message.Signal" and to this logger's node by name. mapping() returns what
became what, and tools/make_bundle.py rewrites the layout with it.
"""

import argparse
import re
import sys

VOWELS = set("aeiouAEIOU")

# The abbreviations an engineer would write anyway. Lower-case keys; the case
# of the first letter is kept from the original word.
ABBREV = {
    "acceleration": "accel", "accelerometer": "accel", "actual": "act",
    "address": "addr", "ambient": "amb", "amount": "amt", "angle": "ang",
    "angular": "ang", "available": "avail", "average": "avg",
    "battery": "batt", "calibration": "cal", "command": "cmd",
    "communication": "comm", "configuration": "cfg", "control": "ctrl",
    "controller": "ctrl", "coolant": "cool", "counter": "cnt",
    "current": "curr", "demand": "dmd", "desired": "des",
    "diagnostic": "diag", "diagnostics": "diag", "difference": "diff",
    "direction": "dir", "distance": "dist", "electrical": "elec",
    "engine": "eng", "error": "err", "estimated": "est", "exhaust": "exh",
    "external": "ext", "frequency": "freq", "gyroscope": "gyro",
    "hydraulic": "hyd", "hydraulics": "hyd", "identifier": "id",
    "implement": "impl", "indicator": "ind", "information": "info",
    "inclination": "incl", "input": "in", "intake": "intk",
    "internal": "int", "left": "lft", "length": "len", "level": "lvl",
    "limit": "lim", "machine": "mach", "manifold": "manif",
    "maximum": "max", "measured": "meas", "message": "msg",
    "minimum": "min", "mode": "md", "motor": "mot", "negative": "neg",
    "number": "num", "operation": "op", "output": "out",
    "percent": "pct", "percentage": "pct", "position": "pos",
    "positive": "pos", "power": "pwr", "pressure": "press",
    "previous": "prev", "quality": "qual", "range": "rng",
    "rate": "rt", "reference": "ref", "request": "req",
    "requested": "req", "reserved": "rsvd", "resistance": "res",
    "response": "resp", "right": "rgt", "rotation": "rot",
    "selected": "sel", "selection": "sel", "sensor": "sens",
    "sequence": "seq", "setpoint": "sp", "signal": "sig",
    "source": "src", "speed": "spd", "standard": "std",
    "state": "st", "status": "stat", "steering": "steer",
    "switch": "sw", "system": "sys", "target": "tgt",
    "temperature": "temp", "threshold": "thr", "torque": "trq",
    "total": "tot", "transmission": "trans", "vehicle": "veh",
    "velocity": "vel", "voltage": "volt", "warning": "warn",
    "wheel": "whl",
}

NAME = r"[A-Za-z_][A-Za-z0-9_]*"


def split_words(name):
    """-> (words, separator). IMU_Accel_X -> (['IMU','Accel','X'], '_');
    EngineCoolantTemp -> (['Engine','Coolant','Temp'], '')."""
    if "_" in name.strip("_"):
        return [w for w in name.split("_") if w], "_"
    words = re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+", name)
    if "".join(words) != name:
        return [name], ""
    return words, ""


def _keep_case(orig, short):
    if orig[:1].isupper():
        return short[:1].upper() + short[1:]
    return short


def _stem(word):
    """The way people shorten a word they have no abbreviation for: up to the
    end of its first consonant run after a vowel. Guidance -> Guid,
    Curvature -> Curv, Engine -> Eng."""
    m = re.match(r"([A-Za-z]+)([0-9]*)$", word)
    if not m or len(m.group(1)) <= 4:
        return word
    word, digits = m.group(1), m.group(2)
    i = 1 if word[0] not in VOWELS else 0
    while i < len(word) and word[i] not in VOWELS:      # leading consonants
        i += 1
    while i < len(word) and word[i] in VOWELS:          # the first vowels
        i += 1
    while i < len(word) and word[i] not in VOWELS:      # and what closes them
        i += 1
    return (word[:max(i, 3)] if i < len(word) else word) + digits


def _devowel(word):
    if len(word) <= 3:
        return word
    return word[0] + "".join(c for c in word[1:] if c not in VOWELS)


def shorten(name, limit):
    """-> `name` in at most `limit` characters, as recognisable as possible."""
    if len(name) <= limit:
        return name
    words, sep = split_words(name)

    def fits():
        return len(sep.join(words)) <= limit

    order = sorted(range(len(words)), key=lambda i: -len(words[i]))
    for i in order:
        if fits():
            break
        m = re.match(r"([A-Za-z]+)([0-9]*)$", words[i])
        short = ABBREV.get(m.group(1).lower()) if m else None
        if short and len(short) + len(m.group(2)) < len(words[i]):
            words[i] = _keep_case(words[i], short) + m.group(2)

    for shrink in (_stem, _devowel):
        order = sorted(range(len(words)), key=lambda i: -len(words[i]))
        for i in order:
            if fits():
                break
            words[i] = shrink(words[i])

    # Letters off the longest word, down to two - the end of a name is usually
    # what tells it from its neighbours, so it is kept as long as possible.
    while not fits():
        i = max(range(len(words)), key=lambda k: len(words[k]))
        if len(words[i]) <= 2:
            break
        words[i] = words[i][:-1]

    return sep.join(words)[:limit]


def unique(name, taken, limit):
    """`name`, or a numbered variant of it, that is not in `taken`."""
    if name not in taken:
        return name
    for n in range(2, 100000):
        suf = str(n)
        cand = name[:max(1, limit - len(suf))] + suf
        if cand not in taken:
            return cand
    raise ValueError("no free name for %s" % name)


def _assign(names, limit, notes, kind):
    out, taken = {}, set()
    # Names that already fit are claimed first, so a shortened one can never
    # push an untouched one aside.
    for n in names:
        if len(n) <= limit:
            out[n] = n
            taken.add(n)
    for n in names:
        if n in out:
            continue
        s = shorten(n, limit)
        u = unique(s, taken, limit)
        taken.add(u)
        out[n] = u
        notes.append((kind, n, u, u != s))
    return out


def scan(text):
    """-> (messages, signals, nodes) in file order, each once."""
    msgs, sigs, nodes = {}, {}, {}
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"BO_\s+\d+\s+(%s)\s*:\s*\d+\s*(%s)?" % (NAME, NAME), s)
        if m:
            msgs[m.group(1)] = 1
            if m.group(2):
                nodes[m.group(2)] = 1
            continue
        m = re.match(r"SG_\s+(%s)" % NAME, s)
        if m:
            sigs[m.group(1)] = 1
            continue
        if s.startswith("BU_"):
            for n in re.findall(NAME, s.split(":", 1)[-1]):
                nodes[n] = 1
    return list(msgs), list(sigs), list(nodes)


def mapping(text, name_max):
    """-> (msg_map, sig_map, node_map, notes) for a map's text.

    name_max counts the nul, as the firmware does, so a name gets name_max - 1
    characters. Vector__XXX is the exporters' "nobody" and is never renamed.
    """
    limit = name_max - 1
    msgs, sigs, nodes = scan(text)
    notes = []
    msg_map = _assign(msgs, limit, notes, "message")
    sig_map = _assign(sigs, limit, notes, "signal")
    node_map = _assign([n for n in nodes if n != "Vector__XXX"], limit, notes,
                       "node")
    return msg_map, sig_map, node_map, notes


def _object_ref(text, s_, n_):
    """The SG_ or BU_ object a CM_ or BA_ line is about, renamed."""
    text = re.sub(r"^(SG_\s+\d+\s+)(%s)" % NAME,
                  lambda x: x.group(1) + s_(x.group(2)), text)
    return re.sub(r"^(BU_\s+)(%s)" % NAME,
                  lambda x: x.group(1) + n_(x.group(2)), text)


def rewrite(text, msg_map, sig_map, node_map):
    """The map's text with every name replaced by its short form."""
    def m_(n):
        return msg_map.get(n, n)

    def s_(n):
        return sig_map.get(n, n)

    def n_(n):
        return node_map.get(n, n)

    out = []
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        eol = line[len(body):]
        lead = body[:len(body) - len(body.lstrip())]
        s = body.lstrip()

        m = re.match(r"(BO_\s+\d+\s+)(%s)(\s*:\s*\d+\s*)(%s)?(.*)$" % (NAME, NAME), s)
        if m:
            s = m.group(1) + m_(m.group(2)) + m.group(3) + \
                (n_(m.group(4)) if m.group(4) else "") + m.group(5)
        elif re.match(r"SG_\s", s):
            m = re.match(r"(SG_\s+)(%s)(.*)$" % NAME, s)
            if m:
                rest = m.group(3)
                # receivers follow the unit string: "unit" A,B
                q = re.match(r'(.*"[^"]*"\s*)(.*)$', rest)
                if q:
                    rest = q.group(1) + re.sub(NAME, lambda x: n_(x.group(0)),
                                               q.group(2))
                s = m.group(1) + s_(m.group(2)) + rest
        elif s.startswith("BU_"):
            head, _, tail = s.partition(":")
            s = head + ":" + re.sub(NAME, lambda x: n_(x.group(0)), tail)
        elif re.match(r"(VAL_|SIG_VALTYPE_)\s", s):
            s = re.sub(r"^(\w+\s+\d+\s+)(%s)" % NAME,
                       lambda x: x.group(1) + s_(x.group(2)), s)
        elif re.match(r"SG_MUL_VAL_\s", s):
            s = re.sub(r"^(SG_MUL_VAL_\s+\d+\s+)(%s)(\s+)(%s)" % (NAME, NAME),
                       lambda x: x.group(1) + s_(x.group(2)) + x.group(3)
                       + s_(x.group(4)), s)
        elif re.match(r"CM_\s", s):
            # CM_ SG_ id Sig "text" - the comment itself is left alone
            head, quote, tail = s.partition('"')
            s = _object_ref(head, s_, n_) + quote + tail
        elif re.match(r"BA_\s", s):
            # BA_ "Attr" SG_ id Sig value;
            a = re.match(r'(BA_\s+"[^"]*"\s*)(.*)$', s)
            if a:
                s = a.group(1) + _object_ref(a.group(2), s_, n_)
        elif re.match(r"BO_TX_BU_\s", s):
            head, _, tail = s.partition(":")
            s = head + ":" + re.sub(NAME, lambda x: n_(x.group(0)), tail)
        out.append(lead + s + eol)
    return "".join(out)


def abbreviate(text, name_max):
    """-> (short text, msg_map, sig_map, node_map, notes)."""
    mm, sm, nm, notes = mapping(text, name_max)
    return rewrite(text, mm, sm, nm), mm, sm, nm, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("dbc")
    ap.add_argument("--name-max", type=int, required=True, choices=(16, 32, 64),
                    help="the name length of the setup bundle, nul included")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    with open(a.dbc, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    short, _, _, _, notes = abbreviate(text, a.name_max)
    with open(a.out, "w", encoding="utf-8", newline="") as fh:
        fh.write(short)

    print("%s -> %s   (name_max %d)" % (a.dbc, a.out, a.name_max))
    if not notes:
        print("  every name already fits; nothing changed")
        return 0
    numbered = sum(1 for n in notes if n[3])
    print("  %d name(s) shortened%s" % (
        len(notes), ", %d needed a number to stay distinct" % numbered
        if numbered else ""))
    for kind, old, new, num in notes[:40]:
        print("    %-8s %-40s -> %s%s" % (kind, old, new,
                                          "  (numbered)" if num else ""))
    if len(notes) > 40:
        print("    ... and %d more" % (len(notes) - 40))
    return 0


if __name__ == "__main__":
    sys.exit(main())
