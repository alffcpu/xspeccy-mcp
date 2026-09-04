# xspeccy-mcp

![AI assisted](https://img.shields.io/badge/AI-assisted-blueviolet)
![MCP](https://img.shields.io/badge/MCP-server-informational)
![License](https://img.shields.io/badge/license-MIT-green)

This project is meant to be used by AI agents. It works with any agent that supports MCP,
such as Claude Code or OpenAI Codex.

The simplest way to start is to give your agent a link to this repository and ask it to
download the project, build it, check that the build works, and then use the xspeccy MCP
server while it debugs your code for the ZX Spectrum and similar machines. Everything you
can debug by hand in Xpeccy, an agent can now debug for you.

As a proof of concept, a full ZX Spectrum demo was made this way:
[CPU, DOCKS, U.](https://github.com/alffcpu/cpu-docks-u-zx-demo). All of its debugging and
all of its builds went through this tool, and it did the job well.

There is a longer write-up on working this way, in
[English](https://alffcpu.github.io/xspeccy-mcp/en/) and in
[Russian](https://alffcpu.github.io/xspeccy-mcp/ru/): what to keep in the agent's notes, how
to plan the work, and how the usual edit, build, run and check loop changes once the agent can
drive the emulator itself.

An MCP server that lets AI agents run and inspect ZX Spectrum programs through the
[Xpeccy](https://github.com/samstyle/Xpeccy) emulator core. Xpeccy is by SAM style.

53 tools over one machine, driven from an agent: load a snapshot, tape or disk image, run and
step, set breakpoints by address or by bank, read and write memory, assemble and disassemble,
resolve labels and listing lines from sjasmplus output, profile a frame by T-states, count
coverage, read the screen as text or attributes or a hash, capture PNG/GIF/MP4, and inspect
the AY and the beeper. There is no window and no real-time throttle: it runs as fast as the
host manages, and the same program produces the same screen digest every time.

Several of those are for raster and multicolour code. `run_to_beam` runs until the beam reaches
a given line and dot, instead of stopping at an address. `beam_log` logs the beam position
every time a given address is executed, without stopping the machine, and reports how much it
varies between frames. `frame_digest` hashes the frame as drawn on screen, optionally per
scanline; `screen_digest` hashes screen memory, which does not change if only the raster
timing drifts, so it cannot catch that. Every stop also reports the beam position.

Two more are for pictures that are not meant to be looked at one frame at a time. A gigascreen
or a flickering multicolour is two or three pictures alternating at 50Hz, and a single frame of
one is half the picture: it looks like neither half, and judging it from a screenshot is how a
working effect gets declared broken. `screenshot` and `frame_digest` take `blend`, which
averages the last completed frames in linear light - the colour a person watching actually
sees - and `video_config` makes that the default. On a machine with two screens the tools also
know which of them the ULA is drawing, so `screen_attrs` describes the screen on air rather
than whichever bank happens to be mapped at `$4000`.

`settings` reports every emulator setting the server can change, where the current value came
from and what changing it costs, and changes one for the session or for good.

**Point your agent at [COOKBOOK.md](COOKBOOK.md) before it starts.** The tool descriptions say
what each call does; the cookbook says which calls to make for a given question, in what order,
and which of them answer questions that sound the same and are not.

- **[docs/TOOLS.md](docs/TOOLS.md)** - every tool, its arguments and what it returns
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** - how the emulator core is used without its
  GUI, what that GUI had to be replaced with, the traps in the core worth knowing before
  extending this, and how the whole thing is tested

## How it fits together

Xpeccy is a Qt application whose emulator core is not: `src/libxpeccy` is about 130 files of
plain C with no Qt, no SDL and no OpenGL in any of them. This project links that core directly
and reimplements the four things the GUI normally does around it - pick the hardware and clamp
the RAM to what it supports, load the romset, set the video geometry, and fill the palette,
without which every frame renders black.

The upstream repository is never vendored and never patched. CMake compiles it from wherever
its sources happen to be, `build.py` fetches them by commit rather than by tag - upstream
publishes tags that move - and checks them against a SHA-256 of the sources themselves, taken
over the files with line endings normalised so that a Windows checkout hashes the same as a
Linux one. The pinned commit, the release it belongs to, the oldest release that still
compiles and that hash all live in one file, [VERSIONS](VERSIONS), which the build and
`build.py` both read.

Inside, the JSON-RPC layer, the shared argument rules and the tools themselves are separate:
`src/xsp_server.*` turns a request into a reply without touching a stream, `src/xsp_args.*`
holds the rules every tool shares for reading a number, an address or a label, and
`src/tools/` is one file per subject. `docs/ARCHITECTURE.md` has the rest.

## Build

Requirements: Python 3, CMake 3.16+, a C++17 compiler, and zlib.

```sh
python build.py            # add --smoke to run the test suite against what it built
```

The script downloads Xpeccy, configures the project, and builds `build/xspeccy-mcp`
(`build/xspeccy-mcp.exe` on Windows). Use `python3 build.py` when `python` is not the Python 3
command.

Which Xpeccy is pinned in [`VERSIONS`](VERSIONS) - the one file any version is written down
in, read by both `build.py` and the CMake build. It is fetched **by commit** rather than by
tag name, because upstream also publishes tags called `stable` and `minor` and both are moving
pointers, and what arrives is checked against a hash of `src/libxpeccy` recorded there. A tree
supplied with `--xpeccy-src` is checked too but only warned about, since pointing at a patched
copy is a deliberate act. `xspeccy-mcp --version` reports the server version and the emulator
release it was built against.

**On Windows use MinGW-w64, not MSVC.** The emulator core is GNU C - upstream compiles it
with `-std=gnu99` - so Visual Studio cannot build it, and CMake will pick Visual Studio by
default if it is installed. The short way to a toolchain is MSYS2:

```sh
winget install -e --id MSYS2.MSYS2
C:\msys64\usr\bin\bash -lc "pacman -S --needed --noconfirm \
    mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-zlib mingw-w64-x86_64-make"
python build.py --generator "MinGW Makefiles"
```

`--smoke` needs a `bash`; on Windows either the MSYS2 one or Git Bash will do. `ffmpeg` on
the PATH is optional and only needed for MP4/WebM recording - PNG and GIF are built in.

There are two test suites, and they answer different questions. `tests/smoke.sh` drives the
built server over stdio exactly as an MCP client would, which is the only way to say the parts
are wired together and a poor way to say whether any one of them is right: reverse the order of
the frame history and every one of its checks still passes, because a two-frame blend is the
same colour either way round. `tests/unit/` is where that gets caught - the colour arithmetic,
the 128K bank map, the listing lookups, the PNG and GIF writers, the keyboard matrix, the
settings table applied to a real machine, the argument rules and the JSON-RPC layer. Some of it
is written against formats nothing else validates: the GIF suite decodes what the recorder
wrote, LZW and growing code widths included, and compares the pixels back to the ones that went
in.

`--smoke` runs both, unit first, and `ctest` runs the unit suite on its own.

`python3 tools/coverage.py` builds an instrumented tree, runs both suites and reports how much
of the source they reach, per file, weakest first, failing below a floor. Neither suite alone
answers that, which is why nothing added them up before it existed.

```
TOTAL                       91.6%    71.1%    83.1%    92.8%   (6414/7002 lines)
```

That is one run on one commit; the command is the claim rather than the number.

Builds on Linux, macOS and Windows.

The included `.mcp.json` already points at the built executable, so an agent working in a
clone of this repository finds the server without being told where it is. The path in it has
no `.exe` on the end, which is correct on Windows as well, because the client adds it.

For another MCP client, use `build/xspeccy-mcp` as the server command. It speaks JSON-RPC 2.0
over stdio, one message per line.

The server reads the emulator's own configuration when it can find one, so "give me a
Pentagon" produces the machine Xpeccy's GUI would boot - same romset, same RAM, same screen
geometry. Point it at that directory with `--config <dir>` (on Windows the configuration
lives next to `xpeccy.exe`). Without one it boots a Pentagon 128K from a ROM compiled into
the binary, so it works on a machine that has never seen Xpeccy.

## What it does not do

Writing files into a TR-DOS disk image, RZX playback, breakpoint conditions (`A == 5`) and
watch expressions.

`list_models` reports every machine the emulator core knows, which is more than this server
can run. Of the non-ZX ones, `MSX`, `MSX2` and `IBM PC` load and run, though no tool here
knows anything about their hardware. `GameBoy`, `NES`, `Commodore64`, `BK0010`, `BK0011M`,
`Specialist` and `PC-9801` are refused with a reason, and `machine_config` leaves the previous
machine running rather than taking the server down with it. The first six leave the hardware
table's IO handlers null while the CPU type comes from the profile, so headless they run on a
Z80 and the first `OUT` is a jump through a null pointer; the PC-9801's IO handlers print
unhandled ports straight to stdout, which here is the JSON-RPC stream.

Recording with sound holds a minute of audio. Past that the soundtrack stops and the picture
is cut to match, and the answer says so.

## License

This project is released under the MIT License; see [LICENSE](LICENSE).

A built binary contains more than this repository's code, and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) says what:
[Xpeccy](https://github.com/samstyle/Xpeccy), downloaded at build time and statically linked,
under its upstream MIT License and copyright notice for SAM style;
[nlohmann/json](https://github.com/nlohmann/json), also MIT
([notice](third_party/json/LICENSE.MIT)); zlib; and the 16K ZX Spectrum ROM, which is
somebody else's copyright and is compiled in by default so that a copied executable can still
boot a machine. Build with `-DXSP_BUILTIN_ROM=OFF` to leave the ROM out, and the server will
take one from the host instead.
