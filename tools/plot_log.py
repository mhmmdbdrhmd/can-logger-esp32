#!/usr/bin/env python3
"""
Plot signals out of a recording.

    python3 tools/plot_log.py 1.csv                      # list what is there
    python3 tools/plot_log.py 1.csv NodeStatus.Uptime
    python3 tools/plot_log.py 1.csv 'Motor.*' -o out.png

Signal names are Message.Signal as they appear in the CSV, and may be shell-style
patterns. Signals sharing a unit are drawn on the same axis; different units get
their own subplot, because overlaying a temperature on a pressure produces a
picture that looks informative and is not.

Needs matplotlib (`pip install matplotlib`). Everything else in tools/ is
standard library only - this is the one that is not, which is why plotting lives
here and not in parse_log.py.
"""
import argparse
import fnmatch
import sys
from collections import OrderedDict

try:
    import matplotlib
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is not installed:  pip install matplotlib")

COLUMNS = ["t_us", "id", "name", "signal", "value", "unit", "raw"]


def load(path):
    """signal -> (unit, [t_seconds], [values]). Non-numeric values (the symbolic
    text a DBC value table produces) are mapped onto their order of appearance,
    so an enumerated state still plots as a step trace."""
    series = OrderedDict()
    enums = {}

    with open(path, "r", newline="") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            f = line.split(";")
            if len(f) != len(COLUMNS) or f == COLUMNS:
                continue
            _, _, msg, sig, value, unit, _ = f
            if not sig or value == "":
                continue

            key = f"{msg}.{sig}"
            try:
                v = float(value)
            except ValueError:
                table = enums.setdefault(key, {})
                v = float(table.setdefault(value, len(table)))

            unit_t, ts, vs = series.setdefault(key, (unit, [], []))
            ts.append(int(f[0]) / 1e6)
            vs.append(v)

    return series, enums


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("signals", nargs="*",
                    help="Message.Signal names or patterns; omit to list them")
    ap.add_argument("-o", "--out", help="write a PNG instead of opening a window")
    args = ap.parse_args()

    series, enums = load(args.csv)
    if not series:
        sys.exit("no decoded signals in this file - it was recorded without a "
                 "frame map, so there is nothing to plot")

    if not args.signals:
        print("signals in this recording:")
        for key, (unit, ts, _) in series.items():
            print(f"  {key:<40} {unit or '-':<10} {len(ts):>8} samples")
        return 0

    chosen = [k for k in series
              if any(fnmatch.fnmatch(k, p) for p in args.signals)]
    if not chosen:
        sys.exit(f"nothing matched {args.signals}. Run without arguments to list "
                 f"what is in the file.")

    # One subplot per distinct unit.
    by_unit = OrderedDict()
    for key in chosen:
        by_unit.setdefault(series[key][0], []).append(key)

    if args.out:
        matplotlib.use("Agg")

    fig, axes = plt.subplots(len(by_unit), 1, sharex=True,
                             figsize=(11, 2.6 * len(by_unit) + 1.2), squeeze=False)

    for ax, (unit, keys) in zip(axes[:, 0], by_unit.items()):
        for key in keys:
            _, ts, vs = series[key]
            ax.step(ts, vs, where="post", linewidth=1.1, label=key)
        ax.set_ylabel(unit or "value")
        ax.grid(alpha=.3)
        ax.legend(loc="upper right", fontsize=8)

        if len(keys) == 1 and keys[0] in enums:
            table = enums[keys[0]]
            ax.set_yticks(list(table.values()))
            ax.set_yticklabels(list(table.keys()), fontsize=8)

    axes[-1, 0].set_xlabel("time since the start of the recording [s]")
    fig.suptitle(args.csv, fontsize=10)
    fig.tight_layout()

    if args.out:
        fig.savefig(args.out, dpi=140)
        print(f"wrote {args.out}")
    else:
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
