#!/usr/bin/env python3
"""
Read a recording, check it, and turn it into something analysable.

The CSV the logger writes is deliberately in long form - one row per decoded
signal - so that a parser never has to know anything about the bus. This script
is that parser, and it does three jobs:

    python3 tools/parse_log.py 1.csv                 # summary and integrity
    python3 tools/parse_log.py 1.csv --list          # signals present
    python3 tools/parse_log.py 1.csv --wide out.csv  # one column per signal

`--wide` pivots the long form into a wide table: one row per frame timestamp,
one column per Message.Signal, which is what most plotting and analysis tools
want. Values are carried forward, because CAN signals are sampled at whatever
rate their message is sent at and a wide table needs every column populated.

Only the standard library is used, so this runs anywhere Python does.
"""
import argparse
import csv
import sys
from collections import Counter, OrderedDict

COLUMNS = ["t_us", "id", "name", "signal", "value", "unit", "raw"]

# Rows for frames the LOGGER sent, rather than received, carry the prefix on the
# message name. The schema is unchanged - seven columns, same order - so nothing
# that predates the transmit feature breaks; a reader that does not know about
# the prefix simply sees a message called "TX:WheelInfo".
TX_PREFIX = "TX:"


def is_tx(row):
    return row["name"].startswith(TX_PREFIX)


def base_name(row):
    """The message name with any direction marker stripped."""
    n = row["name"]
    return n[len(TX_PREFIX):] if n.startswith(TX_PREFIX) else n


def read_rows(path):
    """Yields (header_lines, row_dicts). Malformed rows are reported, not fatal:
    a recording cut short by a power loss ends in a partial line, and that is
    not a reason to refuse the 40 minutes before it."""
    header, bad, seen_columns = [], 0, False
    with open(path, "r", newline="") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if line.startswith("#"):
                header.append(line)
                continue
            if not line:
                continue
            fields = line.split(";")
            if not seen_columns:
                if fields == COLUMNS:
                    seen_columns = True
                    continue
                print(f"warning: expected the column header on line {lineno}, "
                      f"got {line[:60]!r}", file=sys.stderr)
                seen_columns = True
            if len(fields) != len(COLUMNS):
                bad += 1
                continue
            yield header, dict(zip(COLUMNS, fields))
            header = []
    if bad:
        print(f"note: skipped {bad} malformed row(s) - the last one is normally "
              f"a recording that lost power mid-line", file=sys.stderr)


def load(path):
    rows, header = [], []
    for hdr, row in read_rows(path):
        header.extend(hdr)
        rows.append(row)
    return header, rows


def summarise(path, header, rows):
    if not rows:
        print("no data rows")
        return

    t0 = int(rows[0]["t_us"])
    t1 = int(rows[-1]["t_us"])
    span = (t1 - t0) / 1e6

    frames = OrderedDict()          # (t_us, id) -> None, preserves order
    per_id = Counter()
    per_sig = Counter()
    raw_only = 0
    sent = 0
    sent_sig = Counter()

    for r in rows:
        key = (r["t_us"], r["id"])
        if key not in frames:
            frames[key] = None
            per_id[r["id"]] += 1
            if not r["signal"]:
                raw_only += 1
            if is_tx(r):
                sent += 1
        if r["signal"]:
            # Grouped under the message's real name, so a signal the logger
            # both watched and wrote is one row rather than two.
            per_sig[f'{base_name(r)}.{r["signal"]}'] += 1
            if is_tx(r):
                sent_sig[f'{base_name(r)}.{r["signal"]}'] += 1

    print(f"file        {path}")
    for line in header:
        if "FRAME MAP" in line or "Bus " in line or line.startswith("#   none"):
            print(f"            {line.lstrip('# ').rstrip()}")
    print(f"duration    {span:.3f} s")
    print(f"rows        {len(rows)}")
    print(f"frames      {len(frames)}  ({len(frames)/span:.0f}/s)"
          if span > 0 else f"frames      {len(frames)}")
    print(f"identifiers {len(per_id)}")
    print(f"undecoded   {raw_only} frame(s) stored as raw bytes")
    print(f"signals     {len(per_sig)}")
    if sent:
        print(f"transmitted {sent} frame(s) were SENT by the logger, not received")
        for name, n in sent_sig.most_common():
            print(f"            {name}  x{n}")

    print("\nper identifier:")
    for ident, n in per_id.most_common():
        rate = f"{n/span:8.1f}/s" if span > 0 else " " * 11
        print(f"  {ident:>12}  {n:>9}  {rate}")

    # Monotonic time is the one invariant worth checking: it is captured in the
    # interrupt, so a step backwards would mean the receive path reordered.
    back = sum(1 for a, b in zip(rows, rows[1:])
               if int(b["t_us"]) < int(a["t_us"]))
    print(f"\ntimestamps  {'monotonic' if not back else f'{back} step(s) BACKWARDS'}")

    gaps = [(int(b["t_us"]) - int(a["t_us"])) for a, b in zip(rows, rows[1:])]
    if gaps:
        print(f"largest gap {max(gaps)/1000:.1f} ms")


def list_signals(rows):
    seen = OrderedDict()
    for r in rows:
        if not r["signal"]:
            continue
        key = f'{base_name(r)}.{r["signal"]}'
        if key not in seen:
            seen[key] = (r["id"], r["unit"], r["value"], is_tx(r))
        elif is_tx(r) and not seen[key][3]:
            seen[key] = seen[key][:3] + (True,)
    if not seen:
        print("no decoded signals - this recording has no frame map behind it")
        return
    print(f'{"signal":<40} {"id":>12}  {"unit":<10} {"":<4} example')
    for key, (ident, unit, example, tx) in seen.items():
        print(f"{key:<40} {ident:>12}  {unit:<10} "
              f"{'TX' if tx else '':<4} {example}")


def to_wide(rows, out_path):
    columns = OrderedDict()
    for r in rows:
        if r["signal"]:
            columns.setdefault(f'{base_name(r)}.{r["signal"]}', None)
    if not columns:
        sys.exit("nothing to pivot: this recording carries no decoded signals")

    names = list(columns)
    current = {n: "" for n in names}
    written = 0

    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["t_us"] + names)

        pending_t = None
        for r in rows:
            t = r["t_us"]
            if pending_t is not None and t != pending_t:
                w.writerow([pending_t] + [current[n] for n in names])
                written += 1
            pending_t = t
            if r["signal"]:
                current[f'{base_name(r)}.{r["signal"]}'] = r["value"]
        if pending_t is not None:
            w.writerow([pending_t] + [current[n] for n in names])
            written += 1

    print(f"wrote {out_path}: {written} rows x {len(names)} signals")
    print("values are carried forward between updates - each signal only "
          "changes when its own message arrives")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="a recording written by the logger")
    ap.add_argument("--list", action="store_true", help="list the signals present")
    ap.add_argument("--wide", metavar="OUT.csv",
                    help="pivot to one column per signal")
    args = ap.parse_args()

    header, rows = load(args.csv)

    if args.list:
        list_signals(rows)
    elif args.wide:
        to_wide(rows, args.wide)
    else:
        summarise(args.csv, header, rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
