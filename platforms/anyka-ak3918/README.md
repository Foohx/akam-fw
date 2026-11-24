# Platform `anyka-ak3918`: direct drivers

The program of the Anyka AK3918 cameras that Akam drives **without the vendor libraries**: raw kernel ioctls on the V4L2-style nodes
(`/dev/video-0-0`, `/dev/isp-param-0`, `/dev/venc`, the PCM nodes), a static non-PIE musl binary built with the pinned Zig.
Today one camera uses it: [`firmware/lsc-indoor-1080p-ak3918`](../../firmware/lsc-indoor-1080p-ak3918).

| | |
|---|---|
| SoC | Anyka AK3918AV130 (ARM926EJ-S, ARMv5TE, soft float); the platform name uses the family prefix, see [docs/socs.md](../../docs/socs.md) |
| libc, linking | musl 1.2, static, `-Wl,--gc-sections`; `time32_shim.c` replaces the time64 syscalls the 4.4 kernel does not run |
| Toolchain | pinned Zig (`tools/build/deps.lock`, fetched by `tools/build/bootstrap.sh`) |
| Kernel interface | raw ioctls, no vendor `.so` (the ioctl layouts are in `ak_ioctls.h`, `isp_nr_layout.h`) |
| Output | `akamd` (`ak_rtsp` in 0.1.x; the image keeps a compatibility link `ak_rtsp`), `flash_tool`, `isp_dump`, `isp_hook.so`, `stock_hook.so`, `patch_div0` |

## What is here

| Path | What |
|---|---|
| `Makefile` | the generic rules, flags, `repro` (reproducible, stripped, hash in `tests/golden/`), `firmware` (image) and `upload` targets; `make -C platforms/anyka-ak3918 all` or the root `make 1080p` |
| `main.c`, `rtsp_session.c`, `venc.c`, `vi.c`, `audio*.c`, `isp*.c`, `imgq.c`, `ae.c`, `night.c`, `leds*.c`, `ircut.c`, `video_ctl.c` | the drivers and the process glue (one thread per concern, kernel encode ring) |
| `control.c`, `talk.c`, `mqtt_svc.c`, `mqtt_ops.c`, `onvif_*.c` | the 8091 text protocol, the speaker channel, MQTT and the ONVIF services as this platform serves them (they read the encoder and ISP state, so they include platform headers) |
| `platform.h` | the shared constants and globals of the program |
| `tools/` | host-side and capture tools built by the same Makefile (`isp_dump`, `isp_hook`, `stock_hook`, `patch_div0`) |

What a **camera** adds is in `firmware/<model>/`: `model.mk` (features and the ordered object list), the registry rows and API binding,
the ONVIF service table, the image builder and the on-camera scripts. Shared code is in `src/` (see [docs/hal-architecture.md](../../docs/hal-architecture.md)).

A second camera with the same SoC and interface adds only `firmware/<new-model>/` and builds with
`make -C platforms/anyka-ak3918 MODEL=<new-model>`.
