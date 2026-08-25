# Cookbook

Recipes for the things this server was built to do. Each one is a sequence of tool calls;
the arguments are shown as JSON so they translate directly.

Written to be read by an agent before it starts: the tool descriptions say what each call
does, and this says which calls to make, in what order, and which of them answer questions
that sound the same and are not.

## Start here

Two things are worth doing before anything else, because getting them wrong makes every
later number meaningless.

**Pick the machine.** `machine_config {"model": "Pentagon"}` - changing the model resets, so
do it first. `list_models` says what is available. The frame is 71680 T on a Pentagon and
69888 on a ZX48K, and every timing answer depends on which.

**Get past the precalculation and snapshot.** Effects build tables at startup, half a second
to fourteen, and anything measured before that finishes measures the table builder. Run once
to the main loop, save, and work from the snapshot for the rest of the session. The recipe is
[below](#snapshot-the-machine-once-the-precalculation-is-done).

After that, pick by question:

| Question | Tool |
|---|---|
| What is on screen? | `screenshot`, `screen_text`, `screen_attrs` |
| Which of the two screens is on air? | `beam_position` → `screen_page`, or `page_on_air` on either screen tool |
| Did my change alter the data? | `screen_digest` |
| Did my change alter the picture? | `frame_digest` |
| Does a frame fit, and what is the headroom? | `frame_cost` |
| Where do the T-states go? | `profile`, `disassemble` with `t_states` |
| What did the code actually do? | `set_breakpoint`, `step`, `trace`, `read_memory` |
| When in the frame does this run, and is it stable? | `beam_log` |
| Put the machine at a raster position | `run_to_beam` |
| The picture flickers on purpose (gigascreen) | `screenshot` with `blend`, `video_config` |
| What can I configure, and what is it set to? | `settings` |

## Pin the machine a project needs

Several things that change how a program behaves are settings rather than arguments: which
RAM banks the ULA steals cycles from, whether contention is on at all, which ROM page a reset
lands in, the AY clock. `settings` with no arguments reports all of them, what they are set
to, and where that value came from.

```jsonc
settings {}                                              // what am I actually running?
settings {"action": "set", "name": "timing.contPattern", "value": "b"}
settings {"action": "set", "name": "video.blend", "value": 2, "save": true}
```

`save: true` writes it into `xspeccy-mcp.conf`. Keep that file in the project's repository and
every session starts from the same machine, which is the difference between a measurement that
can be repeated next week and one that cannot.

Two things the reply tells you and it is worth reading. `scope` says whether the change is
live, needs a reset, or is only read at startup. And anything under `timing.` invalidates every
`frame_cost`, `profile` and `beam_log` taken before it, because they were measured on a
different machine.

What is there, by prefix:

| Prefix | What it covers |
|---|---|
| `timing.` | contention pattern and whether contention is on at all, early timing, border step, the screen-port wait, a turbo multiplier on the CPU clock |
| `boot.` | which ROM page a reset lands in |
| `disk.` | floppy turbo, which decides whether a load costs emulated time |
| `video.` | blend and its gamma, how many frames are kept, greyscale, border size, ULAplus, ATM2 palette decoding |
| `sound.` | AY or YM, stereo mode, and the chip clock, which every note scales with |

Values are refused outside their range rather than quietly corrected, and `reset` puts one
back. `settings {}` is also the answer to "what machine am I on" when a measurement disagrees
with what you expected: several of these used to be fixed in the source, where nothing could
see them.

## Write a routine and watch it run

```jsonc
assemble    {"address": "$8000", "lines": ["di", "ld hl,$4000", "ld (hl),255", "ret"]}
set_register{"name": "PC", "value": "$8000"}
step        {"count": 3}
get_registers
read_memory {"address": "$4000", "length": 1}
```

`assemble` writes the bytes as it goes, so there is no separate load step. Numbers may be
`$`, `#`, `0x` or decimal.

## Load a build and stop where it matters

```jsonc
load_file      {"path": "build/project.sna"}
load_labels    {"path": "build/project.labels"}
set_breakpoint {"address": "draw_loop"}          // label names work as addresses
run            {"max_instructions": 5000000}
disassemble    {"count": 8}
```

Once labels are loaded every address argument accepts a name, disassembly names the lines and
jump targets, and `assemble` takes labels as operands. `resolve_symbol` turns a name into an
address if you need it explicitly - and tells you which **bank** it came from, which is worth
a look: sjasmplus writes `bank:offset`, so a label from bank 5 lands at `$4000+offset` and one
from bank 2 at `$8000+offset`, while EQU constants land in whatever bank their *value* looks
like.

A name that is not in the table is refused (`no such label 'draw_lop'`) rather than quietly
treated as address 0.

## Debug in terms of your source, not addresses

```jsonc
load_listing {"path": "build/project.lst"}
run_to_line  {"line": 214}
source_at    {"context": 5}
step_line
```

`step_line` advances one *source line* rather than one instruction, stepping through calls
into ROM on the way.

## Check a raster effect

The question is usually "where is the beam when my code gets there, and does the effect fit
in the frame?".

```jsonc
set_breakpoint {"address": "raster_start"}
run            {}                                // the reply already carries `beam`
frame_timing                                     // 71680 T per frame on a Pentagon, 224 per line
clear_breakpoints
profile        {"action": "start", "ranges": [
                  {"tag": "multicolor", "start": "raster_start", "end": "raster_end"},
                  {"tag": "music",      "start": "$9000",        "end": "$9400"}]}
run_frames     {"count": 50}
profile        {"action": "read"}
```

Every stop reports the beam, so `beam_position` is only needed when nothing has stopped.
To put the machine at a chosen point of the frame instead, use `run_to_beam {"line": 100}`;
a target already behind the beam means the next frame, so calling it again walks the same
point frame after frame.

The profile reports T-states per range *per frame*, and `percent_of_frame` next to it, which
is the number that decides whether an effect fits. Both bounds of a range are required and
both accept label names; a name that does not resolve fails the call instead of leaving the
range at `0..0`, where it would silently count nothing and read as "this code never ran".

## Snapshot the machine once the precalculation is done

**Do this first, before any measuring or recording.** Effects build tables at startup -
half a second to fourteen - and every measurement you take before that finishes measures the
table builder instead of the effect. Guessing the warm-up in frames is how recordings come out
half black and profiles come out meaningless.

Run to the main loop once, save, and work from the snapshot ever after:

```jsonc
load_file      {"path": "build/project.sna"}
load_labels    {"path": "build/project.labels"}
run            {"max_instructions": 80000000, "stop_pc": "main_loop"}
→ {"reason": "pc", "t_states": 1797204, "frames": 25}    // that is the precalculation, measured
save_snapshot  {"path": "/tmp/after_init.sna"}
```

Every later session starts from there and is instant:

```jsonc
load_file  {"path": "/tmp/after_init.sna"}
frame_cost {"frames": 12}
record_video {"frames": 150, "every_nth": "auto", "path": "/tmp/effect.gif"}
```

`run` reports what the precalculation cost in `t_states`, so this also answers "how long is the
startup?" without wrapping anything in a profiler.

Two ways to check you are past init:

- **`alignment_t`** in `frame_cost`. From a snapshot taken after init it is about one frame;
  from a cold start it is the whole precalculation - 189 318 against 1 986 522 on one real
  effect. An `alignment_t` far larger than `t_states_max` means you are measuring the table
  builder.
- **`screen_digest`** on the first frames: identical digests mean nothing is moving yet.

If you would rather not keep a snapshot file, `skip_until` does the same thing inline -
`frame_cost {"frames": 8, "skip_until": "main_loop"}`, and likewise on `record_video` and
`screen_digest`. The snapshot is still better when you are going to measure more than once,
because it costs nothing to reload.

## Prove an optimisation did not change the data

The one check that makes optimising safe: hash the screen memory before the change and after
it. For code whose timing matters, read the next recipe as well - this one proves the data is
unchanged, which is not the same claim as the picture being unchanged.

```jsonc
// before                                   // after rebuilding
load_file     {"path": "build/project.sna"}  load_file     {"path": "build/project.sna"}
run           {"max_instructions": 50000000} run           {"max_instructions": 50000000}
screen_digest {"frames": 40}                 screen_digest {"frames": 40}
```

Compare the two lists. Identical digests mean the output is identical byte for byte - not
similar, identical. A difference at frame 17 tells you which frame to look at with
`screenshot`.

Set a breakpoint on the main loop and run to it first, so both captures start from the same
point in the effect; otherwise you are comparing different moments and everything differs.

By default it hashes the attributes of **both** screen banks. Keep it that way for
double-buffered code: only one bank is written per frame, so hashing the displayed one alone
reports differences that come and go. `scope: "screen"` adds the bitmap when the effect draws
pixels rather than colour.

Re-take the reference after every *intended* change to the picture - a palette tweak, a
different speed - or the next check compares against something you already replaced.

## Prove the picture did not change

`screen_digest` hashes memory. The picture is made from that memory **and** from when the bank
is switched relative to the beam, so in multicolour the two come apart: timing that has drifted
paints a different screen out of byte-identical data, and the memory hash does not move at all.

```jsonc
frame_digest {"frames": 16, "border": false, "lines": true}
```

```
unique_digests 3
frame 0                                          digest 37191800a7b5
frame 1  38 lines changed  [[32,36],[40,44],...] digest 02c65d5f5b7a
frame 2  32 lines changed  [[32,32],[36,36],...] digest 8f22aa6f4a52
frame 3  same_as_frame 2
```

`lines` reports which scanline ranges differ from the previous frame, which turns "some frames
look wrong" into a line number. Lines moving at a regular interval point straight at the code
that switches banks on that interval - every fourth line for a 4-line multicolour toggle.

If a memory hash is stable across a stretch of frames while `frame_digest` is not, the data is
fine and the timing is not. That is not a paradox; it is the difference between the two tools,
and it is worth knowing before spending a session looking for a data bug that is not there.

## Find out whether raster code runs at the same time every frame

```jsonc
beam_log   {"action": "enable", "addresses": ["mc_isr", "mc_isr.strip"], "size": 200000}
run_frames {"count": 40}
beam_log   {"action": "dump", "count": 1}
```

```
mc_isr        hits 39   1 per frame    jitter_t 4    shape single
mc_isr.strip  hits 720  48 per frame   jitter_t 23   shape uniform
```

`beam_log` records the beam every time a watched address executes and never stops the machine,
so a whole run is one call. Watching by address covers both cases worth watching: the entry of
an interrupt handler, and the `OUT` that flips a bank, since in raster code that instruction
sits at a fixed address.

Read `jitter_t` first and `jitter_shape` second. Hits are numbered inside their frame and
compared position by position across frames, because a strip loop fires its `OUT` dozens of
times a frame and a plain minimum and maximum over every hit would only restate that the loop
spans the frame. `uniform` means every position moved by the same amount - the whole pass
displaced by a constant that varies from frame to frame, so the cause is before the pass
starts. `varies` means the spread grows along the pass, which is drift accumulating inside it.
Those are different faults with different fixes.

A few T-states of jitter on a handler entry is normal: the CPU takes an interrupt only on an
instruction boundary. Tens of T-states on the drawing loop are not.

## How much does one frame of the effect cost?

```jsonc
set_breakpoint {"address": "main_loop"}
run            {}
clear_breakpoints
frame_cost     {"frames": 8}
```

```
t_states_max 119478   interrupts_per_frame 2   budget_t 143360
headroom_t   23882    headroom_percent 16.66   fps 24.41
```

`run_frames` would have counted interrupts, and this effect takes two of them per frame, so
that unit answers the wrong question. `frame_cost` counts the work between two HALTs and
reports the wait separately - `idle_t` and `headroom_t` agree, which is the measurement
checking itself.

The number that matters is `interrupts_per_frame`. Frame rate is `fps_real / quantum`, so
cutting 20 % of the frame changes nothing at all unless it crosses a quantum boundary; below
that it buys headroom, which is a legitimate goal but a different one - decide which you are
chasing before you start.

## Find the expensive routine, then the expensive call

```jsonc
load_labels {"path": "build/project.labels"}
set_breakpoint {"address": "main_loop"}
run         {}
clear_breakpoints
profile     {"action": "start", "ranges": "symbols"}   // split by every label
frame_cost  {"frames": 1}
profile     {"action": "read"}
```

`ranges: "symbols"` needs no prior knowledge of the code: it makes one region per label and
reports the ones that ran, dearest first. Then narrow to the routine that came out on top and
ask what a single call costs:

```jsonc
profile {"action": "start", "ranges": [
           {"tag": "isqrt", "start": "isqrt_hl", "end": "compute_polar"}]}
run     {}
profile {"action": "read"}
→ "t_states": 788972, "calls": 1536, "t_per_call": 513.65
```

`t_per_call` is what you optimise against, and `calls` is worth reading on its own - if a
routine is called 1536 times when the field has 1536 cells, it is called once per cell, and
the cheapest optimisation is often to call it less rather than to make it faster.

Ranges are half-open: `start` up to but not including `end`, so naming the next label as the
end measures exactly one routine.

## Work out the frame rate an effect will run at

```jsonc
frame_timing                                     // t_states_per_frame 71680, fps_real 48.83
profile        {"action": "read"}                // t_states_per_frame per range
```

Budget against **`fps_real`** - the clock divided by the T-states in a frame, 48.83 Hz on a
Pentagon rather than the round 50 a Spectrum is usually quoted at. (There used to be an `fps`
beside it carrying that nominal 50; the core dropped the field and so did these tools.)
An effect costing `T` T-states per frame runs at `fps_real / ceil(T / 71680)`. The
`ns_per_*` fields have a matching `ns_per_*_real`, because the core's own nanoseconds are
quantised to a truncated integer and run 0.6 % short.

## See what actually happened on screen

```jsonc
run_frames {"count": 50}
screenshot {"path": "/tmp/effect.png", "border": false, "scale": 2}
```

`screen_text` decodes the screen to text via the ROM font, which is fine for menus and BASIC
listings, but it is a decode - for anything graphical trust `screenshot`, which is the real
frame buffer.

`screen_text` and `screen_attrs` read the page the ULA is showing and report it as
`page_on_air`. That is the answer you want by default. `read_memory` at `$5800` is not the
same question and will disagree the moment a program flips screens, because bank 5 stays
mapped at `$4000` whatever the ULA is drawing.

```jsonc
screen_attrs {}            → {"page": 7, "page_on_air": 7, "attributes": "28 28 ..."}
screen_attrs {"page": 5}   → {"page": 5, "page_on_air": 7, "attributes": "38 38 ..."}
```

## Look at a gigascreen, or anything that flickers on purpose

A gigascreen alternates two pictures every interrupt and means them to be seen as one. The
colour it is after is in neither frame - it is what the eye makes of the two. A plain
screenshot catches one of them, so what comes back is half the effect and looks like a bug
that is not there.

```jsonc
run_frames {"count": 50}
screenshot {"blend": 2, "border": false, "path": "/tmp/mix.png"}
→ {"blend": {"frames": 2, "frames_used": 2, "distinct_frames": 2,
             "gamma": 2.2, "pattern": "flicker"}}
```

`blend: 2` for a gigascreen, `3` for a three-frame effect. `video_config {"blend": 2}` makes
it the default for every later screenshot, digest and recording.

**Read `pattern` before believing the picture.** It says what the recent frames are actually
doing, and only one of the three answers means blending was the right thing:

- **`flicker`** - two pictures alternating, each frame equal to the one two before it. This is
  what blend is for, and the result is the picture a person watching the screen sees.
- **`animation`** - every frame differs from both of the last two. The effect is not
  flickering, it is moving, and averaging those frames is motion blur. The reply says so.
- **`still`** - nothing is changing, so the blend returns the frame unaltered.

`distinct_frames` is the other number to read. If it is 1, the frames that went into the blend
were identical: the effect holds each picture for more than one interrupt, so blending two of
them mixes duplicates and nothing appears to mix. `frame_cost` says how many interrupts the
effect really takes, and that number is the `blend` to use.

The mixing happens in linear light rather than on the sRGB bytes, which is why black
alternating with white comes out `#B6B6B6` and not the `#808080` that averaging the encoded
values gives. `video_config {"gamma": 2.4}` changes that transfer; `gamma: 1` averages the
bytes, which is the naive result and visibly too dark.

The frame history is dropped whenever the frames on either side of it would belong to
different programs - a load, a reset, a change of geometry - so a blend can never mix one
program's picture with another's.

To check that the *mixed* picture is stable, hash the mix:

```jsonc
frame_digest {"frames": 8, "border": false}              // alternates between two digests forever
frame_digest {"frames": 8, "border": false, "blend": 2}  // one digest, or the effect drifted
```

The raw digest of a flickering effect changes every frame by design and proves nothing. The
blended one is the picture a person sees, so a change in it is a change worth looking at.

`record_video {"blend": 2}` records the same way - a clip of the effect instead of a clip of
the flicker.

When the two pictures live in the two screen pages rather than in one, `beam_position` says
which is on air as `screen_page`, and `page` reads either one. That separates a question the
blended picture cannot answer - whether both halves are being drawn correctly - from whether
they combine into the right colour.

```jsonc
beam_position {}           → {"screen_page": 7, ...}
screen_attrs {"page": 5}   // the half being built
screen_attrs {"page": 7}   // the half on screen
```

## Record a clip

```jsonc
record_video {"frames": 250, "path": "/tmp/demo.gif", "border": false,
              "skip_until": "main_loop", "every_nth": "auto"}
record_video {"frames": 1500, "path": "/tmp/demo.mp4", "every_nth": 2}   // with sound
```

GIF is built in and exact. MP4/WebM goes through ffmpeg and carries the machine's real audio.
Frames stream to disk, so a five-minute recording costs no more memory than a two-second one.

Two arguments turn a clip of an effect from three calls plus arithmetic into one:

- **`skip_until`** winds to an address or label first, so the recording does not open on the
  black screen of the precalculation. (Or load a snapshot taken after init - see
  [Snapshot the machine once the precalculation is done](#snapshot-the-machine-once-the-precalculation-is-done).)
- **`every_nth: "auto"`** measures how many interrupts the effect takes per frame and records
  one hardware frame in that many. An effect on a quantum of 3 draws each picture three times
  running; recording all of them makes the file three times bigger and carries not one extra
  pixel. On a real effect: 628 KB at `every_nth: 1`, 214 KB on auto, same animation. The
  response reports the quantum it found under `every_nth_auto`.

## Answer "is there any sound, and where is it from?"

```jsonc
audio_capture {"frames": 50, "path": "/tmp/check.wav", "watch_ay": true}
```

Returns `silent`, peak and RMS, a zero-crossing pitch estimate, beeper toggles, and
`dominant_source` - beeper, AY or General Sound. With `watch_ay` it also logs every AY
register write with the frame, the T-state and the PC that made it, which is a music driver
laid out flat:

```
frame 100 t=7267  reg7 = 62   pc=$800D
frame 100 t=7325  reg8 = 15   pc=$801B
frame 100 t=7383  reg0 = 200  pc=$8029
```

`ay_state` decodes the current registers into frequencies and volumes; `sound_state` shows
every source at once when you need to know what is even wired up.

## Find out what your test never touched

```jsonc
coverage {"action": "start"}
run_frames {"count": 200}
coverage {"action": "read", "from": "$8000", "to": "$C000", "min_gap": 8}
```

The interesting part of the answer is `unexecuted_gaps`: the biggest stretches of code that
never ran, labelled where labels exist.

## Chase a crash

```jsonc
trace {"action": "enable", "size": 8192}
run   {"max_instructions": 20000000}
trace {"action": "dump", "count": 40}
```

The dump is disassembled, so you see the last forty instructions before things went wrong.
`set_breakpoint` with `access: "write"` on a variable answers "who wrote this?" instead.

## Drive BASIC

```jsonc
reset     {"boot_frames": 150}
type_text {"text": "p1\n", "frames": 4}          // PRINT 1 - "p" is one keyword press
run_frames{"count": 25}
screen_text
```

Remember the 48K ROM is in keyword-entry mode: `PRINT` is the `p` key, `LOAD` is `j`.

## Work with disks

```jsonc
disk         {"action": "insert", "drive": 0, "path": "game.trd"}
disk_catalog {"drive": 0}
disk         {"action": "save", "drive": 0, "path": "game-modified.trd"}
```

`disk_catalog` reads the image itself, so it answers before the machine has booted anything.
To have the *machine* see the disk, reset into TR-DOS - the romset needs a TR-DOS ROM in
page 3 for this to land anywhere:

```jsonc
reset        {"mode": "dos", "boot_frames": 300}
screen_text                                       // ✻ TR-DOS Ver 5.04T ✻ ... A>
```

If that comes back as a screenful of noise, check `machine_state`: a `disk_interface` of
`none` means the ROM is waiting on a drive that does not exist. Every ZX model gets one
automatically, so this should only ever happen on hardware that never had a drive.

## Try a hypothesis and roll it back

Snapshots are the cheap undo: `save_snapshot` before an experiment, `load_file` on the same
path to get back. Handy when poking memory to test a theory about a bug.
