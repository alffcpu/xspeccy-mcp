# Third-party notices

A built `xspeccy-mcp` is not only this repository's code. It statically contains
an emulator core with its machine definitions, a JSON library, a compression
library and the ROM images the core ships, none of which are ours. Each is listed below with what it is, where it comes
from, and the notice its licence asks to be carried with a distributed binary.

This file travels with the source. Anyone shipping a compiled binary should ship
it too, or the equivalent notice alongside.

## Xpeccy (libxpeccy)

- Upstream: https://github.com/dotkoval/xpeccy-plus, a fork of
  https://github.com/samstyle/Xpeccy
- Author: SAM style, and the xpeccy-plus contributors
- Licence: MIT (`LICENSE_eng` in the fork)
- Version: the commit pinned in [`VERSIONS`](VERSIONS) as `XPECCY_COMMIT`

`src/libxpeccy` is compiled into a static library and linked into the
executable. The sources are fetched unmodified at build time and are never
copied into this repository or patched; `build.py` verifies them against the
SHA-256 in `VERSIONS` before building. Only the core is used - the Qt layers
`src/xcore` and `src/xgui` are not compiled. The fork's machine definitions
(`res/machines/*.conf`) and screen layouts (`res/layouts.conf`) are compiled
into the executable as data, under the same licence.

```
Copyright (c) 2009-..., SAM style

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The full text is `LICENSE_eng` in the Xpeccy sources.

## nlohmann/json

- Upstream: https://github.com/nlohmann/json
- Author: Niels Lohmann
- Licence: MIT
- Version: 3.12.0, vendored at [`third_party/json/json.hpp`](third_party/json/json.hpp)

The only vendored dependency in this repository. Its licence text is kept beside
it at [`third_party/json/LICENSE.MIT`](third_party/json/LICENSE.MIT):
Copyright (c) 2013-2026 Niels Lohmann.

## zlib

- Upstream: https://zlib.net/
- Authors: Jean-loup Gailly and Mark Adler
- Licence: zlib licence

Used for the deflate stream inside every PNG the server writes. Normally linked
dynamically from the host; `-DXSP_STATIC=ON` (the default on MinGW, so that a
copied `.exe` runs on a machine without MSYS2) links it into the executable, and
a binary built that way carries zlib's code.

```
This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use
of this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## The ROM images

- Files: `config/roms/*.rom` in the xpeccy-plus sources, about 1.8 MB: the
  Sinclair/Amstrad 48K, 128K, +2 and +3 ROMs, Pentagon, TR-DOS and GLUK,
  Scorpion, Profi, ATM, ZX Evo/TSConf, the General Sound firmware and the
  `sgen.rom` font
- Copyright: each image stays with its own rights holder; the Sinclair images
  with Amstrad plc, who acquired the Sinclair computer copyrights in 1986
- Terms: `config/roms/LICENSE` in the fork, which states that the images are
  **not** covered by the emulator's MIT licence; `config/roms/PROVENANCE.md`
  lists every image, where it came from and its status, and
  `config/roms/AMSTRAD.copyright` carries the canonical Amstrad notice. Those
  three files are not compiled in; read them in the fork

`cmake/builtin_resources.cmake` compiles these images into the executable, so
the linked binary boots every machine on its own ROMs on a host that has
neither an Xpeccy tree nor an Xpeccy configuration. **A built binary therefore
contains the ROM images.** A file of the same name in the host's configuration
still wins, so a host with its own romset uses it. Configure with
`-DXSP_BUILTIN_ROM=OFF` to leave all of them out, and the server will then need
ROMs from the host.

The fork's terms for the images are that they are not sold on their own, not
embedded into hardware, and that the copyright messages inside them are kept;
none of the bundled images is modified, here or in the fork.

Amstrad have given permission for the Sinclair ROM images to be redistributed
with emulators, on the condition that the copyright notice is retained and they
are not sold. That permission is theirs and not ours to grant onward; anyone
redistributing a binary built with the ROMs compiled in should satisfy themselves
that their use is within it.
