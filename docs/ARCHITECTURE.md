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

That makes the sources a build-time dependency. They must not become a run-time one, which is
what `cmake/builtin_rom.cmake` is for: at configure time it reads the 16K ROM out of the
upstream tree and writes it into the build directory as a C array, so the linked binary can
boot a machine on a host where neither that tree nor a configuration exists. It is generated,
never committed, and read rather than copied - the same rule as the core itself. The file
paths still win when they are there (`<confDir>/roms`, then `<XPECCY_SRC>/conf`), so a host
with the real romset uses the real romset; `Env::load()` reports which one in its startup
note. `RomFile::data` is how the loader tells the two apart.

## What the GUI does that we had to redo

The core alone will not boot a machine. `xcore`/`xgui` supply four things, and each of them
had to be reimplemented without Qt:

1. **Machine configuration** - pick hardware, clamp the RAM size to what that hardware
   supports, load the romset. Mirrors `xcore/profiles.cpp`.
2. **Romset loading** - read the emulator's `config.conf`, load each ROM file at its offset,
   size the ROM space. Mirrors `prfSetRomset()`.
3. **Video geometry** - a layout (`vLayout`), the border fraction, and the globals
   `bytesPerLine`, `xstep`, `ystep` and the skip counters. Mirrors `vid_upd_scale()` and
   `MainWin::updateWindow()`.
4. **Palette** - 16 colours into `vid->pal`. Without this every frame renders black, because
   the core never fills the palette itself.

Because all four come from Xpeccy's own configuration files, naming a model gives you the
machine the GUI would have booted - same romset, same RAM, same geometry.

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

- **`compExec()` returns nanoseconds, not T-states.** T-states for the current frame are in
  `comp->frmtCount`; the last full frame is `comp->fCount`.
- **Those nanoseconds are quantised and short.** `comp->nsPerTick` is `1000 / cpuFrq`
  truncated to an integer and then forced even (`zx_init()`), so 3.5 MHz is 284 ns rather
  than 285.714 - every `ns` the core reports is 0.6 % low, and `vid->nsPerFrame` with it.
  Dividing by `nsPerTick` gives exact T-states back, which is why the tools report T-states
  and derive real time from the clock instead (`timing()` in `src/tools/common.cpp`).
- **There is no frame rate in the core.** `comp->fps` used to hold one, a constant `zx_init()`
  set to 50 for every ZX machine whatever the geometry was; `0.6.20260804` deleted it. The
  honest rate is `cpuFrq / t_states_per_frame`, 48.83 Hz on a Pentagon, and that is the only
  one reported now, as `fps_real`.
- **A frame boundary is `comp->flgFRM`,** a flag the caller must clear itself. It and its
  neighbours `flgBRK` (a breakpoint was hit) and `flgDBG` (ignore breakpoints) are macros for
  `sysflag[]` entries; they were bitfields called `frmStrobe`, `brk` and `debug` until
  `0.6.20260804` collapsed them into the array.
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
- **The frame buffer is a pair of globals:** `scrimg` is being drawn, `bufimg` is the last
  complete frame, RGBA8888, `bytesPerLine` bytes per row. `bytesPerLine` is *not* derived
  from the layout automatically - the GUI sets it, so we set it.
- **Without a palette every frame is black.** `vid->pal` starts zeroed.
- **`compCreate()`/`compReset()` leave the key matrix zeroed,** which the hardware reads as
  *every key held down*. `kbdReleaseAll()` after every reset, exactly as the GUI does.
- **A new machine has no disk controller.** `compCreate()` calls `difCreate(DIF_NONE)` and
  leaves it there, because upstream expects the GUI to set one from the profile. A reset into
  TR-DOS then runs a ROM whose drive never answers: it hangs before it clears the screen, and
  the symptom is a screenful of uninitialised RAM rather than an error. `setModel()` calls
  `difSetHW()` with the interface the hardware actually shipped with - Beta Disk for the ZX
  clones, uPD765 for the +3, nothing outside `HWG_ZX`.
- **Half the hardware table has no IO handlers.** Game Boy, NES, C64, BK and the Specialist
  are memory-mapped machines, so their `out`/`in` entries are NULL - and `iowr()`/`iord()`
  call them without checking. What makes that reachable is the next trap:
- **Nothing in libxpeccy sets the CPU type.** It comes from the profile (`cpu.type`), so a
  machine picked with `compSetHardware()` alone keeps whatever CPU the last one had - a Z80
  in front of Game Boy or Specialist hardware, whose first `OUT` jumps through a null
  pointer. `setModel()` refuses the models that would die instead of letting the server
  segfault mid-session.
- **The core prints to stdout,** which for a stdio JSON-RPC server is the protocol stream.
  There are around 220 bare `printf`s across `libxpeccy` - debug leftovers in the file-type
  loaders, the disk interface and the less finished CPUs - and the count has barely moved
  between releases, so this is a standing property rather than a regression. Nothing on a ZX
  path reaches one: the whole smoke suite, disks included, produces a clean stream. `PC-9801`
  is the exception and is refused for it, printing every unhandled port read as it runs.
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
