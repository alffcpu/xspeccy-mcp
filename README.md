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

51 tools over one machine, driven from an agent: load a snapshot, tape or disk image, run and
step, set breakpoints by address or by bank, read and write memory, assemble and disassemble,
resolve labels and listing lines from sjasmplus output, profile a frame by T-states, count
coverage, read the screen as text or attributes or a hash, capture PNG/GIF/MP4, and inspect
the AY and the beeper. There is no window and no real-time throttle: it runs as fast as the
host manages, and the same program produces the same screen digest every time.

Three of those are for raster and multicolour code. `run_to_beam` runs until the beam reaches
a given line and dot, instead of stopping at an address. `beam_log` logs the beam position
every time a given address is executed, without stopping the machine, and reports how much it
varies between frames. `frame_digest` hashes the frame as drawn on screen, optionally per
scanline; `screen_digest` hashes screen memory, which does not change if only the raster
timing drifts, so it cannot catch that. Every stop also reports the beam position.

**Point your agent at [COOKBOOK.md](COOKBOOK.md) before it starts.** The tool descriptions say
what each call does; the cookbook says which calls to make for a given question, in what order,
and which of them answer questions that sound the same and are not.

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

Tested on Linux (Debian bookworm, GCC 12) and Windows (MSYS2 MinGW-w64, GCC 16): both build
warning-free and pass all 112 checks, with identical screen digests and the same hash for the
emulator sources on either platform. macOS is implemented but has not been compiled - expect
small fixes on first contact.

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

## License

This project is released under the MIT License; see [LICENSE](LICENSE). It includes
[nlohmann/json](https://github.com/nlohmann/json), also under the MIT License
([notice](third_party/json/LICENSE.MIT)). [Xpeccy](https://github.com/samstyle/Xpeccy) is
downloaded at build time and remains under its upstream MIT License and copyright notice
for SAM style.
