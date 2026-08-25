# Changelog

Versions come from [`VERSIONS`](VERSIONS), which the CMake build and `build.py` both read.
`xspeccy-mcp --version` reports the server version and the Xpeccy release it was built
against, and says so when the two do not match what is pinned.

## Releasing

Which Xpeccy the server is built against is a decision made here and written down, not
whatever upstream happens to be serving on the day. Moving to a newer one is deliberate:

```sh
python build.py --pin-xpeccy 0.6.20260901          # look it up, hash it, show the change
python build.py --pin-xpeccy 0.6.20260901 --write  # apply it to VERSIONS
python build.py --smoke                            # rebuild and run the suite
```

The helper resolves the tag to a commit, fetches by that commit and computes the source hash,
because doing the hash by hand is how a wrong one gets committed. It writes nothing without
`--write`.

Then read the failures rather than re-recording them. The expected values in `tests/smoke.sh`
were measured against the old release, so a change in them is a real difference in the
emulator until shown otherwise - screen digests especially, since those are the ones that
would silently redefine what "unchanged" means.

Last, bump `XSPECCY_MCP_VERSION` and add a section below. Use a dated upstream tag and never
`stable` or `minor`: both are moving pointers, and a name that moves is not a pin.

## 1.3.0

Seeing a picture that flickers on purpose, settings that were fixed in the source, and
which of the two screens is actually on air. **51 tools -> 53.**

Built and tested against Xpeccy `0.6.20260820`.

### New tool

| Tool | What it does |
|---|---|
| `video_config` | Reads or changes how frames become pictures: the default blend, its gamma, how many completed frames are kept, greyscale, border size. No arguments = report, including the 16-colour palette in use. |
| `settings` | Every emulator setting this server can change: what it is now, what the default is, where the current value came from, and what changing it costs. `set` changes one for the session, `save: true` writes it to a file, `reset` puts it back. |

### Added

- **`blend` on `screenshot`, `frame_digest` and `record_video`.** A gigascreen alternates two
  pictures every interrupt and is meant to be seen as one; the colour it is after is in
  neither frame, because it exists only where the eye and the phosphor average the two. A
  screenshot catches one of the alternating frames - the one thing the effect was never meant
  to show - so what came back was half the effect and looked like a fault that was not there.
  `blend: 2` averages the last two completed frames, `blend: 3` the last three, and the
  result is the picture a person watching the screen sees.

  The average is taken in linear light, not on the sRGB bytes: black alternating with bright
  white comes out `#B6B6B6` rather than the `#808080` that averaging encoded values gives,
  and too dark is exactly the failure the
  [reference on gigascreen colour](https://hype.retroscene.org/blog/graphics/808.html)
  is about. `gamma` is that transfer and defaults to 2.2; at 2.4 the same mix is `#BCBCBC`,
  which is the value that article arrives at.

  `frame_digest {"blend": 2}` hashes the mixed picture, which is the only way to ask whether
  a flickering effect is stable: its raw digest alternates between two values forever and
  says nothing.

- **`distinct_frames` in the reply**, next to `frames_used`. An effect that holds each picture
  for two interrupts blends two identical frames and looks unmixed for a reason that has
  nothing to do with the blend, so the reply says how many of the blended frames actually
  differed rather than leaving that to be guessed.

- **A GIF of a blended recording gets a palette built from the blended picture.** The ZX
  palette is exact for a raw frame and wrong for a mixed one: every averaged colour would be
  mapped back to the nearest pure one, which is the mixing thrown away on the last step.

### Notes on how it works

The server keeps the last few completed frames as it runs (`frame_history`, four by default)
and composes the blend from copies. Nothing is blended in place, so a raw screenshot taken
after a blended one is the same raw screenshot it would always have been, two blended
screenshots of the same moment are byte-identical, and no measurement moves. Keeping the
history costs one frame-sized copy per emulated frame, which did not show up above the noise
in 500-frame runs; `frame_history: 0` switches it off.

This is deliberately not upstream's antiflicker. Xpeccy blends destructively into the buffer
it displays, and its adaptive modes guess which pixels are "really" alternating so that a
static picture stays sharp on a monitor - both of which are right for a human watching and
wrong for a tool whose output is treated as ground truth. What is kept from upstream is the
part that is not a matter of taste: the averaging happens in linear light.

The history is dropped on a reset, a `load_file` and a geometry change, because frames from
either side of one of those belong to different pictures.

### Added: settings

- **19 settings that could not be reached before.** An audit of every setting found the
  server quietly running a machine nobody had chosen: the reset bank booting 48 BASIC while
  the emulator's own profile said TR-DOS, contention on a different group of banks than the
  profile asked for, and nothing configuring sound at all. Those are now rows in a table:
  the contention pattern, early timing, border step, contended memory and I/O, the screen-port
  wait, the CPU multiplier, the reset bank, floppy turbo, the AY type, stereo mode and clock,
  and the picture settings.
- **A settings file.** `xspeccy-mcp.conf`, the same `KEY = VALUE` with `[section]` headers as
  everything else here. Read from beside the Xpeccy configuration, then from the working
  directory, then from `--settings`; later wins. The working-directory one is the point: a ZX
  project carries its machine in its own repository, so every session starts the same and
  changing it is a commit rather than something somebody once typed. Saving preserves the
  comments, order and sections already in the file.
- **Where a value came from** is reported per setting - core default, settings file, or this
  session - because "why is the machine like this" cannot be answered without it.
- **`pattern` on every blended capture**: `still`, `flicker` or `animation`. Blending is for
  flicker, two pictures alternating that are meant to be seen as one. An effect that is simply
  animating also has frames that differ, and averaging those is motion blur; the reply now
  says which it is rather than leaving both looking alike. Found by running the blend against
  a real effect, where it silently returned a smeared picture.

### Notes on settings

Settings are refused outside their range, never clamped: a value that is quietly corrected is
a setting that does not do what the file says it does. Changing anything under `timing.`
invalidates every `frame_cost`, `profile` and `beam_log` measured before it, and the reply
says so. `boot.resetBank = dos` only reaches TR-DOS if the romset carries a TR-DOS ROM; the
stock ZX48 romset does not, and the setting says so rather than leaving a blank screen
unexplained.

Settings fixed when the server is built are deliberately not in the table, because listing
them would suggest they could be changed.

### Changed: the upstream pin

- **Upstream pin moved to `0.6.20260820`** from `0.6.20260804`. Checked before moving: the
  whole smoke log comes out line for line identical on both, pinned frame digests included,
  and raster timing, beam position, frame digests and frame cost agree on Pentagon, ZX48K,
  ZX128K and Scorpion. The release itself is upstream's TSConf/PentEvo and FM work, so almost
  none of it reaches a Spectrum. One thing does: `ay-3-8910.c` changed the AY stereo mix from
  `left = lef + cen/2` to `left = (11*lef + 5*cen)/16`, which moves the levels `sound_state`
  and `audio_capture` report once the AY is actually sounding. The suite does not pin those
  values and this change does not add a pin for them.
- **The oldest Xpeccy that compiles is now `0.6.20260820`.** That release renamed
  `Video.curscr` to `Video.vidPage`, and the tools below read it. `XPECCY_MINIMUM` and the
  CMake probe moved with it - the probe now looks for `vidPage` in `video/video.h`, which is
  the stricter of the two markers this project has used.

### Added: unit tests

- **`tests/unit/`**, 411 checks over the logic that needs no emulator: the linear-light colour
  arithmetic, the frame ring, the 128K `bank:offset` map, the listing lookups and the PNG
  writer. It links only the modules that do not reference `Computer`, which keeps the claim
  "this part is free of the core" checkable rather than asserted, and builds in about a second.
  No test framework: the harness is forty lines, and this project vendors one dependency.
- The reason it earns its place is that the end-to-end suite cannot see inside. Reversing the
  order of the frame history leaves `smoke.sh` passing all 135 checks - a two-frame blend is
  the same colour either way round - and fails four unit checks. Wrong-everywhere lookup
  tables have the same property: they hash consistently with themselves forever.
- `python build.py --smoke` runs both, unit first; `ctest` runs the unit suite; the target is
  `EXCLUDE_FROM_ALL` and can be switched off with `-DXSP_UNIT_TESTS=OFF`.

### Fixed

- **The test suite runs on macOS.** It used `mktemp --suffix`, `mapfile` and `stat -c%s`, all
  three GNU spellings that BSD userland and the bash 3.2 macOS ships do not have. The first
  one printed `unrecognized option` and returned nothing, so every path built from it was
  empty and seventeen checks failed for a reason unconnected to the server. macOS now passes
  135 of 135.
- **The test suite no longer depends on the machine it runs on.** It read whatever Xpeccy
  configuration happened to be installed, while its expected values were measured against the
  built-in defaults, so on a developer's own machine the romset and screen geometry were
  somebody else's and three checks failed. It now runs the server against an empty
  configuration directory. For a project whose argument is determinism, a suite whose answers
  moved with the host was the wrong thing to leave in place.

### Added: which screen is on air

- **`screen_text` and `screen_attrs` read the screen that is on air**, and report it as `page`
  and `page_on_air`. Both used to read through the CPU's view, and on a 128K or a Pentagon
  bank 5 is mapped at `$4000` whatever the ULA is doing - so for anything that flips screens
  between frames, both tools described the screen nobody was looking at. They now read the way
  the ULA does (`vid_mrd_cb`: `ramData[MADR(page,adr) & ramMask]`), which is also what makes
  this correct on a 48K, where there is no bank 5 and the address wraps onto the one screen.
- **`page` argument** on both, to read a given RAM page anyway. `{"page": 5}` while 7 is on air
  is how you compare the two halves of a gigascreen, or look at the frame being built next.
- **`beam_position` reports `screen_page`**, the same number, so a raster stop says which
  screen the beam was on.
- **`beam_position` reports `blank_x`/`blank_y` and `interrupt_at`.** The core tracks the beam
  in three coordinate systems and only two were visible. The third counts from the leading edge
  of the blanking, and it is the one the frame interrupt is defined in, so the distance from
  `blank_x`/`blank_y` to `interrupt_at` is how far the beam is from `INT` - which cannot be
  worked out from `dot`/`line`, because the blanking is not part of the visible image those
  count from.

## 1.2.0

### Added

- **[COOKBOOK.md](COOKBOOK.md)**, recipes for the tools: which calls to make for a given
  question, in what order, and which of them answer questions that sound the same and are not.
  Worth handing to an agent before it starts, since tool descriptions say what each call does
  and not which to reach for. It covers picking a machine and getting past a precalculation,
  raster timing with `beam_log` and `run_to_beam`, proving a change altered neither the data
  nor the picture, profiling, recording, coverage, disks and BASIC.

### Changed

- **`raster_log` is now `beam_log`.** On this platform "raster" means both the scan and the
  bar effects drawn against it, so the name could be read as a log of the picture. The beam
  is what the tool actually records, and the rest of the family already says so:
  `beam_position`, `run_to_beam`, and the `beam` block on every stop.

There is no alias for the old name. It existed for part of a day, and carrying a second name
for a tool nobody had time to call costs more than it saves.

## 1.1.0

Raster debugging, and a dependency that is actually pinned. **48 tools → 51.**

Built and tested against Xpeccy `0.6.20260804`. All 51 tools were exercised against that
release; see *Compatibility* below.

### New tools

| Tool | What it does |
|---|---|
| `frame_digest` | Hashes the frame **as the ULA drew it**, per frame, and with `lines` reports the ranges of scanlines that moved since the previous one. |
| `raster_log` | Records where the beam was every time a watched address executed, without stopping, and reports the jitter across frames. |
| `run_to_beam` | Runs until the beam reaches a raster position, wrapping into the next frame when the target is already behind. |

Why `frame_digest` exists as a separate thing from `screen_digest`: the latter hashes screen
*memory*. In multicolour the picture is a function of that memory **and** of when the bank is
switched relative to the beam, so raster code whose timing has drifted paints a different
screen out of byte-identical data and a memory hash does not move at all. If the data hashes
stable and the screen still looks wrong, that is not a paradox - it is the difference between
these two tools.

`raster_log` numbers hits within their frame and compares them position by position across
frames, because a multicolour strip loop hits its `OUT` dozens of times a frame and a plain
min/max over every hit only restates that the loop spans the frame. `jitter_shape` says
whether every position drifts by the same amount - the whole pass displaced by a constant -
or by different amounts, which is drift accumulating inside the pass. Those are different
faults. `by_position` adds the full breakdown and is off by default, because 48 entries per
address on every call is a lot of reading for one number.

`run_to_beam` treats a target that is already behind as meaning the next frame, so calling it
repeatedly with the same line walks the same point of the raster frame after frame.

### Also added

- Every stop - `run`, `run_frames`, `step`, `step_over`, `step_out`, `step_line`,
  `run_to_line`, `run_to_beam`, breakpoints - now carries a `beam` block with the line, dot,
  T-states into the frame and frame counter. Raster work asked for that after nearly every
  stop, and it is now already there.
- `VERSIONS`, one file holding the server version, the pinned Xpeccy release, its commit and a
  content hash of its sources. Both the CMake build and `build.py` read it.
- `build.py --pin-xpeccy VERSION` resolves an upstream tag to its commit, hashes the sources
  and reports the `VERSIONS` lines that would pin it; `--write` applies them.
- `--version` reports both versions; the startup banner does too.
- The `initialize` reply now carries MCP `instructions`: what the server is, the order to do
  things in (pick a model, load, run past the precalculation, snapshot), and which tool
  answers which question. A tool description can say what one tool does; it cannot say which
  of fifty-one to reach for, and an agent sees no README. Six of the shortest descriptions
  also gained the *when* to go with the *what*.

### Xpeccy is now versioned, not just downloaded

Before this release the pinned version lived in `build.py`, the *minimum* the build would
accept lived in `cmake/xpeccy.cmake`, and nothing compared the two. A tree newer than the pin
sitting on disk was picked up in silence - which is how a build quietly stops being the one
the tests were written against.

- The version is pinned **by commit**, not by tag name. Upstream also publishes tags called
  `stable` and `minor` and both are moving pointers, so a name is not a pin.
- `build.py` verifies what it downloaded against a hash of `src/libxpeccy` - the only part
  compiled here - and refuses to build if it is not the pinned source. The cached copy is
  re-checked on every build, which also catches an accidental edit to a dependency this
  project's rules say is never to be edited.
- Line endings are normalised before hashing. A tree that reached the disk through a Windows
  git checkout carries CRLF and is otherwise identical; hashing raw bytes would report a
  corrupt dependency on every Windows machine.
- A tree given with `--xpeccy-src` is checked too, but only warned about. Aiming at a patched
  copy is deliberate - the emulator's own GUI has to be patched to compile on Windows at all -
  and the binary records which tree it came from either way.
- The CMake build warns when handed a tree that is not the pinned release.

### Compatibility

All 51 tools were run against Xpeccy `0.6.20260804` and all 51 answered. Nothing in this
release changes an existing tool's arguments or removes a field, so anything written against
1.0 keeps working; the `beam` block is added to stop replies and existing fields stay put.

The automated suite (`python build.py --smoke`, 110 checks) exercises 41 of the 51. The other
ten need a fixture the suite does not carry - a disk image, a listing file, a keyboard, a
second machine model - and were verified by hand for this release: `disk_catalog`,
`list_models`, `press_key`, `release_keys`, `run_to_line`, `save_snapshot`, `screen_attrs`,
`set_port_breakpoint`, `step_line`, `step_out`.

### Changed

- `build.py` downloads Xpeccy **by commit SHA** rather than by tag name. Tags can be moved,
  and two of upstream's (`stable`, `minor`) are moving pointers by design.
- `build.py` verifies the downloaded sources against a hash of `src/libxpeccy`, with line
  endings normalised, and refuses to build if they are not the pinned ones. A tree given with
  `--xpeccy-src` is checked too, but only warned about: pointing at a patched copy is a
  deliberate act, and the emulator's own GUI has to be patched to compile on Windows.
- The CMake build warns when it is handed an Xpeccy tree that is not the pinned release. It
  used to enforce only a *minimum* version, so a newer tree sitting on disk was picked up in
  silence - which is how a build quietly stops matching the one the tests were written for.
- `screen_digest` is documented as proving the *data* did not change, which is not the same
  claim as the picture and had been worded as though it were.

### Notes

The server reported itself as `0.1` over MCP up to and including the 1.0 release. It now
reports what `VERSIONS` says.

## 1.0

First public release, [github.com/alffcpu/xspeccy-mcp](https://github.com/alffcpu/xspeccy-mcp).
48 tools over the Xpeccy core: run and step, breakpoints by address or bank, memory, assemble
and disassemble, labels and listings from sjasmplus, T-state profiling, coverage, screen as
text/attributes/hash, PNG, GIF and MP4 capture, AY and beeper inspection.
