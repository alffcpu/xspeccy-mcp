# Architecture

## The idea

Xpeccy is a Qt application, but its emulator core is not. `src/libxpeccy` is ~130 files of
plain C with no Qt, no SDL and no OpenGL anywhere in it - all of that lives in `src/xcore`
and `src/xgui`. So this project links the core directly into an MCP server and reimplements
the thin layer the GUI normally provides around it.

```
                            xspeccy-mcp (this repo)
   ┌──────────────────────────────────────────────────────────────────┐
   │  xsp_mcp.cpp        the command line and start-up, and nothing    │
   │                     else                                          │
   │  xsp_server         JSON-RPC 2.0, the tool registry, dispatch     │
   │  xsp_args           the argument rules every tool shares          │
   ├──────────────────────────────────────────────────────────────────┤
   │  tools/machine      what machine this is, and changing it         │
   │  tools/execution    frames, steps, stopping                       │
   │  tools/memory       registers, memory, assembly both ways         │
   │  tools/debug        breakpoints and the trace ring                │
   │  tools/video        the picture, and recording it                 │
   │  tools/source       symbols, listings, which lines ran            │
   │  tools/timing       where the T-states and the beam went          │
   │  tools/audio        AY registers and what the mixer produced      │
   │  tools/input        the keyboard as a person would use it         │
   │  tools/media        snapshots, tapes, disks                       │
   ├──────────────────────────────────────────────────────────────────┤
   │  xsp_machine        machine, execution, breakpoints,              │
   │                     disassembly, assembly, profiling,             │
   │                     frame cost, instruction timing                │
   │  xsp_video          geometry, palette, frame capture,             │
   │                     screen digest (md5)                           │
   │  xsp_blend          frame history and linear-light averaging      │
   │  xsp_settings       every changeable emulator setting, one row    │
   │                     each, with where its value came from          │
   │  xsp_audio          sampling, AY decoding, WAV                    │
   │  xsp_record         GIF encoder, colour table, ffmpeg pipe        │
   │  xsp_labels         symbol tables                                 │
   │  xsp_listing        source line mapping                           │
   │  xsp_keyboard       key matrix                                    │
   │  xsp_config         reads Xpeccy's own configuration              │
   │  xsp_platform       Linux / macOS / Windows                       │
   │  xsp_png            PNG via zlib                                  │
   └──────────────────────────────────────────────────────────────────┘
                              │ links
                              ▼
             libxpeccy  (upstream, compiled in place)
```

The three layers are worth keeping apart, and were not always. Until the split,
all of it lived in one 3036-line `xsp_mcp.cpp` whose `registerTools()` was a
single 2354-line function holding every handler as an anonymous lambda. Nothing
in there had a name, so nothing in there could be called from a test, and every
one-line change rebuilt the lot.

Now the protocol does not know what the tools are: `dispatch()` takes a request
and returns the reply rather than printing it, and `serve()` is the only thing
that touches a stream. That is what makes a wrong method, an unknown tool, a
tool that throws and a client sending CRLF into things a unit test can check
instead of things that need a process and a pipe.


**The upstream repository is never modified and never vendored.** CMake compiles
`$XPECCY_SRC/src/libxpeccy/*.c` into a static library from wherever the sources happen to
live, so upstream can be updated, moved or swapped without touching a line of this repo.

That makes the sources a build-time dependency. They must not become a run-time one, and
xpeccy-plus keeps part of what a machine *is* in data files rather than code: the machine
definitions, the layouts and the ROMs. `cmake/builtin_resources.cmake` compiles them in.
At build time, not configure time, because reading 1.8 MB through `file(READ HEX)` costs
over a second and configure reruns on every CMakeLists edit, it runs
`cmake/embed_resources.cmake` to write `generated/xsp_builtin_data.cpp`: `res/machines/*.conf`
and `res/layouts.conf` always, `config/roms/*.rom` unless `XSP_BUILTIN_ROM` is off. The
arrays are byte lists, because MSVC caps a string literal at 64K. `src/xsp_builtin.h` is
the whole interface (`builtin::kMachines`, `kLayouts`, `kRoms`). Like `builtin_rom.cmake`
before it, this is generated, never committed, and read rather than copied - the same rule
as the core itself.

The compiled-in data takes the place of the shipped layer only. The user's files still win:
a ROM of the same name under `<confDir>/roms`, a `machines/<id>.conf` patch, a `layouts.conf`.
`RomFile::data` is how the loader tells a compiled-in ROM from a file, and `Env::load()`
reports which one in its startup note. Only a build with `XSP_BUILTIN_ROM=OFF` still looks in
the source tree for ROMs (`<XPECCY_SRC>/config/roms`), and a binary built that way loses
them when it is moved.

## What the GUI does that we had to redo

The core alone will not boot a machine. `xcore`/`xgui` supply five things, and each of them
had to be reimplemented without Qt:

1. **Machine configuration** - pick hardware, clamp the RAM size to what that hardware
   supports, load the romset. Mirrors `xcore/profiles.cpp`.
2. **Machine definitions** - the fork's `res/machines/*.conf` files carry memory size,
   CPU clock and turbo table, contended memory/IO, snow, floatbus, border minimum, disk
   interface, IDE, drive count, PSG count/type/stereo, Soundrive/GS/SAA, input devices,
   keyboard scan table, ROM/GS-ROM/font files, cold-RAM pattern and noise, and the layout
   name. The shipped files are the compiled-in `builtin::kMachines`.
   `xsp_config.cpp` builds them exactly as `xcore/machines.cpp:mac_build()` does:
   `mac_defaults`, then the inherited parent *as it ships* (a user's patch on the parent
   must not move its children), then the shipped file's keys, then the user's same-stem
   file as a patch - never a replacement. A `MachineDef` is therefore complete, and
   `applyMachineDef()` assigns every field: a key the file does not carry falls to the
   default, not to whatever the previous machine had set. `setModel()` also resolves a
   definition stem ("zxplus2", "evo-tsconf") to its hardware, because several stems can
   share one - `machine_state` reports which one applied as `machine`. The one upstream
   edge kept deliberately: a hand-written `inherit` pointing at a user-only parent gets
   the parent's stock form (the parent's own keys never cross), because the GUI only ever
   produces self-sufficient child files - `xm_save_as` writes a materialized diff and
   uses `inherit` for navigation.
3. **Romset loading** - read the emulator's `config.conf`, load each ROM file at its offset,
   size the ROM space. Mirrors `prfSetRomset()`. A machine definition's own `romN` lines
   become a `machine:<id>` romset. Rom files resolve through `romPath()` to the user's
   `roms/` first, then to the compiled-in copy of the shipped `config/roms/` - the
   headless equivalent of upstream copying its roms into the config directory at first
   run (`config.cpp` seedRom). The GS firmware and the text font are found the same way. If a
   definition's roms fail, the configured romset (or the built-in ROM) takes over so the
   machine still boots on something.
4. **Video geometry** - a layout (`vLayout`), the border mode, and the globals
   `bytesPerLine`, `xstep`, `ystep` and the skip counters. Mirrors `vid_upd_scale()` and
   `MainWin::updateWindow()`.
5. **Palette** - 16 colours into `vid->pal`. Without this every frame renders black, because
   the core never fills the palette itself.

Because all five come from the emulator's own configuration files, naming a model gives you
the machine the GUI would have booted - same romset, same RAM, same geometry, same timings.

## Execution model

One thread, synchronous, no real time. `Machine::execLoop()` calls `compExec()` in a loop and
decides when to stop; everything else - profiling, coverage, tracing, audio sampling - hangs
off that loop as instrumentation rather than as hooks into upstream code.

That has a pleasant consequence: emulation runs as fast as the host can manage instead of at
the machine's own speed, and runs are deterministic. There is no window, no audio device and
no display server involved. Measure it before planning around it, though - 1000 frames is
about 2 seconds of wall time on the machine this was last measured on, against the 20 seconds
they represent, so roughly ten times faster than the Spectrum and not "instant". Ten minutes
of emulated time is some 30 000 frames and a minute of real waiting, which is longer than
most MCP clients will wait for one tool call.

```
  execLoop:
      pcBefore = PC
      ns       = compExec()          ← one opcode, devices synced
      account(pcBefore, ns)          ← profiler, trace, audio sampling
      coverPc(pcBefore, PC)          ← coverage
      if frmStrobe: frames++         ← frame boundary
      stop on breakpoint / pc / frames / budget
```

`account()` also decides whether an arrival in a profiled range is a *call*: it looks at the
opcode of the instruction that handed control over, and a `ret` means a callee coming back
rather than a new invocation. The read happens only on the transition, a few thousand times a
frame rather than a few hundred thousand, so per-instruction cost is unchanged.

`frameCost()` runs the same loop for a different question - it stops on a frame boundary and
splits the time into work and the HALT wait - and feeds the same instrumentation, so one pass
can measure a frame and profile it at once. The partial frame it runs to reach the first
boundary is deliberately not instrumented.

Because the accounting is per instruction and the T-states come from dividing by the core's
own quantum, the profiler's per-range numbers are exact rather than sampled. Cross-checked
against an independent static cycle count of a Pentagon demo's frame: 97 809 T measured
against 97 810 counted for its render, and 22 459 against 22 459 for its attribute packing.
That is the accuracy an effect's frame budget needs.

## Things the core does that are easy to get wrong

Each of these cost time to find, so they are worth writing down:

- **`compExec()` returns nanoseconds, but the exact T-state count lives elsewhere.**
  The cpu counts T into `cpu->t` as it executes (contention waits land there mid-instruction),
  `cpu_exec` returns that count, and `comp->tickCount` accumulates it. The ns that comes back
  is `vid->time` - derived through the video clock - so the T-count is recovered from the
  `tickCount` delta across the call, and the profiler, `RunResult` and `FrameCost` all store
  T-states rather than ns to divide back through. The `ns` keys the tools report are the
  per-call nanoseconds `compExec()` returned, accumulated across the run - a turbo switch
  mid-run is accounted at the rate that was in effect at the time, not recounted at the end.
- **`comp->nsPerTick` is a double in the fork** - the precise `1000/cpuFrq` (285.714 at
  3.5 MHz), where upstream kept a truncated and even-forced int. Upstream's truncation was a
  real bug: tick and dot periods rounded separately slipped the Pentagon's frame interrupt
  across the dot grid roughly every 130 frames, which showed as the border jumping 4 px every
  few seconds. The fork derives `nsPerTickFixed` (16.16) from `nsPerDotFixed` so a tick is
  exactly two dots, and the note at `spectrum.c:comp_update_timings` says why. What survives
  of the trap on our side: copying `nsPerTick` into an `int` truncates it to 285 and every
  reported T-count comes out ~0.25 % high - which is exactly what dividing stored ns by a
  truncated nsPerTick used to produce.
- T-states for the current frame are in `comp->frmtCount`; the last full frame is
  `comp->fCount`. Do not take a delta of `frmtCount` across `compExec` to time an
  instruction: the interrupt resets it mid-instruction. `tickCount` has no reset.
- **There is no frame rate in the core.** `comp->fps` used to hold one, a constant `zx_init()`
  set to 50 for every ZX machine whatever the geometry was; `0.6.20260804` deleted it. The
  honest rate is `cpuFrq / t_states_per_frame`, 48.83 Hz on a Pentagon, and that is the only
  one reported now, as `fps_real`.
- **A frame boundary is `comp->flgFRM`,** a flag the caller must clear itself. It and its
  neighbours `flgBRK` (a breakpoint was hit), `flgDBG` (ignore breakpoints) and `flgBRKMEM`
  (consult the breakpoint maps at all) are macros for `sysflag[]` entries.
- **Writing a breakpoint map does not arm it.** `compExec()` only tests `brkXMap`/`brkIOMap`
  when `flgBRKMEM` is set; the flag is the GUI's job, and a caller that pokes the maps
  directly gets breakpoints that never fire - which is exactly what the first port did.
  `Machine` sets it when a breakpoint goes in and clears it when the last one comes out.
- **Some state is derived, not stored.** A field write that reads back fine can still do
  nothing, because the behaviour lives behind a hook the core's setter installs:
  `flgCNTM` reports contention while `comp_set_cont()` alone wires `cpu->flgCONT`/`xcont`;
  `aymChip::type` names the chip while `chip_set_type()` alone routes the vol/rd/wr
  callbacks; `aymChip::frq` reports the frequency while `ts_set_frq()` alone recalculates
  the `tickFx` quantum the mixer actually ticks at, for all three chips and with the
  type's clock multiplier (a YM2203 runs at twice the AY's). `ulaPlus::conttype` names the
  pattern while `zx_set_vmode()` picks the drawer that fetches early or not. The settings
  table calls the setters, and a sound setting covers every chip on the board, as in the
  GUI.
  The test is on the hook - `cpu->flgCONT`, `a->vol != ay_vol` - never on the flag.
- **A dot is not a pixel.** The frame buffer holds 8 bytes per dot and the draw callback
  decides what fills it: `vid_dot_full()` writes one colour twice, `vid_dot_half()` writes
  one pixel and advances 4 bytes, so in the text modes (VID_TSL_TEXT, VID_ATM_TEXT,
  VID_EVO_TEXT) and in the modes drawn through `vidDrawByteDD()` (VID_ATM_HWM,
  VID_PRF_MC, VID_PRF_MONO) the pair carries two different pixels and the shown width is
  `vsze.x*2` - the same width the upstream GUI uploads. `xsp_video::pixelScale()` answers
  which of the two layouts the current `vmode` is drawing; reading only the first pixel of
  a pair silently halves a text screen. A capture does not ask the current mode, though:
  the program may have switched since the frame was drawn. Every frame boundary records
  the mode the frame was drawn in (the wider of the one it started and ended in), and
  captures, digests and `screen.width` read the frame in that mode. The paper area for
  `border: false` is per mode as well: the ATM/Evo modes draw 320x200 and Profi 256x240
  dots around the layout's ZX screen.
- **A file can name its machine.** `xcore/filemachine.cpp`'s `fm_pick` assigns a hardware
  to each media format (`.spg` -> TSLab, `.trd` -> Beta Disk, `.dsk` -> +3DOS) and a
  snapshot to the machine it was saved on (`snaGetHardware`/`z80GetHardware` +
  `snapHwRuns`). `load_file` does the same through `machineForFile()` - stem first,
  hardware name second - before calling the loader, because the loaders themselves do
  not switch hardware (`loadSPG` only remaps memory on the current one).
- **The machine list is `tabHwItem tabHwPtr[]`,** terminated by `id == HW_NULL`, and entries
  whose `core` is NULL are separators for the GUI's menu rather than machines. Upstream
  stopped declaring it in `hardware.h` - the GUI declares it for itself in `setupwin.cpp`,
  and `xsp_machine.cpp` does the same. The older `hwTab[]` it replaced is still in the file
  but behind an `#if 1 / #else` that never compiles.
- **`cpuGetRegs()` does not clear the bunch it returns.** It fills one entry per register the
  CPU has and marks the end by setting `id = REG_EOT` on the next one - and nothing else on
  it, so that entry's `name` is whatever was on the stack. Walking the array until `name` is
  NULL therefore dereferences garbage: it read as an intermittent segfault in `get_registers`,
  about half of runs, and under a debugger (which turns ASLR off) it would not reproduce at
  all. Stop on `REG_EOT`. The previous release pre-filled all 32 entries, which is why the
  name test used to work.
- **The frame buffer is the whole raster, two pixels per dot.** In the fork `bufimg` is the
  beam-height x dot-clock bitmap, 2 px per dot horizontally; the tools' frame is the visible
  window cropped out of it and decimated back to one pixel per dot (`xsp_video.cpp`). Reading
  the buffer raw doubles the width and includes the blanked area - a screenshot that looks
  "wrong, wider" is an unnormalised one.
- **Determinism is one variable:** the core seeds cold-RAM noise, the CMOS clock and the
  power-on phase from `time(NULL)`; `xhost_time_fixed` replaces it. `Machine::init()` pins it
  to a constant, so a machine definition with `ram.noise` produces the same bytes on every
  host and every run. One leak remains: `atm2inFE` (the ATM2's keyboard clock) reads
  `time()`/`localtime` directly on every port $FE read - on that machine the guest sees
  wall-clock seconds. The fix belongs in the core.
- **`comp->tickCount` is a signed int that wraps.** It is the exact T counter (cpu_exec
  charges whole instruction costs into it, contention waits included), and at 3.5 MHz it
  crosses `INT_MAX` roughly ten minutes of emulation in (2^31 / 3.5e6 ≈ 613 s). Every delta across a
  `compExec()` call is taken as an unsigned difference, so the wrap stays a small positive
  number; a signed difference would come out as a two-billion-step regression.
- **`compExec()` can return without executing.** A fetch breakpoint, and the irq break
  (`flgIBRK`), are checked before the opcode runs: the call returns 0 and sets
  `flgBRKPRE`. A loop that counts iterations as instructions would count one that never
  ran - so would the profiler and the coverage map. `execLoop` checks the flag before it
  accounts anything; `step`/`frameCost` never see it because they run with `flgDBG` set,
  which suppresses the check entirely.
- **The Pentagon's IM1 goes through RAM at `$5B00`.** The 128K rom's interrupt handler
  jumps to a DOS-paging trampoline the boot code writes there. On cold RAM that address
  holds the power-on pattern - on a Pentagon `ram.cold` means `$FF` bytes, which execute
  as `rst #38`: an interrupt lands on it and recurses forever instead of returning. It is
  authentic hardware behaviour, not a core bug - but any test that enables interrupts
  must boot the machine first, or the flicker is an rst loop.
- **A Pentagon does not boot into BASIC.** `resbank` defaults to `RES_128` in the
  definitions, so a reset lands on the 128K menu, not "1982 Sinclair". 48 BASIC is three
  cursor-downs and an Enter away, plus the self-test it then runs. A machine that *does*
  boot straight into BASIC is one whose definition says `reset = basic48`, like zx48.
- **`loadSPG` trusts the file completely** - a 16-bit block count walks a fixed 256-entry
  descriptor table, payload `fread`s go unchecked, and its depackers read a compressed
  stream up to 64K wide against a 16K buffer (`bbstream` reads `len` bits from a `base`
  the caller picked), so a hostile compressed block reads out of bounds. `checkSpgFile`
  re-walks the structure and decodes every packed block through bounded mirrors of the
  upstream depackers, so a bad file is refused before the core sees it.
- **The `.td0` and `.dsk` parsers are just as trusting.** `.td0` reads a header, then
  loops on track records it can never see end (the stream reader reports EOF as success)
  and decompresses declared sizes into fixed buffers; `.dsk` indexes a 29-entry sector
  table with a declared count of up to 255 and copies extended-format track lengths into
  a 6144-byte sector buffer. `checkTd0File`/`checkDskFile` walk both formats the same
  way the core does - every offset proven against the actual file length first - on
  `load_file` and on `disk insert`.
- **The SD core's capacity math is 32-bit.** `sdcSetImage` sizes the card with a signed
  `ftell` and a doubling loop: at 1 GiB the shift overflows (between 1 and 2 GiB the loop
  can spin forever, a 2 GiB image reports capacity 0). The `sdcard` tool rejects images
  `>= 1 GiB` itself, and `sdcReset` runs on every mount/eject so a swapped card does not
  inherit a transfer the old card was in the middle of.
- **Without a palette every frame is black.** `vid->pal` starts zeroed.
- **`compCreate()`/`compReset()` leave the key matrix zeroed,** which the hardware reads as
  *every key held down*. `kbdReleaseAll()` after every reset, exactly as the GUI does.
- **A new machine has no disk controller.** `compCreate()` calls `difCreate(DIF_NONE)` and
  leaves it there, because upstream expects the GUI to set one from the profile. A reset into
  TR-DOS then runs a ROM whose drive never answers: it hangs before it clears the screen, and
  the symptom is a screenful of uninitialised RAM rather than an error. `setModel()` calls
  `difSetHW()` with the interface the hardware actually shipped with - Beta Disk for the ZX
  clones, uPD765 for the +3, nothing outside `HWG_ZX`.
- **Nothing in libxpeccy sets the CPU type.** It comes from the profile (`cpu.type`), so a
  machine picked with `compSetHardware()` alone keeps whatever CPU the last one had.
  `setModel()` takes it from the machine definition, and refuses a model the headless core
  cannot run rather than letting the server segfault mid-session. The fork dropped the
  non-ZX half of the table (GameBoy, NES, C64, BK, Specialist, PC-9801), so what upstream
  refused for its NULL IO handlers is now simply an unknown name.
- **The core prints to stdout,** which for a stdio JSON-RPC server is the protocol stream.
  There are bare `printf`s left in `libxpeccy` - debug leftovers in the file-type loaders
  and the disk interface. Nothing on a ZX path reaches one: the whole smoke suite, disks
  included, produces a clean stream.
- **Breakpoints are bits in the memory cell** (`MEM_BRK_FETCH/RD/WR` in `cartridge.h`),
  reached through `getBrk`/`setBrk`, so they follow the bank rather than the address.
- **`comp->debug` means "ignore breakpoints"** - needed to step off a breakpoint you are
  standing on, or the run stops on the spot forever.
- **The built-in assembler takes decimal and `0x`,** but not `$` or `#`. `xsp_machine.cpp`
  normalises the notation, and substitutes labels, before handing a line to `cpuAsm()`.
- **`BB:OOOO name` in a sjasmplus symbol file is a bank and an offset inside it,** not a CPU
  address. `xsp_labels.cpp` maps it the way `xcore/labels.cpp` does - bank 5 to `$4000`,
  bank 2 to `$8000`, the rest to `$C000`, `FF` straight through. Reading the number as an
  address silently moves every label in a banked build.
- **`opCode.t` is not the instruction's T-states.** It is the base; every memory read or write
  the opcode performs adds three more as `z80_mrdx`/`z80_mwr` run, so `ld (hl),a` is 4 in the
  table and 7 on the machine. Summing the table gives wrong answers for exactly the operand
  forms you care about, which is why `instrTiming()` executes the instruction in a sandbox CPU
  (`cpuCreate` with its own 64K and dead I/O) and reads back what it charged. Conditional
  instructions run twice, once with every flag set and once with none - which covers both
  outcomes of all eight Z80 conditions - with B set so `djnz` branches the same way.
- **HALT is `PC--`, executed over and over.** `flgHALT` goes up on the first execution and the
  interrupt clears it with `PC++`. So "the CPU is waiting" is not one long instruction but
  thousands of 4 T ones, and a frame boundary is the 0-to-1 transition, not the opcode.
- **The core must be compiled at `-O1` or higher.** At `-O0` the GameBoy CPU core fails to
  link: `lr_swaph()` is declared `inline` without `static`, so no out-of-line copy is emitted.
- **A current GCC will not compile it without `-fpermissive`.** GCC 14 turned a family of old
  C looseness into errors rather than warnings, so `-w` no longer reaches them, and on Windows
  `cpu/cpu.c` trips one: it assigns the `FARPROC` that `GetProcAddress()` returns (through the
  `dlsym` shim) straight to a `cpuCore*(*)()`. `cmake/xpeccy.cmake` adds the flag for GCC.

## How it is tested

Two suites, because there are two questions and neither answers the other.

`tests/smoke.sh` starts the built server and talks JSON-RPC to it down a pipe,
exactly as a client would. It is the only thing that can say the parts are wired
together, and it is a poor way to say whether any one of them is right: reverse
the order of the frame history so that "the frame before last" means the wrong
frame, and every one of its checks still passes, because a two-frame blend comes
out the same colour either way round.

`tests/unit/` is where that gets caught. It links every module the server is
built from except the tool registrations, so a check can call a function by name
instead of inferring it from an answer. Some of it is written against formats
nothing else validates: the GIF suite decodes what the recorder wrote, LZW and
growing code widths and table resets included, and compares the pixels back to
the ones that went in. Some of it needs a real machine, and builds one - the
settings table is a claim that every changeable emulator setting lives in one
row, and half of that claim is each row's get and set actually reaching the core,
which nothing but a running machine can check.

The harness has no framework behind it: this project vendors one dependency, and
a second bought nothing that `CHECK`, `CHECK_EQ` and a self-cleaning temporary
directory do not.

`tools/coverage.py` builds an instrumented tree, runs both, and adds them up. The
number it prints is a map and not a score: what it is for is the bottom of the
table, which is how `xsp_keyboard.cpp` - pure, needing no emulator, and at 40% -
came to be noticed at all. It fails below a floor so that a change which guts the
tests fails rather than quietly lowering a number nobody reads.

## Dependencies

zlib, for PNG compression, and a C++17 compiler. That is the whole list. PNG, WAV and GIF are
written here rather than pulled in as libraries - a PNG is a few chunks, a WAV is a 44-byte
header, and GIF's LZW is a page of code. `ffmpeg` is optional and only used for MP4/WebM
recording.

At run time the list is meant to be empty, which takes saying because it is easy to lose. The
core is a static library and the ROM is compiled in, so what is left is the C and C++
runtimes and zlib itself. On MinGW those are three DLLs out of the MSYS2 tree - `libgcc_s_seh-1`,
`libstdc++-6`, `zlib1` - and a machine without MSYS2 has none of them, which is why `XSP_STATIC`
defaults to on there and links them in. The result imports `KERNEL32` and `msvcrt` and nothing
else. Elsewhere a fully static link is not the norm and the option defaults to off.

## Platform layer

`xsp_platform.cpp` holds everything that differs between systems: home and temp directories,
where Xpeccy keeps its configuration, and Windows' habit of rewriting `\n` on stdout, which
would corrupt a line-delimited JSON-RPC stream. That one is real and measured, not defensive:
a `\n` written to stdout on MinGW-w64 arrives as `\r\n` unless `_setmode()` has said otherwise.
The same applies to the pipe that feeds ffmpeg, which carries raw RGBA - `xsp_record.cpp`
opens it `"wb"`, because in text mode 300 bytes of frame go in and 302 come out.

On Unix the config path is `$HOME/.config/samstyle/xpeccy` - built exactly as
`xcore/config.cpp` builds it, `XDG_CONFIG_HOME` deliberately ignored, because that is what the
emulator itself does. On Windows the GUI keeps its configuration next to its own executable,
which cannot be guessed, so `--config` exists. Either way the romsets are read from a `roms`
subdirectory of that path, which is where `xcore/profiles.cpp` looks for them.

Windows needs MinGW-w64 rather than MSVC: the core is GNU C, built upstream with `-std=gnu99`.
It is built and smoke-tested there; the one thing to know when testing by hand is that the
server is a native program, so every path it is handed - `--config`, and every tool's `path`
argument - is a Windows path, not the `/c/...` an MSYS2 or Git Bash shell prints. `smoke.sh`
converts its own temp paths with `cygpath -m` for exactly that reason.
