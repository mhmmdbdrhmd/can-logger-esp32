# Changelog

## v1.0.0

First tagged release.

**Flashing it:** one file at offset `0x0`, the same on macOS, Linux and Windows.

```bash
pip install esptool
python3 tools/flash.py --image can-logger-esp32-v1.0.0-4mb-merged.bin
```

### What it is

An ESP32 + MCP2515 CAN logger that writes every frame to an SD card as CSV,
decodes it live against a DBC file on the card, and serves a web app over its
own Wi-Fi hotspot. Nothing about any particular bus is compiled in.

- **Recording.** Identifiers, timestamps and payloads to `N.csv`, decoded
  through `/frames.dbc` when one is present and as raw bytes when it is not.
  Values are computed in integer arithmetic wherever the factor and offset are
  decimal, so the CSV is bit-exact with respect to the wire.
- **A dashboard you lay out yourself.** A grid up to 6 × 8, ten ways to draw a
  value, drag to rearrange, thresholds per cell. Filled from the frame map in
  one press. Every widget is SVG built in the browser; the ESP32 renders
  nothing.
- **Writing back to the bus.** Values set up at a desk and sent in the field,
  behind an arm gate that expires. Multiplexed commands carry their own
  selector; signals that are only meaningful together leave in one frame.
- **Set up before you go out.** `customise.py` opens the real page against your
  own `.dbc` with simulated data, on any of the three platforms, and writes
  what you build next to it.
- **Diagnostics that are about the receive path**, not just the SD card: the
  interrupt line, sticky controller flags, queue depth, bus load, and frame
  loss reported as the floor it is.

### Known limits

- **The receive path is field-proven; the rest of this build is not.** The
  reader task, the driver and the controller configuration are unchanged from a
  firmware that recorded hours of a ~540 frame/s bus with zero frames lost on
  real hardware. Transmitting, the dashboard and the heap-sized frame map have
  not been on a live wire. Bench those against a node you can afford to
  confuse.
- 32 sendable values is a hard ceiling: whether a value repeats is a bit in a
  32-bit mask, in the firmware and in the browser alike.
- The frame map is sized to your file but bounded by `DBC_HEAP_RESERVE`, which
  keeps memory back for Wi-Fi. A very large map is trimmed and says so.
- Classical CAN only. The MCP2515 has no CAN FD.

### Fixed in this release, found in ~10 h of field recordings

The rates and counts below were measured off those recordings with tooling that
is not part of this repository, so they are **not reproducible from a clone** —
they are why these changes exist, not claims this repo can re-check. Everything
under *Verified* is.

- **The receive interrupt could latch and stay latched.** `ERRIF`/`MERRF` are
  sticky and the INT pin is level-active, so one uncleared flag killed every
  future edge and dropped the receiver to a 20 ms poll — about 135 frames/s
  where the bus was carrying 540. Four of ten recordings were affected, three of
  them from the first byte because the state survived from whatever ran before.
  Error flags are now cleared on every service pass, in an order that cannot
  re-set them, and the pass runs on a timeout rather than only on an interrupt
  so a fix on the interrupt path cannot be dead code exactly when it is needed.
- **The per-identifier log line printed uninitialised memory** when no traffic
  had arrived, truncated its own totals away on any bus with more than about
  nine identifiers, and cut its last entry mid-number by adding `snprintf`'s
  return value blind. It now reports totals first and rotates its window, so a
  hundred-identifier bus is fully described over a minute of lines.
- **Frame loss was understated by about 40 %.** The counter was incremented
  once per service pass that found an overflow, and reported as a frame count;
  in the latched state that made it converge on a poll rate. Overflow *events*
  and a *lower bound* on frames are now separate, and the page says `≥`.
- **The frame map dropped everything past 256 signals.** A 104-message,
  707-signal bus decoded 256 and logged the other 451 as raw payload. The
  tables are counted from the file and allocated to fit it.
- **Signal names were cut at 23 characters**, so `GuidanceCurvatureCommand`
  became `GuidanceCurvatureComman` — long enough to look right, short enough
  that matching CSV rows back to the source DBC by name returned nothing. The
  limit is 31, anything still too long is counted and reported, and every CSV
  header states the cap.

### Verified

`./test/run_tests.sh` — 311 assertions, natively, against the real sources.
`pio run` — RAM 24.9 %, Flash 54.7 %. The web app is driven headlessly against
a simulator, and every figure in the README is reproducible by a command in it.
