# Third-party notices

Akam builds on, or ships, the following third-party work. Versions are the
ones pinned in `tools/build/deps.lock`.

## Action-Smart-Connect-Flasher (MIT)

Source: https://github.com/4ddict/Action-Smart-Connect-Flasher
(`install.sh` v0.3.2, commit b99baf2)

Four fixes in `src/ak_rtsp/` were ported from the patches that `install.sh`
applies at build time:

- `ae.c`: default AE `exp_max` lowered from 1039 to 2.
- `rtsp.c`: RTSP client socket kept blocking (no `O_NONBLOCK`).
- `night.c`: auto day/night thresholds NIGHT=3000 / DAY=1200.
- `night.c`: mutex-protected tuning and mode-change reset of the debounce/lock
  state (the `night_get_snapshot` / `g_mode_generation` logic).

```
MIT License

Copyright (c) 2026 LSC 3215672.2 Decloud Wizard contributors

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

## Dropbear SSH (MIT-style)

Source: https://matt.ucc.asn.au/dropbear/ (release DROPBEAR_2026.94)

The Dropbear binary built by `tools/dropbear/build_dropbear.sh` is included in
the firmware image. Its license (MIT-style, with notices for contributed code
and bundled libtom* components) is copied unmodified to
`dist/dropbear/LICENSE.dropbear` by the build script and must be kept with any
redistributed binary.

The image also carries, in `/usr/local/akam/`, the licence texts of the other
code linked into `ak_rtsp` and `dropbearmulti`: `LICENSE.musl` (musl's
`COPYRIGHT`, from the pinned Zig tarball), `LICENSE.smolrtsp`, and
`LICENSE.slice99`, `LICENSE.metalang99`, `LICENSE.datatype99`,
`LICENSE.interface99` (their macros are expanded into `ak_rtsp`), `LICENSE.zig`
(Zig's compiler-rt is linked into both binaries) and `LICENSE.akam` (the
repository's own MIT licence, covering `ak_rtsp`), plus this file as
`THIRD_PARTY_NOTICES.md` (it carries the flasher's MIT notice above). They are
copied unmodified from the pinned sources by `build_firmware.sh`, which fails
if one is missing, and are listed with their SHA-256 in `MANIFEST.txt`.

## SmolRTSP and its dependencies (all MIT)

Linked into `ak_rtsp` (SmolRTSP) or used as headers at compile time (the
other four). Each project's own `LICENSE` file carries the full text.

| Project | Version | Copyright | Source |
|---|---|---|---|
| SmolRTSP | commit 8ae3b51 | (c) 2020-2024 OpenIPC | https://github.com/OpenIPC/smolrtsp |
| slice99 | v0.7.8 | (c) 2020-2025 hirrolot | https://github.com/hirrolot/slice99 |
| metalang99 | v1.13.5 | (c) 2020-2025 hirrolot | https://github.com/hirrolot/metalang99 |
| datatype99 | v1.6.5 | (c) 2020-2025 hirrolot | https://github.com/hirrolot/datatype99 |
| interface99 | v1.0.2 | (c) 2021-2025 hirrolot | https://github.com/hirrolot/interface99 |

## Zig (MIT) - build tool only

Source: https://ziglang.org/ (version 0.14.1)

`zig cc` is used as the cross-compiler (`arm-linux-musleabi`). Zig is
downloaded by `tools/build/bootstrap.sh`, is not part of this repository, and
the compiler itself is not distributed with the firmware. The musl libc and the
compiler-rt that Zig links statically into the binaries are MIT licensed; their
notices ship in the image (`LICENSE.musl`, `LICENSE.zig`).
