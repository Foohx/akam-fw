# Install and update through the assistant: the user journey per model

Status of every step at the start of the branch (`dev` f82ce99), then what `feat/assistant-install` adds.
Legend: **exists** = works from the assistant on Linux, macOS and Windows; **partial** = exists with a named limit;
**missing**. `akam` (CLI and menu) is `tools/assistant`; "script" means a bash tool under `firmware/` or `tools/`.

## 1080p (`lsc-indoor-1080p-ak3918`, kind `native-app`, APP partition `mtd7`)

| Step | Status at start | How |
|---|---|---|
| Release kit (signed tarball) | exists | `akam get-kit` (HTTPS, SHA256SUMS + SSH signature, safe extraction), `akam releases` |
| Kit build (maintainer) | exists | `tools/release/make_kit.sh`, `sign_kit.sh`, `verify_kit.sh` |
| First install, SD card | exists | `prepare-backup-sd`, `prepare-flash-sd` (guarded device choice, FAT32, hash verify, all three OS) |
| Backup | exists | `akam backup` (SSH, SHA256SUMS, second read), recognised by the stock APP sha256 |
| Flash (first install, SSH) | exists | `akam image` + `akam flash` (red box, md5, detached worker) |
| Update Akam to Akam | exists | `akam update`, `akam verify` (version from the control port `fw.version`) |
| Rollback / recovery | exists | `akam restore` (stock APP from the backup), SD recovery |

## 2K (`lsc-rotatable-2k-ak3918av130`, kind `cardless-appfs`, appfs `mtd3`)

| Step | Status at start | How |
|---|---|---|
| Signed release kit for the 2K | **missing** | no producer (the 1080p `make_kit.sh` refuses this kind); the kit verifier only knew the 1080p layout |
| First install, SD card: the kit | **partial** | `firmware/lsc-rotatable-2k-ak3918av130/image/build_card.sh` (bash, needs a source tree and a build) |
| First install, SD card: screens | **missing** | the 1080p SD screens are refused on a 2K (`modelGate`) |
| Backup (mtd0..mtd5) | partial | by hand over SSH (`on-camera.md`); the assistant backup is 1080p only |
| Card-less image build | exists | `akam appfs-build` (Go port of `build_appfs.sh`, any OS) |
| Flash, first card-less | exists | `akam appfs-flash --execute` (Go port of `flash_appfs.sh`, every gate, typed confirmation) |
| Hot update (card-less to card-less) | partial | `appfs-flash --current-image F --hot-preflight` then `--hot`; kit, image, current image and backup dir all passed by hand |
| Detect model and version | **missing** | no client of the signed control API (`GET /info`) in the assistant |
| One `akam update` flow | **missing** | `update` is the 1080p flow and is refused on a 2K |
| Verify after the update | **missing** | the flash reads mtd3 back; nothing waits for the camera or checks the version after |
| Keep the previous image for rollback | **missing** | the operator keeps `--current-image` by hand |
| Rollback / recovery | partial | on-camera worker rolls back by itself; SD card override; UART/CH341A in `uart-recovery.md`; no assistant screen |

## What this branch adds (status after)

| Step | Now | How |
|---|---|---|
| Signed release kit for the 2K | exists (producer + verifier + release pipeline) | `tools/release/make_kit_2k.sh`, `tests/kit2k_test.sh`; the kit verifier takes a layout (`kit.Layout`, `appfs.KitLayout`). Built by `make_release.sh` into the one signed `SHA256SUMS` of the firmware release (the file the assistant downloads); not yet published in a release |
| First install, SD card: the kit | exists | the kit carries `card/` |
| First install, SD card: screens | exists | `akam prepare-2k-sd` (also the "Set up a new camera" entry on a 2K): kit, SSH key, Wi-Fi, device, red box, FAT32, sha256 verify of every file |
| Backup (mtd0..mtd5) | exists | `akam backup-2k` (read only, double read, verified, recorded for `update`); `--backup-dir` still takes a backup made by hand |
| Detect model and version | exists | `internal/camera/akamapi`: HMAC signer on the shared vectors, `GET /info`; needs an API key |
| One `akam update` flow | exists | `appfs.PrepareUpdate` / `appfs.RunUpdate`, CLI `rot2k_install.go`, also the "Update this camera" entry |
| Verify after the update | exists | wait for SSH, `mtd3` read back and compared, version compared (a mismatch is a warning, the image hash is the gate) |
| Keep the previous image | exists | `ImageStore` in the camera's state folder: content-addressed images and one atomic `store.json` {current, previous, pending}; an unrecorded write is settled by a read-only `mtd3` dump at the start of the next update; `appfs-flash` records its verified write |
| Rollback | exists | `akam update --rollback` (same gates); recovery by SD card or UART as before |
| Windows and macOS | exists for all of the above | pure Go, no bash on the user path; `--hot` runs the on-camera helper, uploaded verbatim |

## Remaining

* The 2K kit is built by `make_release.sh` into the one `SHA256SUMS` of the firmware release (signed and published by
  `publish.sh` like the 1080p kits); no release carrying it has been published yet, so `update` and `prepare-2k-sd` without
  `--kit` have nothing to download until the first firmware release with the 2K kit.
* `akam backup-2k` backs the 2K up from the assistant (read only, double read, sizes and `SHA256SUMS` checked, recorded for
  `update`); it is a separate command, there is no menu entry for it yet.
* `akam key-2k` creates the API key: the camera makes it itself (`akam_api_key.sh new`, its own random pool; the key is NOT
  generated on the PC because that script has no "install this secret" entry), the secret comes back once over SSH, is kept in the
  camera's state folder (`api_key`, 0600) and is never printed. It writes `/config/akam/api_keys` on the camera, so it asks first.
  Key rotation (revoke then create again) is by hand over SSH for now.
* Seeding `/config/akam` (Wi-Fi, SSH key) before a first card-less flash, and the staged-reboot screens of the first flash, are
  still the design of `assistant-rot2k-flash-design.md` section 3.3 and 3.5 (`appfs-flash` prints the steps).
* Not run on hardware: every test uses fakes (no camera, no network). The hot update itself is the already-tested Go port of
  `flash_appfs.sh --hot`; the first live run of `akam update` should be a rehearsal with a recovery means at hand.
