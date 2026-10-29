# Tool reference

All 61 tools the server exposes. Arguments marked **bold** are required; everything else has
a default. Numeric arguments accept a number or a string in any of the usual notations:
`32768`, `"0x8000"`, `"$8000"`, `"#8000"`, `"%1000000000000000"`.

**Every argument that means an address also accepts a label name** once `load_labels` has
run - `address`, `from`, `to`, `stop_pc`, `port`, `set_register`'s `value`, and the `start`
and `end` of a `profile` range:

```jsonc
set_breakpoint {"address": "main_loop"}
coverage       {"action": "read", "from": "draw_start", "to": "draw_end"}
profile        {"action": "start", "ranges": [
                  {"tag": "pack", "start": "pack_attrs", "end": "im2_init"}]}
```

The TR-DOS ROM's entry points have names of their own that need no symbol file:
`trdos.read_bytes` (`#3FE5`), `trdos.write_bytes` (`#3FCA`), `trdos.out_c` (`#2A53`),
`trdos.ret` (`#3D2F`), `trdos.call` (`#3D13`) and the rest listed under
[Disk controller](#disk-controller-tr-dos).

A string that is neither a number nor a known label is an **error**, never a silent zero:
`no such label 'pack_atrs' (start takes a number, $hex or a label name; 158 labels loaded
from …)`. The same goes for plain numeric arguments - an unreadable value is refused rather
than replaced by the default.

Every tool returns JSON as text. A failing tool returns `isError: true` with the reason and
leaves the machine running - the server does not die on a bad call.

A single address in a reply comes as a pair, `pc` and `pc_hex`, `address` and `address_hex`.
In a list - `trace`, `disassemble`, `assemble`, `beam_log` events, `source_at`, the
`resolve_symbol` table, the `load_labels` sample - each row carries the `$hex` form alone: the
decimal twin was a quarter of every row, and `$hex` is what goes back into an argument.

---

## Machine

The emulator's own configuration decides what a model *means*: name a machine and its
romset, RAM size and screen geometry come from the profile Xpeccy itself would boot.

| Tool | Arguments | What it does |
|---|---|---|
| `settings` | `action`, `name`, `value`, `save`, `path` | Every emulator setting this server can change, what it is set to, where that value came from, and what changing it costs. No arguments reports all of them; `name` with `report` returns just that one row. |
| `machine_config` | `model`, `memory`, `romset`, `geometry`, `boot_frames` | Read or change the machine. `model` takes a hardware name or a machine-definition id (`zxplus2`, `evo-tsconf`) - several definitions can share one hardware, and the id picks exactly one. Any change resets and re-boots it, and the reply describes the machine after the boot. An unknown `romset` or `geometry` is refused before anything changes; a step that fails after others went through (a romset whose files cannot be read) still resets, and the error lists what was applied. No arguments = report the current setup. |
| `list_models` | - | Every hardware name the core supports, the loaded machine definitions (`machines`: id, display name, hardware), plus the romsets and geometries defined in the configuration. |
| `machine_state` | - | Model, the machine definition applied (`machine`), RAM, romset, CPU and clock, frame timing, screen size, the disk controller, and which ROM/RAM bank is visible at `$0000/$4000/$8000/$C000`. |
| `reset` | `mode`, `boot_frames` | Reset (`default`, `48`, `128`, `dos`, `shadow`) and run `boot_frames` (default 100) so the machine reaches its prompt. |
| `video_config` | `blend`, `gamma`, `frame_history`, `greyscale`, `border_size` | Read or change how frames become pictures. Sets the default blend for every later `screenshot`, `frame_digest` and `record_video`. No arguments = report. |

`machine_config {"model": "ZX48K"}` is usually all you need; `memory` is clamped to what the
model actually supports.

`reset` picks the ROM page, which on a 128K machine is four different machines: `128` boots
the 128 menu, `48` the 48 BASIC, `dos` TR-DOS, and `shadow` whatever service ROM the romset
puts in page 2 - a Gluk or a Quick Commander, if the romset has one.

**`disk_interface`** in `machine_state` is the controller the model came with - Beta Disk on
the Soviet clones, uPD765 on the +3, none elsewhere. It is set for you; the reason it is
worth reporting is that a ZX machine without one cannot boot TR-DOS, and the symptom is a
screen of uninitialised RAM that looks exactly like a corrupt ROM.

A name `list_models` does not know is refused with "unknown machine", and a machine the
headless core cannot run is refused with the reason - `machine_config` leaves the machine
you already had running rather than half-applying the new one. Machines in the list whose
definition carries no hardware (a user file that inherits a user-only parent, where the
parent's own keys never cross) are visible but cannot be selected - same as upstream.

A reset leaves the machine on whatever `reset` its definition declares: Pentagon's
`resbank` is `RES_128`, so it comes up on the 128K menu, not in BASIC - `press_key`
`down` x3 and `enter` chooses 48 BASIC, then its self-test runs. And a program that
enables interrupts on a machine that has not booted should expect the Pentagon's IM1 to
land on the RAM trampoline at `$5B00` that only the boot writes - cold RAM makes it an
`rst #38` loop.

## Execution

| Tool | Arguments | What it does |
|---|---|---|
| `run` | `max_instructions`, `stop_pc`, `max_frames` | Run until a breakpoint, `stop_pc`, a frame count or the instruction budget (default 10 000 000). Video and interrupts stay live. |
| `run_frames` | `count` | Run exactly N timing-accurate frames. One frame is 1/50 s of emulated time. |
| `step` | `count` | Execute N steps, ignoring breakpoints. A step is one instruction or the CPU taking a pending interrupt, which runs no instruction and lands on the handler; those are counted in `interrupts`, not `instructions`. |
| `step_over` | - | One instruction, but `CALL`/`RST`/block instructions run to completion. |
| `step_out` | - | Run until the current subroutine returns. |

Every one of these returns a stop `reason`: `steps`, `breakpoint`, `pc`, `frames`, `budget`,
`return`, or `fdc` with the disk controller's event in `fdc_event` (see `fdc_break`), along with the new `pc`, the instruction count, and both `ns` and **`t_states`**.
Take the T-states: they are the exact count the cpu charged, contention waits included;
`ns` is derived from them for convenience and cannot be converted back exactly.
`run {"stop_pc": "main_loop"}` therefore measures a precalculation in one call, with no
profiler wrapped around it.

## CPU and memory

| Tool | Arguments | What it does |
|---|---|---|
| `get_registers` | - | Main and shadow registers, the flags string, IM/IFF, halt state, T-states into the frame. |
| `set_register` | **`name`**, **`value`** | Set one register by the name `get_registers` reports. |
| `read_memory` | **`address`**, `length`, `format` | Read as the CPU sees it, through the current paging. `"bank:offset"` (both hex, like a symbol file) reads a physical 16K RAM bank directly, paged in or not. `FF:` means "no bank", as in a symbol file, and reads the CPU address. A bank past the machine's RAM, an offset past `$3FFF` and a read that would run off the end of the bank are refused rather than wrapped: the next bank by number is not what follows in CPU space. Returns a hex string by default; `format:"bytes"` returns a decimal array, `format:"both"` returns both. |
| `write_memory` | **`address`**, `bytes` or `hex` | Write through the CPU mapping. Writes to a ROM page are dropped by the hardware - read back if it matters. |
| `find_bytes` | **`pattern`**, `from`, `to` | Search the address space for a hex pattern (`"3E 05 C9"`). Returns the first address or −1. |

## Code

| Tool | Arguments | What it does |
|---|---|---|
| `disassemble` | `address`, `count`, `t_states` | Disassemble from an address (default PC). Each line carries `address_hex`, text, the raw `bytes`, the jump target as `target_hex`, and - when loaded - the label and the original source line. |
| `assemble` | **`address`**, `lines` or `text`, `write` | Assemble Z80 mnemonics and write them to memory. Numbers may be decimal, `0x`, `$` or `#`; labels work once `load_labels` has run, and the block may define its own. Stops at the first line that fails. |

```jsonc
{"name": "assemble", "arguments": {
  "address": "$8000",
  "lines": ["di", "ld hl,$4000", "ld (hl),255", "jr $8003"]}}
```

### Labels in the block itself

Write `name:` and jump to it by name, in either direction - no address arithmetic:

```jsonc
{"name": "assemble", "arguments": {
  "address": "$8000",
  "lines": ["top: dec a", "jr nz,top", "call sub", "jr done",
            "sub: ret", "done: jp top"]}}
```

The addresses the block assigned come back in `labels`, so the next call can aim at them. Two
passes make the forward references work: the first places the labels, the second assembles.
Instruction length on a Z80 follows the mnemonic rather than the operand's value, so the
addresses the first pass produces are the final ones.

Names are refused rather than silently swallowed in two cases. A label called `a`, `hl`, `c`
or any other register or condition name would be substituted into the place a register belongs
- `c:` would quietly turn `jr c,x` into a jump to a number - and a name defined twice has no
single address. The mnemonic at the head of a line is never substituted, so `ret:` is a
perfectly good label and `ret` still assembles.

Directives (`org`, `db`, `equ`) are still not supported: this assembles instructions.

Whitespace is yours to spend. The core's own assembler wants exactly one space in the line and
none anywhere else, so `ld a, 5` - the way every source file writes it - used to come back as
`can't assemble`, and looked for all the world like the label feature being broken. Lines are
now squeezed into the shape it wants before it sees them: `ld a, 5`, `ld  a,  5`,
`ld a, (hl)` and `ld a, (ix + 5)` all assemble.

### `disassemble {"t_states": true}`: what a routine costs, without counting by hand

Adds `t_states` to every line and `t_states_total` to the result, so "where do the 59 T per
cell come from?" is a question the server answers rather than one you count off a table:

```jsonc
disassemble {"address": "pack_attrs", "count": 40, "t_states": true}
→ {"text": "ld a,(ix+#05)", "t_states": 19}
  {"text": "jr nz,#800A",   "t_states": 12, "t_states_not_taken": 7, "t_states_text": "12/7"}
```

The numbers are **measured, not tabulated**: each instruction is executed in a throwaway CPU
with its own memory and dead I/O, and the T-states it charges are read back. Prefixes, `(ix+d)`
forms and block instructions therefore come out right without a special case anywhere.

A conditional instruction is run both ways and reports both outcomes; `t_states` is the taken
cost, `t_states_not_taken` the fall-through, and `t_states_text` is the familiar `12/7`.
`t_states_total` then assumes every branch is taken and `t_states_total_min` that none is -
for straight-line code, which is what you are usually costing, there is a single total.

These are uncontended timings, which is the truth on a Pentagon; on a machine with contended
memory the real cost depends on where the beam is.

## Breakpoints

| Tool | Arguments | What it does |
|---|---|---|
| `set_breakpoint` | **`address`**, `access`, `scope` | Break on `exec` (default), `read`, `write`, or a combination like `"read,write"`. |
| `set_port_breakpoint` | **`port`**, `access` | Break on an I/O port read or write. A port up to `$FF` matches on the low byte whatever is on the high one, so `$FE` catches `out ($FE),a` with any A; a 16-bit port above `$FF` (`$7FFD`) matches exactly. The reply's `match` says which. |
| `clear_breakpoints` | - | Remove everything this server armed, memory and I/O alike, and the disk controller's `fdc_break` stops. |

### Which bank the breakpoint lands in

Breakpoints live in the memory cell itself, so they follow the *bank*, not the CPU address -
a breakpoint in a paged-out bank still fires when that bank comes back at a different address.

That is only useful if the right cell got marked in the first place. A plain address can only
mean the cell mapped there *at the moment you arm it*: ask for `$C000` while bank 0 is in the
window and bank 7 is where your code lives, and the breakpoint sits in bank 0 for ever. Say
which bank and it lands where the code is, paged in or not:

```jsonc
{"name": "set_breakpoint", "arguments": {"address": "07:0000"}}   // bank 7, offset 0
{"name": "set_breakpoint", "arguments": {"address": "main_loop"}} // its bank, from the label
```

A label loaded from a `BB:OOOO name` line carries its bank, so the name alone is enough. The
reply says which bank was used and whether it is paged in right now (`bank_paged_in`) - a
`false` there is normal for a banked build, not a warning. A bank the machine does not have,
or an offset past `$3FFF`, is an error rather than a cell somewhere else; `FF:8000` is the
symbol-file spelling of "no bank" and arms the plain address `$8000`.

`scope` picks the other kind the core keeps: `"address"` puts the breakpoint on the CPU
address instead, where it fires whatever is paged in. Use it for a fixed entry point that
several banks are mapped through; `"cell"` (the default) for a routine.

## Vision

| Tool | Arguments | What it does |
|---|---|---|
| `screenshot` | `path`, `border`, `scale`, `blend` | The last completed frame as a PNG - the ground truth of what is on screen. `border: false` crops to the 256×192 paper area; `scale` enlarges by pixel doubling. Without `path` it writes into a temp directory and returns the path. |
| `record_video` | `frames`, `path`, `every_nth`, `skip_until`, `border`, `scale`, `audio`, `blend` | Record while the machine runs. See below. |
| `screen_text` | `page` | The screen decoded to 32×24 text by matching each cell against the ROM font. Cheap way to read a listing or a menu; graphics come out as `?`. |
| `screen_attrs` | `page` | The 32×24 attribute grid as hex - ink, paper, bright and flash per cell. |
| `screen_digest` | `frames`, `scope`, `banks`, `sync`, `skip_until`, `max_instructions` | A 12-hex-digit hash of screen *memory*, one per frame. The cheap way to prove a change did not alter the data. |
| `frame_digest` | `frames`, `border`, `lines`, `sync`, `skip_until`, `max_instructions`, `blend` | A 12-hex-digit hash of the frame *as drawn*, one per frame, and with `lines` the ranges of scanlines that moved since the previous one. The only one of the two that can see a raster timing fault. |

### `blend`: a picture that flickers on purpose

A gigascreen alternates two pictures every interrupt, and the colour it is after is in
neither of them - it exists only because the eye and the phosphor average the two. The same
goes for any flickering multicolour. A screenshot catches one of the alternating frames,
which is the one thing the effect was never meant to show, and an agent looking at it sees
artefacts rather than the effect.

`blend` averages the last N completed frames instead:

```jsonc
screenshot {"blend": 2, "border": false, "path": "/tmp/mix.png"}
→ {"width": 256, "height": 192,
   "blend": {"frames": 2, "frames_used": 2, "distinct_frames": 2, "gamma": 2.2}}
```

2 is a gigascreen, 3 a three-frame effect. `video_config {"blend": 2}` makes it the default
for every later `screenshot`, `frame_digest` and `record_video` so it need not be repeated.

Three things are worth knowing about the result.

**It is averaged in linear light, not on the bytes.** Black alternating with bright white is
`#B6B6B6`, not `#808080`: encoded sRGB values are not proportional to light, and averaging
them directly gives a mix that is visibly too dark. `gamma` (default 2.2) is the transfer
used to get into linear light and back; `gamma: 2.4` gives `#BCBCBC`, the value the reference
on gigascreen colour arrives at, and `gamma: 1.0` turns the correction off and averages the
bytes.

**`distinct_frames` is the honest part of the answer.** An effect that holds each picture for
two interrupts blends two identical frames, and the result looks unmixed for a reason that
has nothing to do with the blend. When `distinct_frames` is below `frames_used`, that is what
happened - blend more frames, or check the effect's quantum with `frame_cost`.

**Nothing about the machine changes.** The frames come from a history of completed frames the
server keeps as it runs; blending reads it and composes a copy. Two blended screenshots of
the same moment are byte-identical, the frames themselves are never blended in place, and a
raw screenshot taken afterwards is the same raw screenshot it would have been. The history is
dropped on a reset, a `load_file` and a geometry change, because frames from either side of
one of those belong to different pictures.

`frame_digest {"blend": 2}` hashes exactly that averaged picture, which is how to ask "is the
*mixed* picture stable?" - the raw digest of a gigascreen alternates between two values
forever and says nothing about whether the effect is right.

### `screen_digest`: proving the *data* did not change

`screen_attrs` costs 768 bytes of text per frame, which is unusable for checking forty frames.

### `page`: which screen is on air

A 128K or a Pentagon has two screens, in RAM banks 5 and 7, and bit 3 of `$7FFD` says which one
the ULA is drawing. Only one of them is on the television; the other is where the next frame is
being built.

`screen_text` and `screen_attrs` read the one that is **on air**, and say which that was:

```json
{"attributes": "28 28 28 ...", "page": 7, "page_on_air": 7}
```

This matters more than it sounds. The CPU's view is no guide at all: bank 5 is mapped at `$4000`
whatever the ULA is doing, so reading `$5800` describes bank 5 even while bank 7 is the picture.
For double-buffered and gigascreen code that is the screen nobody is looking at, and it is the
answer these two tools used to give.

Pass `page` to read a given bank anyway - `{"page": 5}` while 7 is on air is exactly how you
compare the two halves of a gigascreen, or check what the next frame is being built out of.

On a 48K there is one screen and `page` is always 5; the read wraps onto it, which is what the
ULA itself does.

`beam_position` reports the same number as `screen_page`, so a raster stop can say which screen
the beam was on without a second call.
`screen_digest` runs the machine and hashes screen memory instead:

```jsonc
screen_digest {"frames": 40}
→ {"digests": [{"frame": 0, "digest": "36c29afe0aab"}, ...]}
```

Take the list before a change and after it and compare. Identical lists mean identical output,
byte for byte - there is no tolerance and no sampling.

- **It hashes RAM banks, not the visible frame.** By default *both* screen banks, 5 and 7
  (`banks: [5,7]`, just `[5]` on a 48K). Double-buffered code writes one bank per frame and
  leaves the other holding the previous picture, so hashing only the bank that happens to be
  displayed flip-flops between matching and not.
- **`scope`** is `attrs` (768 bytes a bank, the colour, which is what most attribute-based
  effects are) or `screen` (the 6144-byte bitmap as well).
- **`sync`** decides what a frame is: `halt` (default) is the code between two HALTs - the
  effect's own frame even when it overruns the interrupt - `frame` is one interrupt, and an
  address or label name ends the frame when PC reaches it.
- **`skip_until`** runs to an address or label before hashing anything, so a capture is not
  forty frames of the precalculation's black screen.
- The digest is the first 12 digits of the MD5 of those bytes in bank order, so it matches
  what an external script hashing the same bytes with md5 produces.
- **`from` and `to`** hash a stretch of memory instead of the screen - a precalculated table,
  a buffer, a sprite bank, anything the picture does not show. Addresses are CPU addresses and
  take label names like every other address argument, the range is inclusive, and the reply
  says `scope: "memory"` with the bounds and the byte count. `banks` and `scope` describe the
  screen and are refused alongside it, and half a range (`from` without `to`) is an error
  rather than a guess.

```jsonc
// did the optimisation change the table the effect precalculates?
screen_digest {"frames": 1, "from": "sin_table", "to": "$B7FF"}
→ {"scope": "memory", "from_hex": "$B000", "to_hex": "$B7FF", "bytes": 2048,
   "digests": [{"frame": 0, "digest": "0ee0646c1c77"}]}
```

### `frame_digest`: proving the *picture* did not change

`screen_digest` hashes memory. The picture is a function of that memory **and** of when the
bank is switched relative to the beam, so in multicolour the two questions come apart: raster
code whose timing has drifted paints a different screen out of byte-identical data, and the
memory hash does not move at all. `frame_digest` hashes the frame the ULA actually drew.

Use `screen_digest` to prove an optimisation left the data alone, and `frame_digest` to prove
it left the *screen* alone. For anything raster-timed, only the second one is evidence.

```jsonc
frame_digest {"frames": 16, "border": false, "lines": true}
→ {"width": 256, "height": 192, "unique_digests": 5,
   "digests": [{"frame": 0, "digest": "fa0805a1c33e"},
               {"frame": 3, "digest": "8f22aa04b117",
                "changed_line_count": 26,
                "changed_lines": [[32,32],[36,36],[40,40],[44,44]]}, ...]}
```

- **`lines`** hashes every scanline separately and reports the ranges that differ from the
  previous frame, which turns "some frames look wrong" into a line number. In the example the
  lines that moved are every fourth one - the exact boundaries of a Stellar 4×4 bank toggle,
  which names the culprit without reading a single instruction.
- **`border`** defaults to true. Crop it away when only the paper matters, or border effects
  will make every frame differ for a reason you are not looking for.
- **`sync`** defaults to `frame` - one interrupt - because the picture is made by the ULA and
  its unit is the hardware frame, not wherever the effect calls its own frame boundary. This
  is the opposite default from `screen_digest`, deliberately.
- **`same_as_frame`** appears when a digest was seen before, so a cycle that should repeat can
  be read straight off, and `unique_digests` says how many distinct pictures there were.

If your data hashes stable and the screen still looks wrong, that is not a paradox. It is the
difference between these two tools.

`record_video` defaults to an animated **GIF**, encoded in-process from the emulator's own
palette, so it needs nothing installed and the colours are exact. Give the path an `.mp4` or
`.webm` extension and the frames go to `ffmpeg` instead, with the machine's real sound muxed
in from the same run. Frames are written as they are produced, so length is free: a
two-second GIF is ~47 KB and a thirty-second one ~450 KB. `every_nth: 2` halves the frame
rate and the file size.

**`every_nth: "auto"`** picks that number for you by measuring the effect's quantum the way
`frame_cost` does, then recording one hardware frame in that many. An effect that takes three
interrupts per frame draws each picture three times running, so recording every frame triples
the file for no extra information - 628 KB against 214 KB on a real effect, same animation.
The quantum it found comes back as `every_nth_auto`. **`skip_until`** takes an address or
label and winds there before recording, which keeps the precalculation out of the clip.

A recording keeps the pixel width of its first frame. A program that switches between a
full-dot mode and a hires or text mode (Profi, ATM, Evo, TSConf) would otherwise change the
width under the encoder: frames drawn in the other width are resampled to the first one and
counted in **`frames_resampled`** - exactly when widened (each pixel twice), first pixel of
each pair when narrowed, so start the recording in the hires mode to keep all of it. Frames
of a changed screen geometry cannot be fitted and are counted in **`frames_dropped`**.

## Symbols and source

| Tool | Arguments | What it does |
|---|---|---|
| `load_labels` | **`path`** | Load a symbol table: `05:200E name`, `FF:8000 name`, `name: EQU $8000`, `name = 32768` - the same file `xpeccy -l` takes. Returns a `by_bank` census and a sample. |
| `resolve_symbol` | `name` or `address` | Name → address (with the bank it came from), address → name, or the whole table with no arguments. |
| `load_listing` | **`path`** | Load a sjasmplus `.lst` listing and map addresses to source lines. |
| `source_at` | `address`, `context` | The source line at an address (default PC) with lines of context either side, the current one marked. |
| `step_line` | - | Step until execution reaches a *different source line*, not just the next instruction. Calls into ROM are stepped through. |
| `run_to_line` | **`line`**, `max_instructions` | Run until a given source line is reached. |

### The `BB:OOOO name` format, and where it bites

sjasmplus' `LABELSLIST` writes `bank:offset`, and the offset is **inside the bank**, not a CPU
address. The mapping back is the one Xpeccy's own loader uses, so a label lands where the
emulator would put it:

| Bank | Where it is seen | `05:200E` → |
|---|---|---|
| `5` | `$4000`–`$7FFF` (always paged on a 128K) | `$600E` |
| `2` | `$8000`–`$BFFF` (always paged) | - |
| anything else | `$C000`–`$FFFF`, *when that bank is paged in* | - |
| `FF` | already a CPU address, taken as is | - |

A bare `:8000 name` counts as bank `FF`. `resolve_symbol` reports the source `bank` (`-1` for
a plain CPU address), and `load_labels` returns a `by_bank` count, which is the quickest way
to see what you actually loaded.

Two traps worth knowing before you trust a name:

- **EQU constants are in the same file and are indistinguishable from addresses.** sjasmplus
  writes `07:0001 ROT_SPEED` for `ROT_SPEED EQU 1`, and it arrives as a label at `$C001`.
  Nothing can tell it apart from real code in bank 7 - that is the format, not a bug. In
  practice they cluster in the bank you never generate code into, so `by_bank` makes them
  obvious. Labels resolved from banks other than 2 and 5 deserve a second look.
- **Banks other than 2 and 5 are only correct while that bank is paged in.** `$C000` means
  whichever bank the machine has selected at the time, so a breakpoint set from such a label
  may sit in a different bank than the label meant. Breakpoints themselves live in the memory
  cell and do follow the bank once armed.

Where two labels resolve to the same address, the first one in the file wins the *reverse*
lookup (address → name); every name still resolves forward.

## Coverage

| Tool | Arguments | What it does |
|---|---|---|
| `coverage` | `action`, `from`, `to`, `max_gaps`, `min_gap` | `start`/`stop`/`read`/`reset`. Reports the executed fraction of a range and the largest stretches that never ran, labelled where labels exist. An executed instruction marks all its bytes by its real length, so the bytes a taken `jr` skips stay a gap. |

The unexecuted gaps are the point: after a test run they tell you which branches your test
never took.

## Raster and timing

| Tool | Arguments | What it does |
|---|---|---|
| `beam_position` | - | Where the beam is: dot, line, the zone (`paper`, `border`, `blank`), T-states since the interrupt, T-states per line and per frame, plus `blank_x`/`blank_y`, `interrupt_at` and `screen_page`. |
| `frame_timing` | - | Frame geometry and budget: T per line and per frame, what the last frame actually cost, nanoseconds per dot/line/frame, the paper/border/blanking split, interrupt position and length. |
| `frame_cost` | `frames`, `sync`, `skip_until`, `max_instructions` | What one *effect* frame costs: work T per frame, the HALT wait, min/max/avg, the interrupt quantum, budget, headroom and the resulting frame rate. |
| `profile` | `action`, `ranges`, `from`, `to`, `max_ranges`, `top` | `start` (over named ranges or `"symbols"`), `read`, `stop`, `reset`. T-states, calls and cost per call per range, plus a histogram of the busiest 256-byte pages, and `armed`/`warning` when the numbers are not a measurement. |
| `trace` | `action`, `count`, `size` | `enable`/`disable`/`clear`/`dump` a ring buffer of executed addresses; `dump` disassembles them. |
| `run_to_beam` | **`line`**, `dot`, `max_instructions` | Run until the beam reaches a raster position, wrapping into the next frame when the target is already behind. Steps the picture instead of the program counter. |
| `beam_log` | `action`, `addresses`, `count`, `summary`, `by_position`, `size` | Record where the beam was every time a watched address executed, without stopping, and report the jitter per position across frames. |

Every stop - `run`, `run_frames`, `step`, `step_over`, `step_out`, `run_to_beam`, a breakpoint -
now carries a `beam` block with the line, dot, T-states into the frame and the frame counter,
so raster work does not need a second call to `beam_position` after every single stop.

### Three ways to say where the beam is

The core tracks the beam in three coordinate systems at once, and `beam_position` reports all
of them because each answers a different question:

| Fields | Measured from | Use it for |
|---|---|---|
| `dot`, `line` | top-left of the visible image | matching what a screenshot shows |
| `paper_x`, `paper_y` | top-left of the main screen, `-1` outside it | "which character row am I on" |
| `blank_x`, `blank_y` | the leading edge of the blanking | comparing against the interrupt |

The third one is not spare detail. **The frame interrupt is defined in it** - the core fires
`INT` when `blank_x`/`blank_y` reach a fixed position - and `interrupt_at` reports that position,
so the distance from `blank_x`/`blank_y` to `interrupt_at` is how far the beam is from `INT`.
There is no way to work that out from `dot`/`line` alone, because the blanking is not part of
the visible image those count from.

### `run_to_beam` and `beam_log`: timing that repeats, and timing that does not

A breakpoint answers *where is the beam this once*. Raster code raises a different question:
whether the answer is the same on every frame. These two tools are for that.

`run_to_beam` stops by picture position rather than by address. A target already behind the
beam means the next frame, not an immediate stop, so calling it repeatedly with the same line
walks the same point of the raster frame after frame:

```jsonc
run_to_beam {"line": 100, "dot": 0}
→ {"reason": "beam", "target": {"line": 100, "dot": 0},
   "beam": {"line": 100, "dot": 5, "t_states_frame": 26021, "frame_counter": 382},
   "overshoot_lines": 0, "overshoot_dots": 5}
```

The beam only moves between instructions, so it lands a few dots past the target and says by
how much. Landing on a different *line* is what would matter.

`beam_log` watches addresses and records the beam every time one executes, without stopping:

```jsonc
beam_log {"action": "enable", "addresses": ["mc_isr", "mc_isr.strip"], "size": 200000}
run_frames {"count": 40}
beam_log {"action": "dump", "count": 1}
→ {"summary": [
     {"label": "mc_isr",       "hits": 39,  "hits_per_frame_min": 1,
      "jitter_t": 4,  "jitter_shape": "single"},
     {"label": "mc_isr.strip", "hits": 720, "hits_per_frame_min": 48, "positions_per_frame": 48,
      "jitter_t": 23, "jitter_t_least": 23, "jitter_shape": "uniform"}]}
```

**`jitter_t` is the number to read.** Raster code runs the same address many times per frame -
a multicolour strip loop hits its `OUT` once per band - so the plain minimum and maximum over
every hit merely restate that the loop spans the frame, which is useless. Hits are therefore
numbered within their frame and compared across frames position by position, and `jitter_t` is
the worst of those spreads.

**`jitter_shape`** is the second number worth reading. `uniform` means every position drifts by
the same amount - the whole pass is displaced, frame to frame, by a constant. `varies` means
the spread differs along the pass, which is drift accumulating inside it. Those are different
faults with different causes, and this tells them apart without printing one entry per
position. `by_position: true` adds that full breakdown when you want it; it is off by default
because a 48-strip loop produces 48 entries per address every time you look.

- The ring almost always starts mid-frame, and that frame is missing its early hits. Those
  would fake an enormous spread on every position, so everything before the first frame
  boundary in the buffer is dropped.
- If the number of hits per frame is not constant, positions do not line up between frames
  and the reading is not trustworthy; `hits_per_frame_min` differing from `hits_per_frame_max`
  raises a `warning` saying so rather than quietly reporting a number.
- Watching is by address, which covers both cases worth watching: the entry of a handler, and
  the `OUT` that flips a bank, since in raster code that instruction lives at a fixed address.

In the example above the handler entry jitters by 4 T - instruction-boundary granularity of
interrupt acceptance, unavoidable - while all 48 strip positions jitter by the same 23 T,
which `jitter_shape: "uniform"` states outright. The whole pass is displaced by a constant that
changes from frame to frame, rather than stretching as it goes, so the cause lies before the
pass starts and not inside it.

### `frame_cost`: the unit an effect is actually measured in

`run_frames` counts interrupts. An effect frame is whatever the code does between two waits
for the interrupt, and as soon as a render overruns, that is two interrupts or three - so
counting interrupts answers the wrong question. `frame_cost` measures the work between two
frame boundaries and keeps the time parked in HALT separate:

```jsonc
frame_cost {"frames": 8}
→ frame_list: [{"frame": 1, "t_states": 119478, "idle_t": 23976, "total_t": 143454, "interrupts": 2}, ...]
  t_states_max 119478, interrupts_per_frame 2, budget_t 143360,
  headroom_t 23882, headroom_percent 16.66, fps 24.41
```

- `t_states` is the **work**: everything except sitting in HALT. `idle_t` is the wait.
  Adding them gives the wall clock, which is why `headroom_t` and `idle_t` agree.
- `interrupts_per_frame` is `ceil(t_states_max / t_states_per_frame)` - the quantum.
  **`effect_fps`** is `fps_real / quantum`, so shaving T-states only buys a frame per second
  when it crosses a quantum boundary. Everything else is headroom, which is a fine goal but a
  different one. (`fps` is kept as the old name for the same number. `effect_fps` says which
  frequency it is: `frame_timing`'s `fps_real` is the *machine's*.)
- `sync` is `halt` by default, or an address/label whose PC ends the frame - for code that
  waits on a counter rather than HALT.
- **Measure two frames or more**: the first frame after repositioning shows a transient
  `idle_t`, because the interrupt phase has to settle. The work figure is right from the
  first frame. Measure enough frames to cover every phase, too - an effect that cycles
  through segments has a different cost in each, and the quantum follows the dearest one.

### `frame_cost`'s `alignment_t`: the "am I measuring the precalculation?" reading

`alignment_t` is a field of the `frame_cost` reply, not a tool of its own.
`frame_cost` has to reach a frame boundary before it can time whole frames, so it runs one
partial frame first and reports what that cost as **`alignment_t`**. It is not averaged into
the results and the profiler does not see it.

It is also the cheapest check that you are measuring the right thing. On one real effect:

| starting point | `alignment_t` |
|---|---|
| snapshot taken after the precalculation | 189 318 |
| cold start, straight after `load_file` | 1 986 522 |

An `alignment_t` far bigger than `t_states_max` means the run spent its time in the table
builder, not the effect - position the machine first. `skip_until` does that inline, or take
a snapshot after init once and load it (see
[Snapshot the machine once the precalculation is done](../COOKBOOK.md#snapshot-the-machine-once-the-precalculation-is-done),
the habit worth forming).

Interrupts the CPU actually services are included in the work, because a real machine pays
for them. A model that does not emulate interrupts will read a little lower - on a demo
servicing one IM 2 interrupt per frame, 43 T lower (19 to acknowledge, plus the handler).

### `profile`: where the T-states go, and per call

Ranges are **half-open**, `[start, end)`, so naming the next label as `end` measures exactly
one routine and does not charge that label's first instruction to it - nor count entering the
next routine as a call.

```jsonc
profile {"action": "start", "ranges": [
           {"tag": "isqrt", "start": "isqrt_hl", "end": "compute_polar"}]}
run     {}
profile {"action": "read"}
→ {"tag": "isqrt", "t_states": 788972, "calls": 1536, "t_per_call": 513.65, ...}
```

- **`calls`** is how many times the routine was invoked. **`entries`** is every arrival from
  outside, which is a bigger number for anything that calls something else: when a callee
  returns, control arrives in the range again. Counting those as calls would double the count
  for every caller and halve its cost per call - on a real routine, 3072 entries for 1536
  invocations. `calls` leaves them out by checking whether the instruction that handed control
  over was a `ret`; `entries` keeps the raw count. A return faked with `pop hl` / `jp (hl)` is
  indistinguishable from a jump and will read as a call.
- **`t_per_call`** is the number to optimise against - "this routine costs 513 T per call" is
  actionable in a way that "it costs 13 % of the frame" is not. The T-states are the range's
  own: time inside a nested callee belongs to the callee, not the caller.
- **`t_states_inclusive`** and **`t_per_call_inclusive`** are the same numbers with the
  callees put back in - what the routine costs you altogether, which is the figure to compare
  against the frame budget when you are deciding what to cut. For a leaf the two are equal by
  definition; for a caller they separate: a routine that runs 528 T of its own and spends 552
  in the routine it calls reports 44 T per call self and 90 inclusive. An invocation is
  counted as running until SP climbs back above where it stood when the routine started, so
  how it returned does not matter. Recursion charges the outermost invocation only, and a
  range entered by a jump - a main loop - gets one too, which is why a main loop's inclusive
  time is the whole run.
- **`ranges: "symbols"`** splits by the loaded symbol table instead: one region per non-local
  label, up to the next one. This is the first thing to run on code you do not know. It
  reports only regions that executed, dearest first, capped at `max_ranges` (default 24), and
  `from`/`to` bound the address window (default `$4000`–`$FFFF`).
- **`unattributed`** is the time that fell in no range at all - with `symbols` it should be
  zero, and anywhere else it is the part of the frame you have not accounted for yet.
- **`interrupt_acceptance`** is the CPU taking interrupts: 11-19 T each of bus cycles that
  run no instruction at any address. They are in `t_states`, so ranges, `unattributed` and
  this add up to the total, but in no range's own time, and `instructions` and `hot_pages`
  do not count them.

Two things to know about `symbols` mode, both inherent to using a symbol table as a map:
labels that are really data or `EQU` constants become regions and will collect any code that
happens to sit after them, and local labels (`name.loop`) are skipped on purpose, since they
would cut routines into fragments - as would an SMC label declared `name equ $+1`, which sits
*inside* an instruction. A region flagged **`start_never_executed`** is the first case: code
ran inside it but never at its first address, so the label is probably a table and the time
really belongs to whatever precedes it. Narrow the window with `from`/`to` when that happens.

### When the numbers are not a measurement

A profiler that was never started reports zeros, and zeros read exactly like "this code never
ran". So `read` says which it is:

- **`armed`** - has a start ever succeeded.
- **`running`** - is it counting now.
- **`warning`** - present whenever the numbers are not what they look like, with
  **`last_start_failed`** carrying the error when the last `start` was rejected.

That last case is the expensive one: a `start` whose ranges name a label that a refactor
inlined away fails, and without the warning the next `read` looks like a measurement showing
your code never executed. A failed `start` is atomic - it leaves the previous profile exactly
as it was rather than half-replacing its ranges - and is remembered until a start succeeds.

On a Pentagon these report 71680 T per frame and 224 T per line - the real numbers, because
they come from the emulator's own timing rather than a table in this repo.

### 50 Hz is not the frame rate

**T-states are counted exactly**, straight from the cpu's own tick counter, so every
`t_states` figure a tool reports - per range, per run, per frame - is the integer a real
machine would agree with, waits included. The nanosecond fields exist for convenience and
are computed at the fork's precise `nsPerTick` (kept as a double, and derived from the dot
period so a tick is exactly two dots; the int truncation upstream used was itself a bug
that slid the frame interrupt off the dot grid).

| Field | Pentagon | Meaning |
|---|---|---|
| `t_states_per_frame` | `71680` | exact, from the core's own dot timing |
| `fps_real` | `48.828125` | `1e9 / ns_per_frame`: frames per second at the clock the CPU is actually running, so a turbo raises it |
| `ns_per_t_state` | `285.714…` | per-tick period, precise, turbo included |
| `ns_per_t_state_real` | `285.714…` | the same value, kept for compatibility |
| `ns_per_frame` | `20480000` | the frame's span in ns, exact to a fraction of a dot |
| `ns_per_frame_real` | `20480000` | real time a frame takes - also identical |

There used to be an `fps` beside `fps_real`, carrying the nominal 50 that `zx_init()` set for
every ZX machine whatever its geometry. Upstream deleted the field, and these
tools no longer report it. Nothing else changed: `fps_real` was always the number to budget
against, and it still is.

### The raster comes from the geometry, not from the model

Each machine definition pins its own geometry (`zx48.conf` says `geometry = ULA.48`), so
naming a model times the frame the way the real machine does. The footgun that remains is
the *separate* `geometry` argument: `machine_config {"model": "ZX48", "geometry": "default"}`
(or a patched definition) gives you a ZX48 timed at Pentagon's 71680 T instead of its own
69888 - every budget, headroom and fps out by 2.6 %, and nothing about the numbers looks
wrong.

`machine_config`, `machine_state`, `frame_timing` and `frame_cost` say so, in a `warning`
field naming both figures. The explanation comes in full the first time the server says a
given warning; after that the `warning` is one line with the same figures and the error
(`geometry 'ULA.Pentagon' gives 71680 T per frame, a ZX48 runs 69888: timings +2.6% out`),
so a reader who never saw the first one - a subagent on the same server, a conversation
whose start was summarized - still gets the facts. The machine is left exactly as configured - the fix is yours to
make, by naming a geometry whose height matches:

```jsonc
machine_config {"model": "ZX48", "geometry": "ULA.48"}   // 448x312 → 69888 T
```

The warning appears only for machines whose raster is not in doubt - `ZX48` and `Scorpion`
at 69888, `Plus2A`/`Plus3` at 70908, `Pentagon`/`Pentagon1024SL` at 71680. For anything
else nothing is claimed and nothing is said.

So an effect costing `T` T-states runs at `fps_real / ceil(T / t_states_per_frame)` - 48.8 Hz
if it fits in one frame, 24.4 Hz if it takes two. `profile` returns `t_states_per_frame` and
`fps_real` at the top level next to the per-range numbers, so the whole sum is in one answer.

## Sound

| Tool | Arguments | What it does |
|---|---|---|
| `audio_capture` | `frames`, `rate`, `path`, `watch_ay`, `max_writes` | Run N frames while sampling the mixed output. Reports whether there is any sound, peak and RMS, a zero-crossing pitch estimate, beeper toggles, and which source dominates. `path` writes a `.wav`; `watch_ay` logs register writes. |
| `ay_state` | `chip` | The AY/YM decoded: raw registers as a hex string, and per channel the tone period as a frequency, volume, mixer bits and envelope flag; plus noise, envelope and an `audible` summary. |
| `sound_state` | - | Every source at once: beeper, both AY chips of a Turbo Sound pair, General Sound, SAA1099, tape, and the current mixed output level. |
| `ay_writes` | `offset`, `limit` | Page through the AY register writes recorded by the last `audio_capture` with `watch_ay` on - each with the frame, T-state and the PC that made it. |

## Keyboard

| Tool | Arguments | What it does |
|---|---|---|
| `type_text` | **`text`**, `frames` | Type a string; shifts are added automatically for upper case and symbols, `\n` presses Enter. |
| `press_key` | **`key`**, `frames`, `release_frames` | Hold one key and release it. Names: `a`–`z`, `0`–`9`, `enter`, `space`, `caps`, `symbol`, `delete`, `break`, `left`/`right`/`up`/`down`, `edit`, `capslock`, `graph`, `extend`. |
| `release_keys` | - | Release everything, in case a key got stuck. |

The 48K ROM starts in keyword-entry mode, so `PRINT` is one keypress (`p`), not five.

## Files and media

| Tool | Arguments | What it does |
|---|---|---|
| `load_file` | **`path`**, `drive` | Load by extension: `.sna`/`.z80` snapshots, `.spg` (TSConf/Profi), `.tap`/`.tzx`/`.wav` tapes, `.trd`/`.scl`/`.udi`/`.fdi`/`.td0`/`.dsk`/`.ima` disks, `.bin` raw. Snapshots start running immediately. `.bin` is the legacy BK format - a little-endian address/length header, payload written to RAM at the even addresses - and stays on whatever machine is running. Every format's declared structure is validated before the core sees it - `.spg`, `.td0`, `.dsk`, `.fdi`, `.udi`, `.trd`, `.scl`, `.tap`, `.tzx`, `.sna`, `.z80`, `.ima` - a truncated or miscounted file is refused, never half-loaded, and the machine is not switched for a file that cannot load (the core's own parsers would hang on, overflow, or fabricate them). A `.tzx` is also played through in the validator the way the core will expand it, loops and calls included, and refused past 2^26 pulses (the core keeps every pulse, 8 bytes each). An extended `.dsk` sector shorter than its size code loads with the rest of the sector zeroed and its CRC rewritten - copy-protected disks have them - instead of carrying the previous sector's bytes. A refused load leaves the session as it was: memory, registers, machine and mounted disks. `.ima` needs HD-density tracks: the adapter enables them on the drive before loading, and any other image puts the drive back to DD. When the format names a machine the current one cannot run (an `.spg` wants TSConf, a `.trd` wants a Beta Disk, a `.dsk` wants a +3), the machine switches first and the reply's `machine`/`model` say where it landed - the way upstream's `filemachine` picks. |
| `save_snapshot` | **`path`** | Save the current state as `.sna`. A `.sna` has no room for the disk controller or the angle of the disk: saved while a command runs, the reply carries a `warning` - use `checkpoint` for that. |
| `tape` | `action`, `block` | `state`, `play`, `stop`, `rewind`, `next`, `eject`, `blocks`. Loading a tape does not press play. |
| `disk` | `action`, `drive`, `path`, `angle_deg`, `rpm_percent`, `spinup_ms`, `write_protect` | `state`, `insert`, `eject`, `save`, `setup`. Drive 0 is A. `state` also reports the controller the machine has, and each drive's `track_bytes` (6250 for DD, 17700 once an `.ima` made it HD), the angle of the disk, and the drive setup when it differs from a plain drive. `eject` returns the drive to DD. `setup` turns the disk (`angle_deg`, where the next command starts), makes the drive faster or slower (`rpm_percent`, -10..10), delays a command while the motor comes up to speed (`spinup_ms`, 0..1000), and sets `write_protect`. `save` warns when `disk_track` faults are on the disk: they are bytes on its tracks and go into the file. |
| `disk_catalog` | `drive` | The TR-DOS catalogue of the inserted disk: names, extensions, start address, length, track and sector. |
| `sdcard` | `action`, `path` | `state`, `mount`, `eject`, `lock`, `unlock`. The raw sector-addressed card image the TSConf/ZX-Evo, ATM or Profi SD controller sees; mount it before boot. Images larger than 1 GiB are refused - the core's capacity math is 32-bit and cannot survive them; exactly 1 GiB is fine. Mounting or ejecting resets the controller, so a swapped card powers up uninitialized like a real one - including its lock: a card mounted after a locked one starts unlocked. |

Inserting an image does not boot it: `reset {"mode": "dos"}` puts the machine in TR-DOS, and
from the `A>` prompt the disk is readable. `disk_catalog` reads the image directly and works
without any of that.

---

## Disk controller (TR-DOS)

The Beta Disk interface's WD1793 (KR1818VG93 on the clones), for debugging and tuning code
that drives it - a track loader, a fast reader, anything that calls the TR-DOS ROM from RAM.
The tools answer on a machine with a Beta Disk; on a +3 (uPD765) or a machine with no disk
they say so.

| Tool | Arguments | What it does |
|---|---|---|
| `fdc_state` | `trdos` | The controller now, read without side effects: registers, status decoded for the command class, INTRQ/DRQ, the running command and its phase (`find_id`, `find_data`, `transfer`...), bytes left, the lost-data bit; the selected drive (head cylinder against the track register, side, motor, protect); the disk under the head - position, angle, what field and sector the head is over, and `next_ids`: in how many T-states each sector's ID comes round. `trdos: true` adds TR-DOS's system variables. |
| `fdc_log` | `action`, `count`, `since` | The journal, always on, last 1024 rows. `read` (default): a row per command and its sectors, rows for `#FF` writes, rows for commands the controller ignored. `summary`: totals since `clear`. `clear`. |
| `fdc_break` | `on`, `command`, `id`, `clear` | Stop `run`, `run_frames`, `run_to_beam`, `step_over`, `step_out` on a controller event. `on`: `command`, `end`, `error`, `lost_data`, `crc_error`, `not_found`, `index`, `interrupt_in_transfer`. `command`: only that command (`"read_sector"` or `"$80"`). `id`: `{cylinder, side, sector}` stops when that ID passes the head. Armed until `clear` or `clear_breakpoints`. |
| `fdc_sweep` | **`stop_pc`**, `angles`, `max_frames`, `verify` | The same stretch from the current state at N evenly spaced disk angles (default 8): time, frames, sectors read and broken, revolutions, and with `verify` whether memory matches the disk. Puts the machine back afterwards. |
| `disk_track` | `action`, `drive`, `cylinder`, `side`, `sector`, `kind` | The physical layout of one side of a cylinder: sectors in the order they pass the head, byte offsets of ID and data, size, mark, both CRCs, the order as a string, where the head is, T per byte and per revolution. `fault` puts a fault on a sector, `clear_faults` takes them all off. |
| `disk_sector` | **`action`**, `drive`, `cylinder`, `side`, `sector`, `count`, `hex`, `address`, `length`, `file` | The image, bypassing the controller: `read` (hex, up to 16 sectors), `write` (CRC recomputed), `verify` memory against the disk in TR-DOS order from a sector or from a catalogue file. |
| `checkpoint` | **`action`**, `name` | `save`, `load`, `list`, `drop`: the whole machine in this server's memory, the controller mid-command and the angle of the disk included. |

### What the journal knows, and how

The core keeps every fact the controller has - registers, the step of the command it runs,
the head's place in the track to the byte - but no history. The server looks at it around
every instruction and writes down what changed. When the controller stands still and the
TR-DOS ROM is paged out nothing can change (its ports are not even decoded then), and the
check costs nothing worth measuring: a run with no disk activity was as fast as 1.4.1 within
the noise, and one that reads the disk continuously ran about 3.5% slower.

A command row:

```jsonc
{"n": 16, "t": 11885821, "frame": 165, "frame_t": 62243,
 "pc_hex": "$2A53", "pc_name": "trdos.out_c",          // the OUT that wrote the command
 "cmd": "read_sector", "value_hex": "$80", "drive": 0, "side": 0,
 "track": 0, "sector": 14,                              // the registers as the program set them
 "head": 0,                                             // the cylinder the head is really on
 "disk_pos": 3875,                                      // where on the track (bytes) it started
 "duration_t": 87039, "status_hex": "$04", "status": ["lost_data"], "end": "done",
 "sectors": [{"id": [0, 0, 14, 1], "found_t": 52409, "data_t": 58012, "bytes": 256,
              "lost": 10, "first_lost": 205, "interrupts": 1, "isr_max_t": 1120}]}
```

- `found_t` is when the ID came round, counted from the command; `data_t` the first data byte.
  The difference between the two is the gap the format leaves; `found_t` is the rotational
  wait a loader can win back by reading sectors in the order they arrive.
- **A lost byte** is one the controller replaced while the last was still waiting in the data
  register: DRQ was up before the instruction, the instruction did not touch `#7F`, and a new
  byte arrived. `first_lost` is the offset of the byte that was overwritten - the first byte of
  the sector that is wrong in memory. `bytes` minus `lost` is what the CPU took, and the
  summary's `data_port + lost = delivered` holds to the byte.
- `interrupts` and `isr_max_t`: interrupts accepted while this sector was being transferred,
  and how long the longest handler ran (from acceptance until the interrupted PC came back with
  the same SP). Set against `byte_window_t` in the summary - 112 T at 3.5 MHz, the time a byte
  waits - a handler of 1120 T costs ten bytes, every time it lands on a transfer.
- `end`: `done`, `forced` (a `$D0`-`$DF` stopped it), `reset`, `checkpoint`. A restore the
  reset itself starts has no `pc_hex`.
- `ignored` rows: a command written while the controller was busy (`why: "busy"`) or held in
  reset (`"reset"`) - the WD1793 drops those silently.
- `turbo: true` on a row means the `fdcturbo` setting was on; the controller then moves many
  bytes per instruction and the counts are approximate.

`fdc_log {"action": "summary"}`:

```jsonc
{"commands": 17, "by_kind": {"read_sector": 16, "restore": 1},
 "sectors": 16, "sectors_ok": 10, "sectors_lost_data": 6, "crc_errors": 0, "not_found": 0,
 "revolutions": 1,
 "t": {"busy": 1390682, "find_id": 836538, "seek": 67, "transfer": 458932},
 "bytes": {"delivered": 4096, "lost": 59, "data_port": 4037, "status_polls": 41847},
 "interrupts": {"during_commands": 19, "during_transfer": 6, "sectors_hit": 6,
                "longest_handler_t": 1120, "byte_window_t": 112},
 "throughput": {"span_t": 1395300, "bytes_ok": 2560, "bytes_per_s": 6422,
                "ceiling_bytes_per_s": 20480}}
```

`ceiling_bytes_per_s` is the data on the track under the head divided by one revolution - what
a reader that never waits and never loses a sector would get from that side.

### The angle of the disk, and why `fdc_sweep` exists

In the core the disk turns only while the controller runs a command (and for the 15
revolutions the motor coasts after it); between commands it stands still. Its angle is a byte
position on the track, `fdc_state`'s `disk.position`, and it decides when each sector arrives
relative to the interrupts. An emulator starts every run at the same angle, so a loader that
meets its sectors badly at one angle and well at another always shows the same one; on the
real machine the angle is whatever it is. `disk {"action": "setup", "angle_deg": 90}` sets it
by hand, and `fdc_sweep` runs the same stretch at every angle:

```jsonc
fdc_sweep {"stop_pc": "loaded", "angles": 8}
→ {"summary": {"runs": 8, "reached": 8, "failed": 0, "t_min": 666587, "t_max": 1279113,
               "t_mean": 972865, "best_angle_deg": 45.0, "worst_angle_deg": 90.0,
               "spread_frames": 8.5},
   "runs": [{"angle_deg": 0.0, "reached": true, "t": 754079, "frames": 11, "commands": 8,
             "sectors_ok": 5, "sectors_lost_data": 3, "revolutions": 1}, ...]}
```

`verify: {"address": "$6000", "file": "PART2.C"}` (or a `cylinder`/`side`/`sector` and a
`length`) adds a comparison of memory with the disk after each run: `"verify": "ok"`, or the
first byte that differs.

The machine is put back where it was; profile, trace, coverage, `beam_log`, the journal and
the `fdc_break` stops are untouched. Breakpoints still stop a run, and the run says so in
`stopped`.

### Faults and a different drive

`disk_track {"action": "fault", "cylinder": 0, "side": 0, "sector": 5, "kind": ...}`:
`crc_data` (the data CRC is wrong: the read ends with CRC error), `crc_id` (the ID's CRC),
`no_id` (the ID mark is gone: five revolutions, then not found), `no_data` (the ID is there,
the data mark is not: not found), `deleted` (an `$F8` mark with a correct CRC: the status
carries the deleted-mark bit). Each track is kept as it was before its first fault and
`clear_faults` puts it back byte for byte. A fault is a change to the disk's bytes: `disk save`
writes it into the file and says so.

`disk setup`'s `rpm_percent` scales the time of one byte, which on a drive is the same thing
as its speed: data and rotation both. `spinup_ms` holds the command when the motor comes on.
There is no byte-level jitter: the controller moves the disk a whole byte at a time, and when
sectors arrive against the interrupts - the thing a loader can get wrong - is what the angle
and the speed already cover.

### `checkpoint` against `.sna`

A `.sna` holds memory and the CPU; it has no room for a command in progress, a head position
or the angle of the disk, so a snapshot taken during a load comes back with the controller
reset and the loader waiting for an interrupt that never arrives. `checkpoint` takes the
whole machine as the core sees it, and a load resumes the command to the T-state. The price
is that it lives in this server process only: the state holds pointers into the core's own
tables, which the next run of the server has at other addresses, so it cannot be a file. The
contents of the disk are not in it either; loading on a drive that now holds another image
says so, and a checkpoint taken on another machine (model, definition or memory) is refused.

### The TR-DOS ROM by name

`disassemble` names these while the TR-DOS ROM is paged in (the address decides nothing: the
48K ROM's `#3FE5` is the font), and every address argument takes the name. Each is checked
against the bytes of the ROM before it is given; a TR-DOS that has other code there gets no
name rather than a wrong one. Checked against 5.03 and 5.04T.

| Name | Address | What is there |
|---|---|---|
| `trdos.read_sectors` | `#1E3D` | body of `#3D13` function 5: read B sectors from D track, E sector to HL |
| `trdos.read_sector` | `#2090` | `di`, `ld a,#80`, `out (#1F),a`: one sector by table entry, with retries |
| `trdos.read_sector_cmd` | `#2093` | `out (#1F),a` with the command in A, then the bytes through `#3FE5` |
| `trdos.out_c` | `#2A53` | `out (c),a : ret` - a port write from RAM |
| `trdos.entry` | `#3D00` | `RANDOMIZE USR 15616` |
| `trdos.command` | `#3D03` | `RANDOMIZE USR 15619`: one command from BASIC |
| `trdos.call` | `#3D13` | run TR-DOS function C |
| `trdos.init` | `#3D21` | prepare TR-DOS and its variables |
| `trdos.ret` | `#3D2F` | `nop : ret` - jump here from RAM with a ROM address on the stack |
| `trdos.write_bytes` | `#3FCA` | wait for DRQ/INTRQ on `#FF`, `outi` to `#7F` until INTRQ |
| `trdos.read_bytes` | `#3FE5` | wait for DRQ/INTRQ on `#FF`, `ini` from `#7F` until INTRQ |

`fdc_state {"trdos": true}` decodes the system variables two independent references agree
on: `drive_modes` (`#5CC8`), `bad_sectors` (`#5CD6`), `file_descriptor` (`#5CDD`), `sector` and
`track` (`#5CF4`/`#5CF5`), `temp_drive`, `step_rates`, `last_command`, `sector_arg`, `buffer`,
`mode`, `error` and `error_hi` (`#5D0F`/`#5D10`), `sys_copy` (`#5D16`, the last `#FF` value),
`default_drive` (`#5D19`).
