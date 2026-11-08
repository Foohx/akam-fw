# Changelog

## Unreleased: project renamed from Camyn to Akam

The project was called Camyn during early development. Everything user-visible was renamed; the
flash layout, ports, the `ak_rtsp` binary name, the 8091 protocol and the persistent key files under
`/etc/config/dropbear/` are unchanged, so an existing camera keeps its host key and `authorized_keys`
after a reflash.

| Former name | New name |
|---|---|
| `CAMYN_*` environment variables and flags (`CAMYN_ROOT`, `CAMYN_FLASH_I_KNOW`, ...) | `AKAM_*` |
| `camyn_*.sh` scripts (`camyn_ssh.sh`, `camyn_lockdown.sh`, `camyn_hwcheck.sh`, `camyn_reflash_ssh.sh`) | `akam_*.sh` |
| `docs/camyn_v1_*.md`, `specs/*-camyn-v1.md` | `docs/akam_v1_*.md`, `specs/*-akam-v1.md` |
| SD card folder `camyn/` (with `authorized_keys`) | `akam/` |
| SD card flag file `CAMYN_MAINTENANCE` | `AKAM_MAINTENANCE` |
| On the camera: `/usr/local/camyn/`, `/tmp/camyn_*` | `/usr/local/akam/`, `/tmp/akam_*` |
| `LICENSE.camyn` in the image | `LICENSE.akam` |

### Deprecated aliases (removed in the next release)

An SD card prepared before the rename still works with the new image:

- `akam_ssh.sh` reads `camyn/authorized_keys` when `akam/authorized_keys` is absent and logs a
  `DEPRECATED` line in `/tmp/akam_ssh.log`.
- The file `CAMYN_MAINTENANCE` at the card root is accepted as `AKAM_MAINTENANCE` by the patched
  `service.sh` (SD hooks and the RAM marker), by `akam_lockdown.sh` (logs `DEPRECATED`) and by
  `akam_hwcheck.sh` (prints a `WARN`). Build without the alias with `AKAM_MAINT_FLAG_LEGACY=`.
- `flash_guard.sh` accepts `CAMYN_FLASH_I_KNOW=1` as `AKAM_FLASH_I_KNOW=1`, with a warning.
- `patch_service_sh.sh` recognises daemon lines already disabled with the former `CAMYN-DISABLED:` marker.

What to change on your side: rename the `camyn` folder and the `CAMYN_MAINTENANCE` file on your SD
card, use the new script and variable names, and (optionally) rename your local `~/.ssh/camyn` key
and the `~/camyn-backup` / `~/camyn-work` / `~/camyn-sdprep` directories that the docs now call
`~/.ssh/akam`, `~/akam-backup`, `~/akam-work` and `~/akam-sdprep` (`prepare_first_boot_sd.sh` looks for
the toolkit clone in `~/akam-sdprep/toolkit` by default: pass `--toolkit ~/camyn-sdprep/toolkit` to reuse an old clone).
