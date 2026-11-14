# Using Akam day to day

This page is for when Akam already runs on your camera: every setting and command, the ports that are open,
how to watch the stream, how to update, and the security notes. Not installed yet? Start with the
[README](../README.md) and the [compatible cameras](compatible-cameras.md) page.

## What Akam offers

The complete list of features, ports, defaults and test status is on the [features](features.md) page. In short: RTSP
video and microphone on port 554 (one client at a time), two-way audio, day/night and LED settings, a read-only
control port on 8091, optional ONVIF-compatible services (not certified), optional MQTT with Home Assistant
discovery, opt-in NTP, SSH with a key only. For several viewers, put
[go2rtc](https://github.com/AlexxIT/go2rtc) or MediaMTX in front of the camera.

## Open ports

| Port | Service | When |
|---|---|---|
| 22/tcp | SSH (Dropbear, public key only) | always |
| 554/tcp | RTSP video, audio and the talk backchannel | always |
| 8091/tcp | metrics and control, read-only from the network | always |
| 80/tcp (or the port you chose) | ONVIF device, media and imaging services | only after `./akam onvif enable` |
| 3702/udp | ONVIF WS-Discovery (multicast) | only after `./akam onvif enable` |

Telnet (23, 24) and FTP (21) are closed. UDP 123 is used outgoing only, and only if you set an NTP server.
Never expose any of these to the Internet.

## Several cameras

Each camera has its own name, progress, log and folders. Switch with `./akam --camera NAME`, or press
`c` in the menu (list, "New camera", "Register an existing Akam camera"). `./akam cameras` lists them,
`./akam rename-camera NEW` renames the current one. A camera that already runs Akam (flashed by
hand, or by an earlier version) is recorded without redoing anything:

```bash
./akam --camera salon --ip 192.0.2.50 [--backup-dir DIR] register
```

## Configs: global, local, copy

> Go assistant only (the bash wizard has no configs; its commands are unchanged). Host-tested only: not run on a real camera.

A **config** is a copy of the settings of a camera, kept on **this computer**, that you can use again on another camera. It holds:

- the **settings** of the Options screen that the camera keeps across a reboot: video (bitrate, frame rate, key frame interval), picture, LED and IR, audio, ONVIF on or off and its port, time sync (NTP), MQTT (broker, port, user, topic and options), the video password on or off and its user name;
- the **SSH key choice**: shared (`~/.ssh/akam`), a key of its own for the camera (`~/.ssh/akam-<name>`), or a custom key file;
- optionally, in a separate private file: the **video password**, the **MQTT password**, and the **Wi-Fi** name and password (to prepare the card of a new camera without typing them again).

It never holds the camera's host name (that is the camera's identity) nor `night.mode` and the white balance (they go back to their default at every reboot), and nothing is ever synchronised: a config is applied only when you ask, and a camera keeps its own settings.

| Config | Where it is | What it is for |
|---|---|---|
| **Global** | `profiles/global.conf` (+ `global.secrets`) in the state folder | the default template: offered when you set up a new camera, and applied on request |
| **Local** | `camera.conf` (+ `camera.secrets`) in the camera's own state folder | what was last read from, or applied to, that camera. Nothing is applied from the global config unless you ask, so the camera simply keeps these |
| **Copy** | nothing new | take the settings of another camera, live if it answers, else its local config |

`akam status` and the camera chooser say where the settings of a camera came from (`local (from global)`, `local (from camera salon)`, `local`), and the Options screen shows it in its title (`settings: local (from global)`).

### Wi-Fi and passwords: your choice, each time

The Wi-Fi password was never kept on this computer before. **It still is not, unless you say yes**, separately for the passwords and for the Wi-Fi, every time you save a config (default No):

> Also keep the Wi-Fi name and password in the global config? They are stored on this computer in a private file; anyone with access to your account can read it. [y/N]

They go in `global.secrets` or `camera.secrets` (mode 600, restricted to your account on Windows), never in the settings file, the state file, the log, a command line or the screen (`config show` says `set` or `not set`, and the Wi-Fi name is not printed or logged either). The camera cannot tell its passwords, so you type them when you keep them. A saved password is kept **with what it belongs to** (the video password with its user name, the MQTT password with its broker and user) and is used only while the settings still say the same: when you save again with another user or broker the old password is removed, and a yes on a matching one asks "Keep it? (No: type a new one)". After `rtsp-auth set` or `mqtt set` on a camera whose saved password is now out of date, you are asked (default No) whether to update it; otherwise it is removed. Delete them at any time:

```bash
./akam config forget-secrets --global        # the global config's passwords and Wi-Fi
./akam --camera salon config forget-secrets  # one camera's
```

The Wi-Fi is only ever used to prepare the card of a new camera. It is never applied to a running camera (there is no command for that).

### What to do

```bash
./akam config save-global                 # read this camera now into the global config (asks before replacing one)
./akam config save-global --from salon    # ... or from another camera (live if it answers, else its saved config)
./akam config show --global               # what the global config holds (passwords: set / not set)
./akam config apply --global              # compare with this camera, show the differences, ask, then apply
./akam config apply --from salon          # copy the settings of camera salon to the current camera
./akam config apply --global --only video,picture
./akam config save-local                  # record this camera's settings as its local config
./akam config show                        # this camera's local config
```

`config apply` reads the camera, shows only what would change (passwords masked), asks one yes/no (default No), then sends the changes the way the Options screens do and tells for each one whether it was applied, failed, or needs a video restart (asked separately, default No). Details:

- A password cannot be read back from a camera, so it is sent only together with the setting that needs it (turning the video password on, changing its user, a new MQTT broker); `--include-secrets` sends the saved ones again. A password that the config does not hold is asked.
- Taking the video password off, and changing the SSH key, each ask their own question (default No). The key change is the same flow as `./akam key use ...`: the new key is added, proven with a second login, and only then remembered.
- `--only` takes `video,picture,led,audio,onvif,ntp,mqtt,auth,key`.
- A failed item does not stop the others; the exit status is 1 if any failed. What the camera could not be read for (an older image, a helper that did not answer) is listed and left out of the comparison, never treated as "off"; when the control port says nothing at all, nothing is compared, saved or recorded (and a source camera is then replaced by its last saved config, with a warning).
- The result is recorded as the camera's local config **before** the video restart question (the port is down for a moment after a restart), and a read that got nothing never overwrites the record.
- Copying from another camera reads it over SSH with its own key and its remembered host key; a changed host key, a host name that is not the one this computer knows, or an unknown host key stop the live read (it says why and uses the last saved config).

In the full-screen assistant, **Configs** on the home screen (key `9`) does all of this from a menu, and **Edit the global config** is a form like the Options screen that needs no camera.

### A new camera

When you set up a new camera and you have saved configs, the assistant asks once, before the card step: **Use a config for this camera?** (the global config, a copy from another camera, or keep the camera's defaults). The Wi-Fi and the SSH key of that config are then offered for the card (each one a yes/no, default No), and its settings are applied after the verify step, with the differences shown and a yes/no like any apply. A `no` leaves it pending: `./akam config apply --global` later.

## Command reference

`./akam help` is the full text. Run `./akam` with no command for the menu.

| Command | What it does |
|---|---|
| `doctor`, `get-kit`, `build` | check this PC, download and verify the prebuilt kit, or build from source |
| `prepare-backup-sd`, `find-camera`, `backup`, `image`, `prepare-flash-sd`, `flash`, `verify` | the installation steps, in order |
| `update`, `restore` | flash a newer image over SSH, or put your original APP back. Go assistant: `update [--version X.Y.Z \| --kit FILE \| --image FILE \| --stock]` ([choose your firmware](wizard_guide.md#choosing-your-firmware-go-assistant-only)) |
| `releases` | Go assistant: the Akam versions published on GitHub, newest first (`--include-prereleases`) |
| `wipe-sd` | erase a card that holds your Wi-Fi password |
| `status`, `step NAME` | show every step; run one step again |
| `cameras`, `rename-camera NEW`, `register` | work with several cameras |
| `hostname [NAME]` | the camera's network name |
| `rtsp-auth [set\|disable\|status]` | the Digest password on the stream (also needed by ONVIF and talk) |
| `led [show \| status on\|off\|auto] [ir auto\|off]` | front LED and infrared LEDs |
| `audio [show \| volume N \| beep \| on \| off \| talk on\|off \| mic on\|off]` | speaker and microphone |
| `picture [show \| brightness\|contrast\|saturation\|sharpness N \| wb \| night \| reset]` | the image |
| `video [show \| bitrate N \| fps N \| gop N]` | encoder settings |
| `onvif [enable [PORT] \| disable \| status]` | ONVIF, off by default |
| `mqtt [set \| test \| status \| disable \| forget]` | publish the camera to an MQTT broker and Home Assistant, off by default ([MQTT](mqtt.md)) |
| `ntp [status \| set SERVER... \| off]` | time synchronisation, off by default |
| `debug [on [MINUTES] \| off \| status]` | let the network change settings on port 8091 for a while |
| `key [show \| use-shared \| use-new \| use FILE.pub]` | which SSH key opens this camera, and change it over SSH: the shared key `~/.ssh/akam`, a key for this camera only `~/.ssh/akam-<name>`, or any ed25519 key. Assistant only; adds the new key, checks it with a second login, saves it for this camera, then asks (default No) whether to remove the old one ([details](manual/ssh-and-lockdown.md#change-the-key-over-ssh)) |
| `config [show \| save-global \| save-local \| apply \| edit-global \| forget-secrets]` | the settings of a camera saved on this computer: a global template, a local config per camera, copy from another camera; passwords and Wi-Fi only if you say yes. Assistant only; see [Configs](#configs-global-local-copy) |
| `talk [SECONDS \| test]`, `say FILE` | speak or play a sound through the camera speaker |

## Update, change the name, add a video password

All of this works over SSH, with no card, on a camera set up by the wizard (or registered):

| I want to... | Command |
|---|---|
| flash a newer image | `./akam update` (`--image FILE` for a specific one). Go assistant: it asks "Which firmware?" (the latest release by default, an older one, a kit or an image you have, or the stock firmware), or `akam update --version X.Y.Z`, `akam releases` to list ([details](wizard_guide.md#choosing-your-firmware-go-assistant-only)) |
| change the camera's network name (what your router lists) | `./akam hostname [NAME]` (default: the camera's name), applied at the next power cycle |
| ask for a user and password on the stream (off by default) | `./akam rtsp-auth set` (also `status`, `disable`) |
| turn the front LED off or make it a status light, or keep the infrared LEDs dark at night | `./akam led` shows both; `./akam led status on\|off\|auto`, `./akam led ir auto\|off` |
| talk through the camera speaker from this computer, or play a sound file on it | `./akam talk` (microphone, Ctrl-C stops), `./akam talk test`, `./akam say FILE`; needs the video password ([two-way audio](two_way_audio.md)) |
| check or change the speaker volume, play a beep, switch the speaker, talk or the microphone on or off | `./akam audio` shows the settings; `./akam audio volume 0-100`, `beep`, `on`, `off`, `talk on\|off`, `mic on\|off` (saved on the camera; `mic off` removes the audio track from the stream and needs a reboot) |
| publish the camera to Home Assistant or another MQTT broker, and take a few commands from it (off by default; the camera connects out, no new port) | `./akam mqtt set` (asks the broker, the password is not shown), then `mqtt test`; also `status`, `disable`, `forget` ([MQTT and Home Assistant](mqtt.md)) |
| give the camera the right time (it has no clock of its own); off by default, no traffic unless you ask | `./akam ntp set pool.ntp.org` (1 to 3 servers; also `status`, `off`) |
| change brightness, contrast, saturation, sharpness, white balance or day/night mode | `./akam picture` shows them; `./akam picture brightness N` (also `contrast`, `saturation`, `sharpness`, N from -50 to 50), `wb conf\|manual`, `night auto\|day\|night`, `reset` (all of them are saved on the camera) |
| lower the bitrate or frame rate, or change the key frame interval of the video | `./akam video` shows them; `./akam video bitrate N` (500 to 3000 kbit/s, default 1536), `fps 10\|15\|20\|25` (default 15; higher rates make dim light darker and grainier and start night mode earlier, see [troubleshooting](troubleshooting.md)), `gop N` (key frame every N seconds, 1 to 4, default 2). Saved on the camera and applied when the video service restarts (it offers to restart it for you; the stream stops for a few seconds). Resolution and flip/mirror are not available yet |
| make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager); off by default | `./akam onvif enable [PORT]` (asks first; needs the video password), `./akam onvif status`, `./akam onvif disable` ([ONVIF](onvif.md)) |
| go back to the original firmware | `./akam restore` (from your backup; no guarantee, see [risks](../README.md#important-disclaimer-and-risks)). Go assistant: also `akam update --stock`, or "Stock firmware (from your backup)" in the firmware list; the same gates, never tested on a real camera |

The LED settings are saved on the camera. `status auto` makes the LED blink while the camera starts or
has no Wi-Fi, blink slowly while someone watches the video, and stay solid otherwise; the default `on`
leaves the LED as it always was. `ir off` still switches the image to night mode but never lights the
infrared LEDs .

`update` and `restore` write the camera's flash and ask the same red yes/no. The video password is a
Digest login (works with VLC, ffmpeg, go2rtc and Frigate); it does not encrypt the video. `./akam
rtsp-auth set` stores the password on the camera twice: as a hash for the Digest login, and in clear text in
a file only root can read (`/etc/config/akam/credentials`), because the ONVIF login (see [ONVIF](onvif.md)) cannot work
from a hash. `./akam rtsp-auth status` says `ONVIF-ready` when that copy is there; `disable` removes both. Put it in the address: `rtsp://<user>:<password>@<camera-ip>:554/`, with
special characters URL-encoded ([401 errors](troubleshooting.md#401-unauthorized-or-a-password-prompt-on-the-stream)).

## Watching the stream

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

Any path works. The camera serves **one RTSP client at a time** (port 554; port 8091 is separate and takes up to 4, see below): to serve several viewers or to record and
watch together, put [go2rtc](https://github.com/AlexxIT/go2rtc) or
[MediaMTX](https://github.com/bluenviron/mediamtx) in front of it. In VLC, if hardware decoding fails,
use `vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/`; a VLC window on a hidden Wayland
workspace pauses the stream ([why and what to do](troubleshooting.md#vlc-goes-black-or-stops-when-its-window-is-on-another-workspace)).

**Two-way audio:** the RTSP port also accepts the standard ONVIF audio backchannel, so go2rtc, Frigate
and Home Assistant's WebRTC card can talk through the camera speaker, as can `./akam talk`. It is offered
only when the video password is on. See [two-way audio](two_way_audio.md) (including configuration
examples and the one-client limit).

Port 8091 (`LIST`, `GET`: exposure, day/night, image, LEDs, state, system and stream metrics) is a plain text
protocol with no password. It accepts up to **4 clients at once**, one of them reserved for the camera itself (so
the SSH helpers always get in; a LAN peer can use 3 and is dropped after 2 minutes of silence), and is
**read-only from the network**: anyone on your LAN can read the state, nobody can change it. Changes
(`SET`) are accepted from the camera itself (over SSH, as `./akam led` does) or, for a limited time, after
`./akam debug on`, which the PC tuning tool needs. Raw changes are lost at the next boot; the settings made with `./akam led`, `audio`, `picture` (its four numbers) and `video` are saved on
the camera ([details](../specs/control-port.md)).

## Security notes

- **Never expose ports 554, 8091 or 22 to the Internet:** no port forwarding, UPnP or DMZ. Use a VPN into
  your LAN ([network hardening](#network-hardening)).
- The stream is open unless you set a password, and anyone on your network can occupy its single RTSP client
  slot (a second client gets `503 Service Unavailable` at once; several viewers: restream with go2rtc, see
  [troubleshooting](troubleshooting.md#only-one-viewer-works-at-a-time)): keep untrusted devices (guest
  Wi-Fi, IoT) off the camera's network.
- Port 8091 is read-only for the network. Its log stream (`LOG` lines: client addresses, auth failures) is sent
  only to the camera itself or while `akam_debug.sh` debug mode is on, and the Wi-Fi name and MAC address read
  as `hidden` for other machines (the IP address stays readable).
- A card with a file named `AKAM_MAINTENANCE` is root access to the camera
  ([details](manual/ssh-and-lockdown.md#7-maintenance-flag)). Never publish your backup, images or keys.
- **Firmware updates from a card are not signed.** With the `AKAM_MAINTENANCE` flag on the card, the stock
  `tf_update.sh` flashes any `*.bin` it finds, with no signature or hash check; without the flag it never runs
  (checked by `tests/lockdown_test.sh`). There is no network update. Treat the flag as physical access.
- **Saved configs:** a config holds no password unless you say yes when you save it (default No). Then the video password, the MQTT password and the Wi-Fi are in `global.secrets` / `camera.secrets` in the state folder, a private file in clear text: anyone with access to your account can read it. `./akam config forget-secrets` deletes them.
- **A backup holds secrets:** the CONFIG partition has your Wi-Fi password and the ONVIF password in clear text.
  The wizard creates the backup folder private to your user (0700, files 0600): keep copies private too and
  never upload them ([details](manual/backup-uart.md#never-publish-the-dump)).

## Network hardening

- **No port forwarding** from your router to the camera, for any port. Reach it remotely, if you must, through a VPN
  into your LAN.
- **DHCP reservation** so that the camera keeps its address for your recorder and your SSH configuration.
- **IoT VLAN or isolated Wi-Fi network** if your equipment supports it: let your recorder or viewer reach 554, your
  admin PC reach 22 and 8091, and nothing else.
- **Block outbound traffic** from the camera at the router. It needs only DHCP, and NTP (UDP 123 to your server) if you
  enabled it. It has no cloud code and should not talk to any cloud.
- Keep the private key `~/.ssh/akam` private, use a passphrase, and consider one key per camera.

## Licences

Akam is MIT ([LICENSE](../LICENSE), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). The image carries the
Dropbear licence text. Your built images contain your camera's stock files, so they are **not distributable**: never
publish them, your backups or any key. See [legal](legal.md).
