# Changelog

## Unreleased

### The unit is the frame

The rule that survived every rewrite of this section: **a frame is one message,
and - when the message is multiplexed - one selector code.** Its values are
added, removed and sent together, under one button.

That is not a policy, it is what a CAN frame is. Eight bytes leave the logger
whether or not somebody typed all of them, so the page no longer pretends one
signal of a frame can be sent by itself.

The correction from the last release is that a multiplexed message holds
*several* frames, and they are separate things. `ABS_Cmd` with six opcodes is
now six boxes and six buttons, not one box and one press that emitted six
frames. And `Diagnostics`, whose `Page 0` carries `SupplyVoltage` **and**
`BoardTemp`, is two boxes of two: you cannot have one of those without the
other, because one frame carries both fields and sending one alone asserts a
value for the other that nobody chose.

### Multiplexing you can declare yourself

For a `.dbc` that multiplexes in fact and does not admit it - payloads on the
same bits, an opcode in front of them, no marker anywhere - the setup sheet now
offers a picker on the message: **which signal selects**. Choosing it drops the
selector out of the value list (it is written for you from then on) and gives
every remaining value a **selector code** field.

Both halves are required. The previous release inferred multiplexing from "more
than one signal is set up", which was a guess dressed up as a rule: it grouped
the signals but could not say what code each belonged to, so the frame went out
under opcode 0 whatever the payload. Knowing that `Cmd_Op` selects does not say
that `Cmd_Amp` means opcode 16, and nothing in the file can be read to find out.

Stored as `msel=` and `mxc=` on the `send` lines. An override is dropped rather
than half-obeyed when a later frame map has no signal of that name in the
message, or declares its own `M` - the file wins.

`group=` and `mux=1` are gone from what is written; both are still read from an
older file and ignored.

### A plain signal in a multiplexed message is refused, and says so

A signal with no `m<code>` in a multiplexed message rides in *every* frame that
message sends, so it belongs to no one selector code and this build has nowhere
to put it. It is now left out of the sendable list and named, in the page and in
`tools/check_dbc.py`, instead of quietly going out as zero under a code it does
not belong to. Decoding is unaffected. Recorded in **Known issues** with what it
costs and when it will bite - a counter or CRC the ECU checks.

None of the 15 `.dbc` files this was developed against has one; the shape is
normal in OEM and AUTOSAR-derived files.

### Whole frames only

Half a frame set up is a frame that still goes out, with the signals nobody
configured as zeros. So choosing one signal by hand brings its whole frame with
it, a frame that will not fit in the remaining room is not added at all rather
than added in part, and a setup file written before this rule - or carried
across to a map where the message has gained a signal - is completed on load and
told to you.

### Dashboard and Send tab, visually

- **Every dashboard row is the same height**, the height of the tallest widget
  in the grid, instead of each row shrinking to its own contents. Done with
  `grid-auto-rows:1fr` rather than a fixed number, so nothing here has to be
  kept in step with the widgets.
- **A plain number cell centres its number.** The empty widget area above it was
  pushing the value onto the floor of the cell; it now sits where a gauge's
  needle would be.
- **A number you can send has a `-` and a `+` beside it** instead of the
  browser's own spinner, whose arrows are a few pixels tall and are missing
  altogether on a phone. Stepping is by the value's own step, rounded so ten
  presses of 0.1 read 1 and not 0.9999999999999999. The buttons write nothing to
  the bus, so they stay live when the logger is not armed.

### The desk tools write no files; the logger still saves by itself

`customize.py` and `tools/preview_dashboard.py` **no longer write anything at
all.** They no longer pair a `.cfg` with every `.dbc` they are shown, so loading
a frame map leaves nothing beside it and the page does not come up on a setup
you did not ask for. `--cfg` is read once at the start and never written back;
`--cfg-dir` is gone; **Export** in the page is the one way a setup comes out.

Saving on the logger is unchanged: an edit reaches it about a second after you
stop making it. A **Save to device** button was tried and removed in the same
breath, because it broke the dashboard - `handleDash()` renders the values from
the layout the LOGGER holds, so a cell it has not been told about shows nothing,
and an editing session that had not reached it was one with half the screen
blank. The button is gone; the note in `webpage.cpp` says why, so it does not
get reinvented.

### What you type stays typed

The Send tab redraws whenever anything changes - arming, a poll, a frame
arriving - and every redraw used to put the inputs back to their configured
defaults, throwing away what had been typed. Each box now keeps its values
across redraws and after sending, with a **Reset** to put them back. Kept in the
page only; reloading clears it and nothing reaches the logger.

## v1.1.1

Seven bugs in v1.1.0. The worst of them silently deleted work.

### Multiplexed, or not - the three kinds are down to two

`single`, `grouped` and `multiplexed` were three names for one question, and the
answer was in the frame all along. A sendable frame is now either multiplexed or
it is not, and it is multiplexed when the `.dbc` says so **or** when more than
one of its signals has been set up.

Everything set up against one message is that message's frame, so its signals
are added, removed and sent **together, under one button**. There is no
`group=` on the `send` lines any more (one on an older file is ignored), no
"sent together with" dropdown, and no manual grouped/multiplexed picker: nothing
is left to decide, because a frame is a frame.

The half of the rule that does not come from the file is what makes a `.dbc`
which is multiplexed *in fact* and does not say so - overlapping signals, no
`M`/`m` marker, common on bench command frames - work without a guess or a
switch.

One press still means one frame per **selector code**: payloads under different
codes are alternatives, so a frame multiplexed into six opcodes goes out as six
frames from one press, while a message with one code, or none, is one frame.

### A multiplexed command could be sent half a frame at a time

The editor drew a multiplexed message as one indivisible box — no Remove except
the group's — while the Send tab gave every one of its payloads a Send button of
its own. Both could not be right, and the Send tab was the wrong one: two
payloads under the same selector code are one frame, and sending one of them
alone writes **zero** over the other, because a multiplexed frame cannot be
seeded from what the bus last carried (those bytes belonged to another code and
mean something else).

The rule now, in both tabs and from one function, is that a frame is one message
and — if the message is multiplexed — one CODE of it:

- **single** — one value, alone in its frame. Two payloads under *different*
  codes are two singles, not two halves of one command: they are alternatives
  and can never share a frame.
- **grouped** — several signals of a plain message, chosen to go out together.
  The choice is real and can be undone; the individual Remove stays.
- **multiplexed** — the payloads that share one code. No choice, no individual
  Remove, one **Send all N**.

*Fill from the frame map* now makes a group per selector code rather than
leaving multiplexed payloads ungrouped, and the "sent together with" list no
longer offers to put two codes in one frame — a frame that could never have gone
out as asked. A setup file written before this can still contain one, so
`cantx.cpp` refuses it as well rather than transmitting a single frame with the
last member's opcode written over both payloads.

**The kind can be corrected by hand.** The box header carries a **grouped /
multiplexed** picker, because a `.dbc` that is multiplexed in fact and does not
say so is common and no tool can infer it. The file's answer is the default;
the choice is kept in the setup file and holds with no frame map loaded.

`tools/check_dbc.py` also warns when a message the file does **not** mark as
multiplexed has signals sharing bits — the shape of a frame that is multiplexed
in fact and does not say so. Nothing can infer that (guessing would refuse
legitimate frames), so it is reported rather than assumed.

### Remove deleted every value of a message except the one you pressed

`renderTxEdit()` declared `var mates` twice in one function scope — once for the
values that are **removed together** (a multiplexed set, *including* this card)
and, two hundred lines later, once for the values that are **sent together** in
one frame (*excluding* it). `var` is function-scoped, so the second declaration
reached back and overwrote the first for the Remove button, which had already
closed over it. The button's label was computed before the overwrite, so it went
on reading *"Remove all 4"* while doing something else entirely.

The result, on any message with more than one value set up:

- **Remove** on a plain value deleted the message's *other* values and kept the
  one you asked it to delete. Four sendable values, one press, three gone.
- **Remove all N** on a multiplexed payload left that payload behind — the
  half-described command the grouping exists to prevent.

Legal JavaScript, no syntax error, no warning. `test/run_tests.sh` now walks
every function in the page — including anonymous callbacks, which is where this
one lived — and fails on a `var` declared twice in one scope. Verified by
putting the bug back and watching the check catch it.

### Multiplexed values stopped being a set when the frame map was away

Whether a value belonged to a multiplexed message was looked up in the frame
map. With no map loaded — the desk tool before a `.dbc` is chosen, which is now
its normal starting state — the answer came back "not multiplexed", the set
dissolved, and the payloads became deletable one at a time.

The fact now travels with the value as `mux=1` in the setup file rather than
being re-derived, so the grouping holds with no map at all. Values written by an
earlier version carry no flag and regain their grouping the next time they are
filled from a map.

### Loading a frame map left the old setup behind

A new `.dbc` replaced the map but not the dashboard or the sendable values
built against the old one. Every cell then read as an unknown, the stale
references were **saved back over the setup file**, and the next run loaded
them again — so the mess persisted across restarts and looked like the tool had
forgotten which frame map it was on.

Loading a map now removes every cell and every sendable value the new map
cannot account for, closes the gaps, and saves — on the logger and in the page,
so the card and the screen agree. An unrelated file leaves an empty setup; a
corrected version of the same file leaves the layout intact, because its
signals are still there. One-off frames named by identifier are never touched.

This is deliberately **not** what happens at boot, where an unresolvable cell is
still kept and drawn as such: there the usual cause is a card with no DBC on it,
and throwing away a layout over that would be far worse.

### ...and the desk tool handed it straight back the next morning

The fix above was the logger's half. `customize.py` still opened on **one shared
setup file**, written by whatever map was loaded last — so running it with no
argument re-seeded a setup built for another bus, against no frame map at all, and the page came up on a screen of `unknown` cells with a role naming a
node that was nowhere in sight. Exactly the symptom the fix above was meant to
end, arriving by a different door.

Three things were wrong, and all three are fixed:

- **One setup per frame map, always.** The `.cfg` is named after the `.dbc`. With
  no map named on the command line the page starts *empty*, and loading one from
  the page opens that map's own setup — beside the `.dbc` when there is one,
  beside `customize.py` when the map came from the file picker. Loading a second
  map in the same session switches to its file and leaves the first exactly as
  it was, instead of writing the new bus over the old bus's setup.
- **The preview server never pruned its own copy.** Only the browser did, and
  only the browser's save put the result on disk. It now applies the same rule
  as `dashDropUnresolved()` in the firmware, and `test/run_tests.sh` asserts the
  two implementations agree — same survivors, same order, same answer about the
  role — because one rule with two implementations is how this came back in the
  first place.
- **A role outlived the map that named it.** `role "Tester"` against a file with
  no `Tester` node is not stale but unanswerable: nothing transmits under that
  name, so both Fill buttons stop separating anything while the header goes on
  claiming a role. It is now cleared with everything else the new map cannot
  account for, and the question is asked again.

The page also **re-reads** the setup after a map is loaded rather than pruning a
second copy of it in the browser. Two copies pruning themselves independently is
how a browser holding the old layout gets to write it back over a card that had
just been cleaned.

### Values from a swapped-out map could be deleted one at a time

A third route to the same broken set: a value whose message is no longer in the
loaded map is not recognised as multiplexed either. The clearing above removes
those values outright, so the case no longer arises.

### The editor now boxes a message's values together, like the Send tab

Four cards in a row with four Remove buttons invites you to treat them as four
independent things. On the wire they are one frame, and the Send tab has always
drawn them that way. The editor now does too: one box per message, the message
name and what the grouping means in its header, and a **Remove all N** on the
header.

Inside the box the rule follows what the frame actually allows:

- **Multiplexed payloads** have *no* individual Remove. The group's button is the
  only one, because keeping some payloads of a multiplexed command describes only
  part of it.
- **A plain message whose signals go out together** keeps a Remove on each value
  as well, because wanting three of its four signals is a legitimate thing to
  want.

Worth knowing when reading your own file: a message is only multiplexed if the
`.dbc` says so with `M` and `m0`/`m1` markers. `MachineConfig` in the example and
`ABS_Cmd` in a typical tester file have overlapping signals but no markers, so
they are plain groups — they are sent whole, and their values may be removed one
at a time.

### `customize.py` asked which .dbc to use

It should never have. With no argument it now opens the page with no frame map
and lets the **Frame map** button load one — the same button the logger has.
A path still works as an argument, `--browse` still opens the file dialog, and
the interactive chooser is deleted rather than merely bypassed.

## v1.1.0

### Load a frame map from the web app

**Frame map**, in the header, takes a `.dbc` off the phone or laptop you are
holding and writes it to the card as `/frames.dbc`. The map is rebuilt on the
spot and the dashboard re-binds to it, reporting how many saved cells no longer
match. No card reader, no laptop, no reboot.

It is streamed to the card a chunk at a time rather than buffered: a real
machine's `.dbc` is ninety kilobytes, larger than the map built from it and a
third of the chip's free heap. It lands on a temporary name and is renamed into
place only once the whole file has arrived, so a dropped connection costs the
upload rather than the map already in use.

Refused while a recording is running, and it says why: every CSV opens with a
header naming the exact map its rows were decoded through, so swapping the map
mid-file would make that header wrong for every row after the swap.

### "Which node is this logger?", asked first this time

A `.dbc` names who transmits each message but never which of those nodes is the
box running this firmware — and that is the whole difference between a reading
and a command. The question is back, with three things fixed:

- **It is asked first.** The sheet opens by itself the moment a frame map is
  loaded, which is when it costs nothing to answer. Previously it lived inside
  the setup-file sheet, which is the last thing anyone opens, so by then the
  work it would have saved had been done by hand.
- **Skip is a first-class answer**, and the default. Both Fill buttons then
  offer every message and nothing is separated. On a machine that already works
  none of the nodes in the file is you, which makes skipping the common case,
  not a refusal to answer.
- **The answer is visible.** A **Role** button in the header shows it on every
  tab and changes it any time. A wrong role does not announce itself; it just
  fills the wrong half of the map into the wrong screen.

`customize.py --role <Node>` answers it before the page opens, and leaving the
argument out is the skip. Stored as `role "<Name>"` in `/dash.cfg`; a file
written when the setting was called `node` still keeps its answer.

### Renamed: customise → customize

`customize.py`, `customize.bat`, and every occurrence in the docs and the page.
One spelling, and it is the one the rest of the project already used.

### Also

- The Send tab's Fill button now describes what it will actually do — *"Add
  every signal Host sends"* with a role set, *"in every message"* without one,
  *"in this message"* when one is chosen — instead of always claiming "this
  message".

## v1.0.0

First tagged release.

**Flashing it:** one file at offset `0x0`, the same on macOS, Linux and Windows.

```bash
pip install esptool
python3 tools/flash.py --image can-logger-esp32-v1.1.1-4mb-merged.bin
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
- **Set up before you go out.** `customize.py` opens the real page against your
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
