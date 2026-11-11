# What Akam contains and where it lives

A map of the repository for people who want to read or change the code. To install Akam you only need
`./akam` (see the [README](../README.md)).

## Components

| Component | What it does | Where |
|---|---|---|
| `./akam` wizard | Guided PC-side setup (bash, English) | `akam`, `tools/wizard/` |
| `ak_rtsp` | RTSP/RTP H.264 + audio server and the 8091 control port (read-only from the network, writes from the camera itself or in debug mode) | `src/ak_rtsp/` |
| Native Linux build | Pinned Zig 0.14.1 and pinned SmolRTSP; `make` and `bash` only | `tools/build/` |
| Dropbear 2026.94 | Static multi-call binary, ed25519 public-key login only | `tools/dropbear/` |
| Camera-side scripts | SSH start, Telnet/FTP lockdown, hostname, stream password, debug mode of 8091 (`akam_debug.sh`), ONVIF on/off (`akam_onvif.sh`), time sync (`akam_ntp.sh`), Wi-Fi supervisor (`akam_net.sh`) | `tools/firmware_patch_templates/` |
| Image builder | Patches **your own** APP dump, repacks, checks, pads to 5 242 880 bytes | `src/ak_rtsp/build_firmware.sh`, `tools/image/` |
| Release kit | Prebuilt Akam binaries and scripts, signed ([kit](akam_v1_kit.md)) | `tools/release/` |
| SD card tool | Prepares, verifies and wipes the microSD cards (Linux) | `tools/sd/sd_card.sh` |
| Flash scripts | Camera-side flash and restore, SSH reflash driver | `tools/flash_scripts/` |
| Post-flash check | Read-only checklist script, run over SSH | `tools/hw_tests/akam_hwcheck.sh` |

## Layout

```text
.
├── akam                          # the wizard entry point
├── docs/                         # documentation (index: docs/README.md)
├── images/                       # hardware photographs and diagrams
├── specs/                        # Akam V1 brief and specs (French, internal working documents)
├── src/ak_rtsp/                  # ak_rtsp, flash_tool, Makefile, build_firmware.sh
├── tests/                        # host tests (fake binaries, synthetic fixture)
└── tools/
    ├── build/                    # check_host.sh, bootstrap.sh, env.sh, deps.lock (pinned versions)
    ├── ci/                       # local_check.sh, distro_check.sh
    ├── dropbear/                 # build_dropbear.sh, localoptions.h, patches
    ├── firmware_patch_templates/ # wrapper, akam_*.sh camera scripts, build-time patches
    ├── flash_scripts/            # install_with_flash_tool.sh, reflash_over_ssh.sh, restore.sh, ...
    ├── hardware_debug/           # upstream ESP32 U-Boot bridge and .NET serial console (Windows)
    ├── hw_tests/                 # akam_hwcheck.sh
    ├── image/                    # assemble_from_kit.sh
    ├── lib/                      # host.sh helpers
    ├── release/                  # make_kit.sh, sign_kit.sh, verify_kit.sh
    ├── sd/                       # sd_card.sh, prepare_first_boot_sd.sh
    └── wizard/                   # the wizard's libraries and a fake camera for the tests
```

For contributors: design notes are in `specs/`; run `tools/ci/local_check.sh` (shellcheck, tests, hygiene)
before sending a change.
