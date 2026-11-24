# Akam specifications

This folder describes Akam as it is built today. It is a reference, not a plan: each file states what the code does,
with the values (ports, paths, limits, defaults, file formats) that a reader needs to implement against it, test it or
review it. When a spec and the code disagree, the code is right and the spec is a bug: fix the spec.

Akam is an open-source replacement firmware for two LSC cameras, the Smart Indoor 1080p and the Smart Rotatable 2K (both
SoC Anyka AK3918AV130). It removes the cloud application, serves local RTSP, adds key-only SSH, disables Telnet and FTP, and
ships a PC-side installer (the Go assistant `akam`, and a bash wizard for the 1080p that will be retired).

**The specs describe the 1080p.** The as-built reference of the 2K is its findings pages, `docs/porting/lsc-rotatable-2k/`
([entry page](../docs/porting/lsc-rotatable-2k/README.md)): `control-api.md` (API), `net-settings.md`, `tracking.md`,
`cardless-appfs.md` (install and flash), `audio-findings.md`, `video-latency.md`. Where a spec has no 2K section, read those pages.

## How specs relate to code and docs

| Where | What it holds | Audience |
|---|---|---|
| `specs/` (this folder) | Behaviour and contracts: protocols, file formats, limits, safety rules, status | Developers, reviewers |
| `src/`, `tools/` | The implementation. Header comments explain single modules | Developers |
| `tests/` | Host tests (shell and C). Each spec names the tests that cover it | Developers |
| `docs/` | Guides for users and maintainers: how to install, use, test, troubleshoot | Users |
| `README.md`, `CHANGELOG.md` | Project front page, per-release changes | Everyone |

A spec never contains a task list, a schedule or a review history. Those live in the issue tracker and in git history.

## Conventions

- **MUST**, **MUST NOT**, **SHOULD**, **MAY** have their RFC 2119 meaning.
- Each file starts with a short summary and a `Status` line.
- Paths are camera paths unless they start with `src/`, `tools/`, `tests/` or `docs/`. Camera paths under
  `/etc/config/` live on the CONFIG flash partition (persistent). Paths under `/tmp/` are RAM and vanish at reboot.
- Example addresses use the documentation range `192.0.2.0/24`. Camera host names look like `akam-1a2b`.
- "Loopback" means a peer in `127.0.0.0/8` on the camera itself (the SSH helper scripts). "Remote" means any other peer.

### Status labels

| Label | Meaning |
|---|---|
| **hardware-validated** | Ran on a real camera (1080p `3215672.3` or 2K `3225722`, named in the section) as part of a flashed image or a RAM test, and the result was recorded |
| **host-tested only** | Covered by host tests (fake cameras, fixtures, stubs, sanitizers). Never run on a camera |
| **experimental** | Opt-in, incomplete or unproven on its target. Do not rely on it for safety |

A file that mixes levels labels each section. The evidence tables are in `docs/testing-status.md`.

## Index

| File | Contents |
|---|---|
| [architecture.md](architecture.md) | Hardware, flash map, what Akam replaces, boot chain, processes, persistent config, RAM use, watchdogs, logging |
| [video-and-streaming.md](video-and-streaming.md) | `akamd` video pipeline, encoder settings, RTSP server, Digest authentication |
| [audio.md](audio.md) | Microphone track, speaker output, two-way audio (ONVIF backchannel), `akam-talk` |
| [image-day-night-leds.md](image-day-night-leds.md) | Picture settings, day/night state machine, IR-cut and IR LEDs, status LED |
| [control-api.md](control-api.md) | Settings registry, signed local HTTP API (HMAC), generated MQTT entities (as built, with its design history) |
| [ptz-patrol.md](ptz-patrol.md) | PTZ patrol / scan engine: presets, routes, generated sweeps and rasters, triggers, priorities with tracking and manual control, safety, flash policy |
| [control-port.md](control-port.md) | TCP port 8091: commands, every key, privacy and write rules |
| [onvif.md](onvif.md) | ONVIF Profile S subset and WS-Discovery |
| [mqtt.md](mqtt.md) | MQTT client and Home Assistant discovery |
| [network-time-security.md](network-time-security.md) | Wi-Fi supervisor, hostname, NTP, SSH, Telnet/FTP lockdown, open ports |
| [installer.md](installer.md) | The Go assistant and the bash wizard (to be retired): commands, state, backup, flash and safety gates |
| [kit-and-release.md](kit-and-release.md) | Release kit, signatures, image assembly, release pipeline, size budgets |
| [roadmap.md](roadmap.md) | Known gaps that are still open |

## Principles that run through all specs

1. Light: one static ARM binary does video, audio, control, ONVIF and MQTT. Optional features cost no thread, socket
   or timer while off. Size and memory budgets are measured, not guessed.
2. Standards first: RTSP, ONVIF Profile S, MQTT 3.1.1 with Home Assistant discovery, SSH. No cloud, no vendor app.
3. Off by default: the RTSP password, ONVIF, MQTT, NTP, the microphone, the signed API (it opens with its first key) and the
   LAN side of port 8091 are all opt-in.
4. Only one application partition is ever rewritten (APP on the 1080p, appfs on the 2K). Bootloader, kernel, root file system and CONFIG are never replaced.
5. No vendor data is distributed. The user supplies their own dump; images are built locally.
6. Never claim "validated" for code that was only compiled or host-tested.
