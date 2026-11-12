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

## Go assistant (`tools/assistant`)

The native assistant binary links the Go modules below, downloaded unchanged by
`tools/build/bootstrap.sh` into `tools/.toolchain/go/mod` (not stored in git; each module has its
licence file in its directory there) and pinned by the checksums of `tools/assistant/go.sum`. Only MIT, BSD-3-Clause, Apache-2.0 and Unlicense (public domain) code is linked; GPL tools are never linked or
embedded (`ffmpeg` is only run when the user has it). Test-only code adds no module.

| Module | Version | Licence | Used for |
|---|---|---|---|
| golang.org/x/crypto | v0.58.0 | BSD-3-Clause | SSH client, ed25519 keys, known_hosts, ssh-agent |
| golang.org/x/sys | v0.50.0 | BSD-3-Clause | system calls (file locks, terminals) |
| golang.org/x/term | v0.48.0 | BSD-3-Clause | hidden password input, terminal detection |
| golang.org/x/text | v0.43.0 | BSD-3-Clause | width tables used by the terminal libraries |
| github.com/ulikunitz/xz | v0.5.17 | BSD-3-Clause | in-process decoding of the kit (.tar.xz) |
| github.com/hajimehoshi/go-mp3 | v0.3.4 | Apache-2.0 | in-process MP3 decoding (`akam say`) |
| github.com/mewkiz/flac | v1.0.14 | Unlicense (public domain) | in-process FLAC decoding (`akam say`) |
| github.com/mewkiz/pkg | v0.0.0-20250417130911-3f050ff8c56d | Unlicense (public domain) | helpers of the FLAC decoder |
| github.com/mewpkg/term | v0.0.0-20241026122259-37a80af23985 | Unlicense (public domain) | linked with the FLAC decoder (terminal helpers it does not use here) |
| github.com/icza/bitio | v1.1.0 | Apache-2.0 | bit reader of the FLAC decoder |
| github.com/jfreymuth/oggvorbis | v1.0.5 | MIT | in-process Ogg Vorbis decoding (`akam say`) |
| github.com/jfreymuth/vorbis | v1.0.2 | MIT | Vorbis decoder (oggvorbis) |
| github.com/jfreymuth/pulse | v0.1.3 | MIT | PulseAudio / PipeWire client for the built-in microphone (`akam talk`, Linux builds only) |
| github.com/charmbracelet/bubbletea | v1.3.10 | MIT | full-screen menu and widgets |
| github.com/charmbracelet/lipgloss | v1.1.0 | MIT | terminal styling |
| github.com/charmbracelet/colorprofile | v0.2.3-0.20250311203215-f60798e515dc | MIT | colour support detection (lipgloss) |
| github.com/charmbracelet/x/ansi | v0.10.1 | MIT | ANSI sequences (bubbletea, lipgloss) |
| github.com/charmbracelet/x/cellbuf | v0.0.13-0.20250311204145-2c3ea96c31dd | MIT | cell buffer (bubbletea) |
| github.com/charmbracelet/x/term | v0.2.1 | MIT | terminal helpers (bubbletea) |
| github.com/muesli/termenv | v0.16.0 | MIT | terminal environment (lipgloss) |
| github.com/muesli/ansi | v0.0.0-20230316100256-276c6243b2f6 | MIT | ANSI text helpers (bubbletea) |
| github.com/muesli/cancelreader | v0.2.2 | MIT | cancellable terminal input (bubbletea) |
| github.com/aymanbagabas/go-osc52/v2 | v2.0.1 | MIT | clipboard escape sequences (termenv) |
| github.com/erikgeiser/coninput | v0.0.0-20211004153227-1c3628e74d0f | MIT | Windows console input (bubbletea) |
| github.com/lucasb-eyer/go-colorful | v1.2.0 | MIT | colour maths (lipgloss) |
| github.com/mattn/go-isatty | v0.0.20 | MIT | terminal detection |
| github.com/mattn/go-localereader | v0.0.1 | MIT (stated in its README; the module has no licence file; linked only in Windows builds, `key_windows.go` of bubbletea) | locale-aware input (bubbletea) |
| github.com/mattn/go-runewidth | v0.0.16 | MIT | character widths |
| github.com/rivo/uniseg | v0.4.7 | MIT | Unicode segmentation |
| github.com/xo/terminfo | v0.0.0-20220910002029-abceb7e1c41e | MIT | terminfo database (bubbletea) |

The Go standard library and runtime (BSD-3-Clause, The Go Authors) are linked into every binary.

## Audio modules: copyright notices and licence text

The audio modules of the Go assistant (`akam say`, `akam talk`) carry these copyright lines. The Apache-2.0
modules are used unmodified under the licence text below; the Unlicense modules are public domain.

- github.com/hajimehoshi/go-mp3 v0.3.4: Copyright 2017 Hajime Hoshi, and The go-mp3 Authors (Apache-2.0)
- github.com/icza/bitio v1.1.0: Copyright 2016 Andras Belicza (Apache-2.0)
- github.com/jfreymuth/oggvorbis v1.0.5: Copyright (c) 2016 Johann Freymuth (MIT)
- github.com/jfreymuth/vorbis v1.0.2: Copyright (c) 2016 Johann Freymuth (MIT)
- github.com/jfreymuth/pulse v0.1.3: Copyright (c) 2019 Johann Freymuth (MIT)
- github.com/mewkiz/flac, github.com/mewkiz/pkg, github.com/mewpkg/term: released into the public domain (Unlicense)

The MIT licence text of the Go modules above is the one reproduced in each module's licence file; it
grants use, copying, modification and distribution provided the copyright line and the permission
notice are kept, and the software is provided "as is", without warranty.

### Apache License 2.0 (go-mp3, bitio)

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "{}"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright {yyyy} {name of copyright owner}

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```
