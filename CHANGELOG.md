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

## 1.2.0

One rename, nothing else.

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
