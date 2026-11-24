# Platform `anyka-ak3918av130`: vendor libraries

The app of the Anyka AK3918AV130 cameras (the NPU and ISP parts are used through the **vendor's own shared libraries**, `libplat_*`, `libmpi_venc`,
`libak_nne`, ...). Today one camera uses it: [`firmware/lsc-rotatable-2k-ak3918av130`](../../firmware/lsc-rotatable-2k-ak3918av130).

| | |
|---|---|
| SoC family | Anyka AK3918AV130 (ARM926EJ-S, ARMv5TE, soft float, NPU) |
| libc, linking | dynamic uClibc 1.0.31 against the camera's own libraries (`LD_LIBRARY_PATH=/appfs/lib`); the camera's libc is the reference for `make poc3-symcheck` |
| Toolchain | Bootlin armv5 uClibc 2018.11 gcc 7, pinned in `toolchain/toolchain.lock` (`toolchain/fetch_toolchain.sh`) |
| Kernel interface | the vendor libraries (own prototypes in `abi.h`, no vendor header, no vendor binary in the repository: `toolchain/prepare_vendor.sh` extracts the `.so` from **your own** backup) |
| Output | `akamd` (`ak_rtsp` in 0.1.x; the image keeps a compatibility link `ak_rtsp`) and the helpers `akam-sntp`, `akam-mic`, `akam-led` into `build/<model-id>/` (published as a copy to the model's `PUBLISH_DIR`, `dist/rot2k-app/` for the 2K, by `make poc3-dist`) |

| Path | What |
|---|---|
| `Makefile` | the generic rules and flags, `poc3-dist` (stripped binaries + `BUILDINFO.txt` with the symcheck proof), `repro` support; or the root `make 2k` |
| `main.c`, `cap.c`, `stream.c`, `rtsp_session.c`, `audio.c`, `aring.c`, `detect.c`, `snap.c`, `image.c`, `ircut.c`, `hal_ptz*.c` | the vendor glue and the process (thread per RTSP client, capture, NPU detection, motor driver) |
| `akam2k_mqtt*.c`, `akam2k_onvif*.c`, `zlive.c`, `isptest.c`, `detpub.c` | per-platform glue that stays here until the registry merge |
| `abi.h`, `isp.h`, `ctype_compat.h` | the ABI declarations of the vendor calls and the ctype workaround for the camera's libc |
| `toolchain/` | `fetch_toolchain.sh`, `toolchain.lock`, `prepare_vendor.sh` |
| `probes/` | the three hardware probes of the study (`poc0_versions`, `poc2_frame`, `poc4_detect`), run by hand on a camera |

A camera adds `firmware/<model>/` (`model.mk`, the HAL descriptors and pins, settings, store, boot scripts, image builders, model sheet);
the shared code is in `src/`.
