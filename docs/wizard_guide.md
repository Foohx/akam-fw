# The setup menu and the bash wizard

> **The bash wizard will be retired.** It stays for the 1080p, on Linux, until the Go program's install has run on a camera;
> new features go to the Go program only. This page describes the menu and the nine steps of the 1080p install (the Go program
> has the same menu), then the bash wizard's differences. `akam` is the Go program ([assistant](assistant.md)); `./akam` below
> means the bash wizard in a source checkout. To install a camera you only need the [install guide](install.md).

### Bash wizard differences

The bash wizard (`./akam` in a source checkout) has `debug` and `video minqp`/`maxqp`, which the Go program does not. It lacks
`control-port`, `api-key`, `key`, `config`, `releases`, `trust-key`, `release-source`, `import-backup`, `--version` and `--kit`
on `update`, and every 2K command. It is 1080p and Linux only.

Cameras: this page is about the 1080p install; the 2K: [2K install](porting/lsc-rotatable-2k/README.md#install).

## The menu

**Follow the menu**, one step after the other. Arrow keys (or `j`/`k`) and Enter to run a step (digits 1 to 9 are
shortcuts), `?` to read what it does and what it risks, `c` to change camera, `q` to quit (your progress is kept). Nothing waits
for a "Press Enter": a result shows on one line under the menu and any key clears it. In a camera list, `s` scans
the network and `o` searches with ONVIF (each asks first), `t` types an address, `r` rechecks, `f` forgets, Esc goes back.

The first run asks for a short name for your camera (for example `kitchen`). `./akam help`
lists every command; add `--dry-run` to rehearse without changing anything. The menu (in colour on a
real terminal; `--plain` or no terminal gives line-by-line prompts):

```text
╭─ Akam · guided setup ───────────────────────────────────── v0.1.0  3/9 done ╮
│ Host  Arch Linux x86_64      Camera  kitchen · no address yet · c to change │
│ Kit   akam 0.1.0               Image   not built yet                        │
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

Below the steps, a *Maintenance* list holds `update`, `hostname`, `rtsp-auth`, `restore` and `wipe-sd`; the other settings commands (`led`, `audio`, `picture`, `video`, `onvif`, `ntp`, `mqtt`; `debug` in the bash wizard only) are typed.
Before a step starts, the wizard shows what it does and asks **Start this step? [Y/n]**. Anything
irreversible (erasing a card, writing the camera's flash) shows a **red box** that says exactly what
will happen and asks one yes/no whose **default is No** (Enter, Esc and `q` mean No). Nothing has to be
typed, and without a terminal these steps refuse to run.

## What the wizard does

| Step | What happens | Risk |
|---|---|---|
| `doctor` | checks the tools on your PC | none |
| `get-kit` | downloads the prebuilt kit of your camera model (bash wizard: `--version X.Y.Z` of a firmware release is required; `--model ID` if several models exist), checks checksum and signature (`./akam build` builds from source instead) | none |
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
  command line; the password is written only to the cards it prepares (in clear text) and to the camera. Only the Go
  assistant can save it for you, in a separate private file, and only after a yes (default No) when you save a
  [config](using_akam.md#configs-global-local-copy); the bash wizard never does.
- **SSH key:** `prepare-flash-sd` asks "Which SSH key should open this camera?": the shared key `~/.ssh/akam` (the default,
  one key for all your cameras), a key for this camera only (`~/.ssh/akam-<camera-name>`), or one of your ed25519 keys. A key
  that is missing is created with `ssh-keygen` (it asks the passphrase) after a yes; an existing file is never overwritten.
  Changing the key of a camera that is already installed is not available in this bash wizard (use the card again, or the
  assistant: `akam key use-shared | use-new | use FILE.pub`, see
  [SSH provisioning](manual/ssh-and-lockdown.md#change-the-key-over-ssh)).
- **Wi-Fi:** `flash` and `verify` save it on the camera and never ask you to remove the card before
  ([if it fails](troubleshooting.md#the-camera-has-no-network-after-removing-the-card)).
- Keep your backup (two copies, on two media) and never publish it: it holds your Wi-Fi credentials.
  The cards hold your Wi-Fi password in clear text: erase them with `./akam wipe-sd` when done.

## The Go program

The single-file Go program `akam` runs on Linux, macOS and Windows and covers both cameras: [assistant](assistant.md) (download, check,
tested platforms, unsigned-binary steps), [install guide](install.md), [hardware test plan](hardware-checks-assistant.md).
It shares the progress files with the bash wizard. On Windows and macOS the 1080p image can be built with `--builder native`
(experimental), or built on Linux or in WSL and given with `--image FILE`, as in the assistant page.

### Choosing your firmware (Go assistant only)

"Set up a new camera" and "Update this camera" ask **Which firmware?** before anything is downloaded, built or
written. The list (a full-screen list, or numbered in `--plain`; Enter takes the first row) holds:

| Row | What it does |
|---|---|
| `Akam X.Y.Z (latest)` (the default) | the newest release published on GitHub, downloaded and verified by its signature |
| `Akam X.Y.Z  - older` | an earlier release, to go back. If it is older than what the camera runs, you get a warning (settings files saved by the newer version may not be understood; your backup stays valid) and one yes/no, default No |
| `Kit already downloaded: X.Y.Z` | a kit kept in `<state folder>/downloads/`; it is verified again before it is used |
| `A kit file or folder...` | a kit you have, by path; verified exactly like a download |
| `An image file you built (.squashfs)...` | an image you built yourself: exactly 5 242 880 bytes, md5 shown, checked as a squashfs. This program did not build it, so it asks you to take responsibility (default No) |
| `Stock firmware (from your backup)` | update only, on an Akam camera, and only when a verified backup of the **stock** firmware exists. It is the restore flow with all its gates (the backup re-hashed, the red box, default No). **Never tested on a real camera.** |

The title shows what the camera runs now: its build id (such as `b35-2af81c5`, read from the signed API, or from port 8091 on an older image or when you opened it) and its Akam version.
The version is read over SSH from the manifest the image carries (`/usr/local/akam/MANIFEST.txt`, the kit's own, so images
built from a release kit state it; an image built from a source checkout states a git description, which is not a version).
When that cannot be read (a camera flashed before this, an image built by hand), the version this assistant recorded after
its own last write is used, and only while the camera still reports the build seen after that write; otherwise the version
is unknown and **no downgrade warning can be given**. The list of releases is only advice read from GitHub (HTTPS, at most 8 MiB
per page and 5 pages of 50 releases, once per session): nothing in it is trusted, and a kit is never used without its SHA256SUMS and signature check. If GitHub
cannot be reached (offline, rate limit), the list says so and the local rows stay available. Prereleases are hidden unless
you pass `--include-prereleases`. In a source checkout (`mode=source`) the first install shows no list: the kit comes from
the checkout. A release or a kit needs an image built from it and from your own stock backup: the Go program builds it with `--builder native`
(experimental) or the bash wizard builds it (`./akam get-kit --version X.Y.Z`, then `./akam step image`, then update again and take
"The image already built from your backup"). A kit is always downloaded or opened and verified again, into a staging folder: the
kit, the recorded downloads and the state change only after every check passed and the camera was written and read back; a
refusal, a failure, a No at the red box or an interruption leave everything as it was. The finer rules (the `kit.swap` marker that
repairs a crash in the final swap, what a rehearsal never installs) are in [the installer spec](../specs/installer.md). An imported image on record is never presented as the image of a
release: it has its own row, and a release row asks (default No) before writing it. If the release list failed, "Try again to read the releases" asks again. Naming a firmware on the command line skips the list:

```bash
akam releases [--include-prereleases]            # the published versions, newest first
akam update --version 0.2.0                        # a release (verified like get-kit)
akam update --kit akam-kit-lsc-indoor-1080p-ak3918-0.2.0.tar.xz            # a kit you have
akam update --image my.squashfs                    # an image you built
akam update --stock                                # back to the original firmware (restore, same gates)
```

`./akam status` then shows `Firmware source:` (`release:X.Y.Z`, `kit:PATH`, `image:PATH` or `stock`) with the date akam
recorded it; an update by the bash wizard does not touch it. It states what was written, recorded after the camera was
written and read back: an image built here from a kit is `release:` (the kit is the
download of that release) or `kit:`; any image you imported, even through a release row, is `image:PATH`. The bash wizard
keeps its current options (`--image`, `./akam restore`), has no list, no `releases` and no `--stock`, and its `update`
refuses `--version` and `--kit` (exit 2: Go assistant only).
