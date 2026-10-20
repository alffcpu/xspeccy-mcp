# Changelog

Versions come from [`VERSIONS`](VERSIONS), which the CMake build and `build.py` both read.
`xspeccy-mcp --version` reports the server version and the Xpeccy release it was built
against, and says so when the two do not match what is pinned.

## Releasing

Which Xpeccy the server is built against is a decision made here and written down, not
whatever upstream happens to be serving on the day. Moving to a newer one is deliberate:

```sh
python build.py --pin-xpeccy v2026.8            # look it up, hash it, show the change
python build.py --pin-xpeccy v2026.8 --write    # apply it to VERSIONS
python build.py --smoke                         # rebuild and run the suite
```

The argument is an xpeccy-plus release tag (`v2026.8`, `2026.8` and `2026.8-plus` all name
it) or a 40-hex commit. The helper resolves a tag to a commit, fetches by that commit and
computes the source hash, because doing the hash by hand is how a wrong one gets committed.
The version it records is the tree's own `VERSION` file with `-plus` appended, which is what
the CMake build detects, so a pin by commit records a version name as well. It writes
nothing without `--write`.

Then read the failures rather than re-recording them. The expected values in `tests/smoke.sh`
were measured against the old release, so a change in them is a real difference in the
emulator until shown otherwise - screen digests especially, since those are the ones that
would silently redefine what "unchanged" means.

Last, bump `XSPECCY_MCP_VERSION` and add a section below. Use a release tag or a commit and
never `nightly`: it is a moving pointer, and a name that moves is not a pin.

## 1.4.0

The emulator core moved from samstyle/Xpeccy to the
[xpeccy-plus](https://github.com/dotkoval/xpeccy-plus) fork, release `v2026.6.2`, pinned by its
commit `1f259fc8` on the fork's main branch. The fork carries contended memory and I/O contention, snow and floatbus, and
machine definitions as data (`res/machines/*.conf`) instead of the hardcoded defaults this
adapter used to carry - a machine named through `machine_config` now comes up with the
contention, PSG chips, drives, ROM set and video timings its definition file declares. A
samstyle tree is no longer accepted at configure time.

54 tools now. `load_file` takes `.spg` snapshots and `.wav` tapes along with the disk
formats the core understands (`.udi`, `.fdi`, `.td0`, `.dsk`, `.ima`). `sdcard` mounts,
ejects, locks and reports the SD card image the fork's machines use. `read_memory` accepts
a physical RAM bank as `"bank:offset"` (both hex, like a sjasmplus symbol), which is how a
bank that is not paged in gets read - on TSConf that is the only way to look at an
arbitrary 16K page. `machine_config` also resolves a model by its machine-definition file
name (`evo-tsconf` finds the TSConf hardware).

Two correctness fixes worth naming: the adapter now sets `flgBRKMEM` when a breakpoint is
armed - the fork's `compExec` only consults the breakpoint maps under that flag, so direct
map writes otherwise armed nothing; and a dangling `else` in the machine-definition parser
that had silently skipped the `rom`/`storage`/`sound`/`input` sections since the parser
landed is fixed, with a unit test on the shipped `pent.conf` so it stays fixed.
`xhost_time_fixed` is pinned at init, so cold-RAM noise, the CMOS clock and power-on phase
no longer depend on `time(NULL)` - same binary, same bytes, every run.

And one accounting fix that the fork's timing rework exposed: the profiler, run/step
results and `frame_cost` now count T-states from the core's own `tickCount` delta - the
exact integer the cpu charges, contention waits included - instead of dividing the
reported nanoseconds back by `nsPerTick`. The fork keeps `nsPerTick` as a precise double
(upstream's truncated int was itself a bug: it slipped the Pentagon's frame interrupt off
the dot grid); truncating it back to int on our side read every T-count ~0.25% high.
The `ns` fields the tools report are unchanged keys, now computed as `t * nsPerTick`.

Further correctness fixes:

- **Machine definitions build the way upstream builds them.** A user's `machines/<id>.conf`
  is a patch over the shipped definition of the same stem, not a replacement; `inherit`
  resolves against the parent *as it ships*, so patching `zx128` cannot move `zxplus2`;
  every field of a definition is applied unconditionally (`mac_defaults` underneath), so
  switching machines can no longer leak contention, timing or storage state from the
  previous one; and `setModel` keeps the definition that was asked for - `zxplus2` and
  `zx128` share the ZX128 board but carry different roms and timing, and `machine_state`
  now reports which definition applied as `machine`. Definition ids are listed by
  `list_models` under `machines` and are valid `model` values.
- **Reset now means what the definition says.** `resbank` is applied, so a Pentagon
  resets onto its 128K menu like the hardware rather than straight into 48 BASIC.
- **Hostile media never reaches the core's parsers.** `.td0` and `.dsk` files are
  walked structurally before loading - the core's own readers treat EOF as success
  (a truncated `.td0` loops forever) and trust declared sector counts past their
  fixed tables. `.spg` compressed blocks are decoded by bounded mirrors of the
  megalz/hrust depackers inside the validator, so a malicious stream is refused
  before the loader sees it. `disk insert` gets the same checks as `load_file`.
- **`.bin` keeps its old contract.** The BK-format loader the API always had -
  little-endian address and length header, low byte of each word written at the
  even address - ported verbatim. It had silently become a raw disk write.
- **Machine selection checks what the board can do, not what it is called.** A
  `.dsk` needs a +3DOS controller, a `.trd` needs Beta Disk on a compatible core,
  an `.spg` needs TSConf - the same pairing upstream's `filemachine` uses, and a
  user patch that drops the disk controller is no longer picked for a disk file.
- **Machine definitions parse the way the GUI parses them.** Inline `#`/`;`
  comments, case-insensitive sections, upstream's boolean table (anything not an
  explicit `no`/`off`/`0`/`false`/`n` is true), and an empty `inherit =` clearing
  the parent rather than meaning "key absent". `[GENERAL] machine` now outranks a
  legacy profile's `[MACHINE] current`.
- **Switching machines leaves nothing behind.** `memory = 0` means 64K as upstream
  has it, 768K fits where the board's mask allows it, a missing GS rom or font is
  cleared rather than inherited from the previous machine, the old frame image is
  dropped instead of being re-read at the new geometry's pitch, and a definition
  whose roms are all dead falls through to the built-in rescue set.
- **Timing reports what the core counted.** Frame and line T-states come out at
  the rounded value - a ZX128 reports its printed 228 T/line, not 227 - `fps_real`
  follows the effective (turbo-adjusted) tick, and `ns` figures in run/step/profile
  accumulate the nanoseconds `compExec` actually returned instead of being derived
  from T-states under the final clock. The geometry-mismatch warning compares
  against the right canonical frame for every shipped machine.
- **ROM files resolve in two places**: the user's `roms/` first, then the roms shipped
  with the core - and a definition whose roms cannot be loaded falls back to the
  configured romset or the built-in ROM rather than booting on `0xFF`.
- **`.spg` is validated before the core sees it** - signature, descriptor table and
  declared payload sizes - because the loader walks them unchecked.
- **`sdcard` rejects images `>= 1 GiB`** (the core's capacity math is 32-bit) and resets
  the controller on mount/eject so a swapped card does not inherit a transfer.
- **`read_memory` requires `address`** and range-checks a bank before it narrows -
  `"100000000:0"` used to wrap silently into bank 0.
- **Pre-execution breakpoints no longer count the instruction they stopped** - a fetch
  or irq break that fires before the opcode runs (`flgBRKPRE`) stays out of the
  instruction count, the profile and coverage.
- **T-state deltas survive `tickCount` overflow** - the counter is a signed int that
  wraps near `INT_MAX`, and deltas are taken as unsigned differences.

One real behaviour difference worth knowing rather than chasing: the Pentagon's
rom serves IM1 through a trampoline at `$5B00` that only its boot code writes -
on cold RAM an interrupt lands on power-on noise and becomes an `rst #38` loop.
Tests that enable interrupts must boot the machine first.

More fixes against upstream behaviour:

- **Three settings now reach the core through its own setters.** `timing.contendedMemory`
  calls `comp_set_cont` (writing the flag alone never wired the CPU hook, so contention
  was reported on and did nothing), `sound.chipType` calls `chip_set_type` (the chip was
  relabelled but kept the previous chip's callbacks), and `sound.chipFrequency` goes
  through the timing recalculation that sets `tickFx` (the pitch was ~14% off).
- **`load_file` picks the machine the file needs**, the way upstream's `filemachine`
  does: `.spg` lands on TSConf, snapshots on the hardware they were saved from when the
  current machine cannot run them, TR-DOS disks on a Beta Disk machine, `.dsk` on a +3 -
  and the reply names the `machine` and `model` it switched to.
- **Half-dot video modes capture both pixels.** The TSConf/ATM/EVO text modes, ATM
  hardware multicolour and both Profi hires modes draw two different pixels per dot;
  captures and digests used to drop the odd one, so such a screen came out at half its
  real width. `screen.width` now reports the full width too. A frame is read in the mode
  it was drawn in, not the one the machine is in when the capture is taken, and
  `record_video` keeps one width throughout: a frame in the other pixel width is resampled
  to it (`frames_resampled`), and frames of a changed geometry are counted in
  `frames_dropped` instead of vanishing. `border: false` crops the ATM/Evo and Profi
  screens to their own paper (320x200, 256x240 dots), not the ZX one.
- **Smaller fixes**: `[GENERAL] machine` accepts a hardware name as well as a definition
  id, `TSLab` is a model alias for TSConf, an `sdcard mount` no longer inherits the
  previous card's lock, the geometry-mismatch warning covers all 128K machines, and the
  smoke suite's `dd` call is spelled so GNU `dd` accepts it.
- **The pinned-core hash now covers the runtime resources** (`res/machines`,
  `res/layouts.conf`, `config/roms`) that decide what a machine boots as, not only the
  compiled sources.

Media validation and failed-load rollback:

- **Every loadable format is walked before the core sees it.** The disk and tape
  parsers the core ships fread whatever a file declares and report success when it
  runs out, so a truncated image "loaded" a disk or a snapshot out of whatever the
  destination buffers already held - and refused formats still switched machines and
  cold-filled RAM first. `.trd`, `.scl`, `.tap`, `.tzx`, `.sna`, `.z80` and `.ima`
  now have the same structural preflight `.td0`/`.dsk`/`.fdi`/`.udi`/`.spg` already
  had: the file must carry what its own fields promise, checked before
  `machineForFile` can touch anything.
- **Two crashes and a leak closed in the walk itself.** A zero-sector `.fdi` track
  divided by the sector count inside `diskFormTrack`; a `.tzx` block `0x11` with a
  zero data field became `fread(SIZE_MAX)` into a 0-byte malloc; a `.tzx` block
  `0x19`'s symbol tables are now bounded against their block (the repro read 506
  bytes past it). `.udi` records of unknown type are refused - the core leaks their
  buffers and never uses the data anyway.
- **`.ima` actually loads now.** All of the core's drives are DD, where a 1.44M
  image's tracks silently failed to format while the loader reported success on the
  stale disk - the adapter enables the HD geometry on just that drive (not the
  whole FDC, which would have silently reshaped every mounted disk) and verifies a
  sector reads back.
- **`.trd` no longer ends on a phantom.** The core's load loop formats one stale
  track past the file end on a boundary; the adapter clears it after a load.
- **`.sna` sizes follow the real 128K layout.** Which bank the file pages in
  decides whether it carries five or six trailing pages (131103 vs 147487) -
  matching `saveSNA`'s own write loop, so a real round-trip still lands.
- **`.z80` page walk mirrors the loader.** 48K snapshots map their pages
  through the odd 4/5/8 scheme, and a short v2 header still starts its blocks
  at offset 55 - the validator's view is now exactly the loader's.
- **Mounted disk density survives everything.** Reset used to re-mark every
  drive DD and silently break a mounted `.ima`; so did a turbo change, a model
  switch or a snapshot's own reset. The adapter now remembers each drive's
  formatted geometry and restores it whenever `zx_init` shrinks it.
- **Short `.trd` tails are zeros, not leftovers.** A file ending mid-track had
  its missing sectors formatted out of the previous track's bytes; they are
  blanked - through the core's own sector writer, so their CRCs verify - and
  the boundary phantom's sector map is cleared with its track.
- **`.z80` duplicates prove nothing.** The loader stops after counting as
  many page blocks as the machine has banks - duplicates included - so a
  file whose repeats end the walk before every bank arrives is refused
  instead of loading half the RAM.
- **`.fdi`/`.td0`/`.dsk` that cannot fit are refused.** A track the drive has
  no room for used to leave the old disk in place while reporting success -
  every `diskFormTrack`-fed format now has to fit the real track length
  before it loads. An extended `.dsk` sector shorter than the bytes the core
  writes into the track loads with its tail zeroed and its CRC rewritten -
  the tail used to come out of the previous sector's buffer.
- **A refused `load_file` gives back what the switch took.** Picking the
  file's machine could eject drives its drive count did not fit; on any
  failure past the validators the machine and every mounted drive are
  restored - flags, path, geometry and the dirty bit included. The machine
  restore itself is attempted first and cannot eat the original error or
  skip the drives, and a drive that survived a failed load still gets its
  track geometry back - a failed `.ima` used to leave a DD disk at HD
  length. The rollback ends where the load succeeds: an error building
  the reply afterwards can no longer un-format the freshly written disk.
  A file that gets past the validators and still fails mid-format - the
  image changed on disk between the check and the open, say - used to
  leave its own path over a half-written disk; the target drive's track
  bytes are part of the snapshot now, and a failed `load_file`/`disk
  insert` hands the previous disk back whole or leaves the drive empty.
  `load_file`'s `drive` argument is range-checked too: a 4 used to land
  on drive A by way of `drv & 3`.
- **A tiny `.tzx` can no longer eat the memory.** The core stores every pulse
  and grows its buffer with an unchecked `realloc` on an `int` count, and a
  20-byte file looping a pure-tone block expands into billions of pulses. The
  validator now runs the tape the way the core's interpreter does - loops,
  calls, jumps, the step limit - and refuses past 2^26 pulses, CSW-compressed
  data included.
- **A refused `load_file` no longer wipes the session.** The track-fit check ran
  after the switch to the file's machine, and switching back cold-filled RAM:
  memory and PC were gone. The check runs before any switch now, and a failure
  that still comes after one restores a core snapshot taken before it. An
  `.ima`'s HD track length no longer outlives it: eject and any other image put
  the drive back to DD (`disk state` shows `track_bytes`).
- **Smaller media fixes.** An SD image of exactly 1 GiB mounts (only larger
  ones break the core's math). An extended `.dsk` track-length byte of `0x80`
  or more is refused, since the core reads it as a signed or unsigned char
  depending on the host.
- **The binary carries what the core keeps as data.** Machine definitions,
  layouts and the shipped ROMs (GS firmware and fonts included) used to be read
  at run time from the source tree the binary was built from: moved or with
  `build/` deleted, `zx48` and `evo-tsconf` were unknown machines, ZX48 ran on
  the default geometry at 71680 T, and Pentagon booted the 48K ROM. They are
  compiled in now (`cmake/builtin_resources.cmake`), under `XSP_BUILTIN_ROM` for
  the ROMs; a user's file of the same name still wins.
- **Sound and contention settings reach the whole board.** `sound.chipType`,
  `sound.chipFrequency` and `sound.chipStereo` set every chip, as the GUI does -
  a TurboSound used to have one half retuned. A chip type change re-derives the
  clock (a YM2203 runs at twice the AY's). `timing.contPattern` also picks the
  screen drawer, as the core's `zx_set_vmode()` does, instead of leaving the
  early fetch to the next reset.
- **Breakpoints land where they say.** `clear_breakpoints` clears the cells it
  armed even after the model or romset changed under them (it used to re-mask
  them with the new machine's mask and miss). A `bank:offset` naming a bank the
  machine does not have, or an offset past `$3FFF`, is an error; `FF:` means
  "no bank" as in a symbol file. A port breakpoint up to `$FF` matches the low
  byte, so `$FE` catches `out ($FE),a` whatever A is. `read_memory` follows the
  same `bank:offset` rules and refuses a read off the end of a bank.
- **Taking an interrupt is not an instruction.** It used to be counted as one
  at the interrupted address, in run/step results, the profiler, coverage,
  trace and `beam_log`. Results carry `interrupts`, the profile an
  `interrupt_acceptance` block, so the parts still add up to `t_states`.
  Coverage marks each instruction by its real length (a taken `jr` no longer
  covers the bytes it skips), and `beam_log` no longer logs a fetch-break PC
  twice.
- **A failed change leaves the machine as it was.** A romset that fails to load
  keeps the ROM that was there (it used to be wiped to `FF` while the old name
  was still reported). `machine_config` checks romset and geometry names before
  switching the model, and resets whatever it did apply when a later step
  fails; its reply describes the machine after the boot, and a geometry change
  with `boot_frames: 0` shows a cleared frame instead of the old one on the new
  pitch. Notes from earlier machines no longer pile up in `config_source`.
- **Machine files read as upstream reads them.** A UTF-8 BOM no longer hides
  the first section, `border.min = no` is the upstream spelling (`none` still
  works), and a ROM shorter than 16K is padded with `FF` instead of mirrored.
- **The pin is a release.** The core was pinned to the tip of an unmerged
  branch, a commit no branch or tag of the fork holds and the host may drop.
  It is `v2026.6.2` now, on the fork's main branch, which carries everything
  the old pin had plus a few later core fixes; the version floor follows it.
- **A bare server says what it runs.** With no configuration it starts on the
  Pentagon definition (128K, Pentagon ROMs with TR-DOS, Beta Disk, AY), and
  the startup line now names that definition instead of the profile's romset
  and geometry, which made it read as a 48K ROM on the default geometry. A
  build with `XSP_BUILTIN_ROM=OFF` finds the fork's `config/roms/48.rom` for
  its fallback romset again; it was still looking for samstyle's
  `conf/1982.rom`.
- **`build.py`** runs on Python 3.8 again, and `--pin-xpeccy <commit>` records
  the tree's own version as the CMake build detects it, so pinning the current
  commit reproduces `VERSIONS` instead of making every build warn.

## 1.3.1

A stability release. No new tools, and no correct call answers differently.
What changed is that a set of *incorrect* calls, which used to hang the server
or be answered with something plausible, are now refused with a reason.

Built and tested against Xpeccy `0.6.20260820`, the same as 1.3.0.

### Calls that never came back

`execLoop` reads `maxInstructions >= 0` as the test for having a budget at all,
so a negative number did not shorten the budget - it switched it off. Two
arguments reached it unchecked:

- `skip_max_instructions`, taken by `record_video`, `screen_digest`,
  `frame_digest` and `frame_cost` through `skip_until`
- `max_instructions` on `run_to_line`

Either of them set to a negative value, with a target the program never reaches,
ran until the process was killed. Four neighbouring budgets had been bounded
already and these two were missed. Every budget and every output size now has a
range, and the suite sends the values that used to hang it.

### Requests that got no reply at all

A request that was valid JSON but not a valid JSON-RPC message made the server
read a field off the wrong kind of value. That throws rather than defaulting,
and the throw reached the read loop, which logged it and sent nothing - so a
client that had sent an `id` waited for an answer that was never coming. A
top-level array or number, a non-string `method`, a `params` that is not an
object, and a `tools/call` whose `name` is not a string each did it.

All of them are answered now, with `-32600` or `-32602` and the `id` the client
sent. The protocol version in `initialize` is negotiated against the revisions
this server has been checked against rather than echoed back unread.

### Answers that were wrong rather than missing

- `read_memory` accepted an address past `$FFFF`, read the wrapped one, and
  reported the number it had been given. The reply held the question and a
  different question's answer at once.
- `set_breakpoint` took any word as `access`. A typo like `"wrtie"` armed an
  execution breakpoint, reported success and echoed the typo back, so the
  breakpoint waited somewhere the caller was not watching.
- `gamma: "2.2rubbish"` was read as 2.2. The settings parser refused the same
  string, and the two disagreeing about the project's own rule meant one of them
  was wrong.
- An argument name a tool does not declare was ignored. `screen_text` with
  `pages` instead of `page` returned whichever page was on air as if that had
  been the question, and `read_memory` with `len` instead of `length` returned
  sixteen bytes with nothing in the answer to say so. Refused now, with the
  accepted names listed; the schemas say `additionalProperties: false`.
- Seven arguments were documented as refused when out of range and were
  silently clamped instead. Now refused.
- `--config` pointing at a directory that is not there fell through to the
  search order, and the server came up on whatever configuration the host
  happened to have - a different machine from the one that was asked for. It is
  an error now, and so is a missing `--settings` file and an unrecognised option.

### Recording

- The PNG, WAV and GIF writers asked `ferror()` and then closed without looking
  at the result. `fclose` is where the buffered tail reaches the disk, so all
  three could report a truncated file as a written one - and `record_video`
  dropped the writer's verdict anyway.
- `every_nth` had no upper bound. Past a few hundred the output rate falls under
  one frame a second, which ffmpeg rounds to zero and which overflows the GIF's
  16-bit frame delay. Now 1..1000.
- Two servers recording with sound at the same time wrote over each other's
  intermediate files, and neither found out. Those files carry the process id
  now, and are deleted however the recording ends.
- An error part-way through a recording left the audio capture running, so every
  later execution went on filling it.
- The sample buffer holds a minute of sound. Past that the soundtrack stops, and
  because ffmpeg is told `-shortest` the picture was cut to match without a word.
  Both `audio_capture` and `record_video` say so now.

### Licensing

`THIRD_PARTY_NOTICES.md` lists what a built binary contains that is not ours:
the Xpeccy core, nlohmann/json, zlib, and the 16K ROM. The ROM is somebody
else's copyright and is compiled in by default, because a tool nobody can use
without one should work when it is copied to another machine. `-DXSP_BUILTIN_ROM=OFF`
leaves it out, and the server then takes a ROM from the host.

### Under the hood

`src/xsp_mcp.cpp` was 3036 lines, 2354 of them a single `registerTools()`
function holding every one of the fifty-three handlers as an anonymous lambda.
It is now the protocol (`xsp_server`), the argument rules (`xsp_args`), the
tools themselves (`src/tools/`, one file per subject), and an entry point that
holds the command line and nothing else. `dispatch()` returns the reply rather
than printing it, and `serve()` is the only thing that touches a stream, which
is what made the protocol testable at all.

The move itself was mechanical and was checked as one: at the commit that made
it, every line of the registrations was byte-identical to its old self and the
tool set, the schemas and the descriptions were unchanged. Everything above came
afterwards, on top of it.

The modules shared by the server and the tests are compiled once rather than
once per target, and copy construction is deleted on the four classes that own
something and free it by hand.

### Tests

The unit suite roughly quintupled and the end-to-end one grew by a sixth. New
suites for the keyboard matrix, the platform layer, the configuration reader,
the settings table against a real machine, the GIF recorder, the audio summary
and WAV writer, the argument rules, and the JSON-RPC layer.

`tools/coverage.py` builds an instrumented tree, runs both suites and reports
lines, regions and branches per file, weakest first, failing below a floor. Line
coverage went from the mid eighties to over ninety per cent and branch coverage
from under sixty to over seventy; the figure moves with every commit, so the
command is the claim rather than a number written down here.

Three checks in the end-to-end suite could not fail: the server's exit status
was never examined, one grepped a file nothing writes to, and one was satisfied
by a reply from before the event it tests.

Two things the tests found rather than confirmed: a `Computer` is 4.6 MB, so one
on the stack overflows it, and a ZX48K reports 64K of memory because the core
allocates RAM in powers of two. The first is fixed; the second is written down,
because it looks like a bug and is not one.

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
