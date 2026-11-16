# Own app (study) -- build-only skeleton

Feasibility study for replacing the stock `yy_cam` of the Rotatable 2K with an Akam app that links the camera's own
vendor libraries. Read [own-app-feasibility.md](../../../docs/porting/lsc-rotatable-2k/own-app-feasibility.md)
first. **Nothing here runs on a device and nothing here is shipped**: no card, kit or release uses this directory.

| File | What |
|---|---|
| `toolchain.lock` | pin (URL + sha256) of the Bootlin uClibc ARM926 soft-float toolchain, separate from the 1080p zig/musl one |
| `fetch_toolchain.sh` | download + verify + extract it into `tools/.toolchain/` (project-local, git-ignored) |
| `prepare_vendor.sh` | extract the vendor `.so` from **your own** backup (`mtd3.bin`) into `dist/rot2k-app/vendor/` (git-ignored) |
| `poc0_versions.c`, `Makefile` | PoC-0: print the vendor library versions (no vendor header, own prototypes only) |
| `poc2_frame.c` | PoC-2: one H.264 stream from the sensor to a file (hardware-validated, both VI channels) |
| `akam2k_*.c/.h`, `make poc3` | PoC-3: `akam2k_rtsp`, both channels over RTSP (`/stream0`, `/stream1`, TCP interleaved); compiles the shared RTSP/SmolRTSP sources of `src/` unmodified (needs `SMOLRTSP=<checkout>`), see the feasibility doc |
| `poc4_detect.c` | PoC-4: run the stock human-detection model on the NPU (third VI channel, `ak_svp2_*`), print boxes/scores/timing; see [ai-own-app.md](../../../docs/porting/lsc-rotatable-2k/ai-own-app.md) |

No vendor binary, header, disassembly or sysroot file is committed. Only the PoC programs were run on hardware (see the docs); nothing here is shipped.
