# The setup wizard: menu, steps and the native binary

This page is for people who want to know what `./akam` does at each step, how its menu works, and what the
experimental single-file `akam` binary can and cannot do. To install Akam you only need the short steps in the
[README](../README.md#install-in-5-steps).

## The menu

**Follow the menu**, one step after the other. Arrow keys (or `j`/`k`) and Enter to run a step (digits 1 to 9 are
shortcuts), `?` to read what it does and what it risks, `c` to change camera, `q` to quit (your progress is kept). Nothing waits
for a "Press Enter": a result shows on one line under the menu and any key clears it. In a camera list, `s` scans
the network and `o` searches with ONVIF (each asks first), `t` types an address, `r` rechecks, `f` forgets, Esc goes back.

The first run asks for a short name for your camera (for example `salon`). `./akam help`
lists every command; add `--dry-run` to rehearse without changing anything. The menu (in colour on a
real terminal; `--plain` or no terminal gives line-by-line prompts):

```text
╭─ Akam · guided setup ───────────────────────────────── v1.0.0-dev  3/9 done ╮
│ Host  Arch Linux x86_64        Camera  salon · no address yet · c to change │
│ Kit   akam 1.0.0               Image   not built yet                        │
╰─────────────────────────────────────────────────────────────────────────────╯
   ✔  1  Check this computer                                          done
   ●  2  Get the Akam kit                                             todo
   ✔  3  Prepare the backup microSD card                              done
   ✔  4  Find the camera on your network                              done
 ▸ ●  5  Back up the camera (all 8 partitions)                        todo
   ✖  6  Build the Akam image from your backup                        blocked
   ✖  7  Prepare the flash microSD card                               blocked
   ✖  8  Flash the image to the camera                                blocked
   ✖  9  Verify the result                                            blocked
 ↑↓ move  ⏎ run  ? help  r pick several  a run all  c camera  q quit
```

Below the steps, a *Maintenance* list holds `update`, `hostname`, `rtsp-auth`, `restore` and `wipe-sd`; the other settings commands (`led`, `audio`, `picture`, `video`, `onvif`, `ntp`, `mqtt`, `debug`) are typed.
Before a step starts, the wizard shows what it does and asks **Start this step? [Y/n]**. Anything
irreversible (erasing a card, writing the camera's flash) shows a **red box** that says exactly what
will happen and asks one yes/no whose **default is No** (Enter, Esc and `q` mean No). Nothing has to be
typed, and without a terminal these steps refuse to run.

## What the wizard does

| Step | What happens | Risk |
|---|---|---|
| `doctor` | checks the tools on your PC | none |
| `get-kit` | downloads the prebuilt kit, checks checksum and signature (`./akam build` builds from source instead) | none |
| `prepare-backup-sd` | writes the backup toolkit to a microSD card | erases that card |
| `find-camera` | asks the camera's address, probes its ports | none |
| `backup` | copies the 8 flash partitions twice, compares them, stores them in `<Documents>/Akam backups/<name>/<date>` (private to your user) | none (read only) |
| `image` | builds the Akam image from your backup, shows the hardware verdict | none |
| `prepare-flash-sd` | writes the image, your SSH key and the Wi-Fi settings to a card | erases that card |
| `flash` | writes the APP partition (over SSH when Akam already runs, else through the maintenance telnet and the card) | **can brick the camera** |
| `verify` | checks SSH, ports and video, saves the Wi-Fi on the camera before you remove the card, then checks the boot without the card | none |

Good to know:

- The wizard never runs `sudo` itself, shows every command before running it and never reboots the camera: you unplug
  and replug the power yourself. It never keeps your Wi-Fi password in its own state or log files and never puts it on a
  command line; the password is written only to the cards it prepares (in clear text) and to the camera.
- **SSH key:** `prepare-flash-sd` asks "Which SSH key should open this camera?": the shared key `~/.ssh/akam` (the default,
  one key for all your cameras), a key for this camera only (`~/.ssh/akam-<camera-name>`), or one of your ed25519 keys. A key
  that is missing is created with `ssh-keygen` (it asks the passphrase) after a yes; an existing file is never overwritten.
  Changing the key of a camera that is already installed is not available in this bash wizard (use the card again, or the
  assistant: `akam key use-shared | use-new | use FILE.pub`, see
  [SSH provisioning](akam_v1_ssh_and_lockdown.md#change-the-key-over-ssh)).
- **Wi-Fi:** `flash` and `verify` save it on the camera and never ask you to remove the card before
  ([if it fails](troubleshooting.md#the-camera-has-no-network-after-removing-the-card)).
- Keep your backup (two copies, on two media) and never publish it: it holds your Wi-Fi credentials.
  The cards hold your Wi-Fi password in clear text: erase them with `./akam wipe-sd` when done.

## Native binary (experimental)

A single static binary version of the wizard is being written in Go ([tools/assistant](../tools/assistant/README.md),
[plan](../specs/09-spec-assistant-binary.md)). It is **experimental: the bash wizard (`./akam`) remains the
reference**. The binary covers the daily commands on a camera that already runs Akam (`status`, `led`, `audio`,
`picture`, `video`, `debug`, `onvif`, `ntp`, `rtsp-auth`, `hostname`, `talk`, `say`, `find-camera`) with no SSH,
`nc` or `akam-talk` to install, kit download and verification, `update`, `restore` and `verify` over SSH, and a
first install through the maintenance telnet and a built-in microSD writer (`backup`, `flash`, `prepare-*-sd`,
`wipe-sd`). The `image` step stays in the bash wizard by default; an experimental native image builder (`akam image --builder native`, opt-in) exists but was never flashed on a camera. It has been tested on fake cameras and files only: the
update, restore, verify, telnet backup and flash and SD writer phases were never run against a real camera or card.
Phase 11 adds builds for Linux, Windows and macOS (amd64 and arm64), a home screen with a camera chooser and option screens,
and a backup over SSH. The Windows and macOS binaries are **built, not hardware-tested**, and they import an image built on Linux
or in WSL for now ([install and unsigned-binary steps](assistant_install.md), [hardware test plan](assistant_hardware_test_plan.md)).
It shares the progress files with `./akam`. Build it with `tools/assistant/build.sh` and try it with
`AKAM_USE_BIN=1 ./akam ...`.
