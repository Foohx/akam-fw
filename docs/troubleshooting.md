# Troubleshooting

Real problems met while installing and using Akam, with their fixes. Start with `./akam doctor`
(checks the tools on your PC) and `./akam status` (shows which step is done, todo or blocked). The
wizard keeps a log in `~/.local/state/akam/` (it never contains your Wi-Fi password).

Back to the [README](../README.md) or the [documentation index](README.md).

## Finding the camera's IP address

The camera gets its address by DHCP. Options:

- look at your router's list of clients; the Wi-Fi chip's maker shows up as **AltoBeam**;
- use the wizard's `find-camera` step (`./akam step find-camera`);
- reserve a fixed address for the camera in your router, so it never changes
  ([network hardening](akam_v1_install.md#5-network-hardening)).

In this documentation the address is always written `<camera-ip>`.

## "No medium found" with a USB card reader

The reader is detected but the card cannot be read (`No medium found`, an empty `lsblk` size).
Cheap readers often fail like this. Unplug and replug the reader, try another USB port, then another
reader (or a built-in SD slot). Check with `lsblk` that the card shows a size before going on. The SD
tool only accepts whole removable USB or SD disks, never a partition or a system disk.

## A multi-line paste ate a prompt

Pasting several lines into a terminal can feed the next line to a prompt before you read it. Paste
**one command at a time**. The wizard's confirmations are a red yes/no whose default is No, and it
refuses destructive actions without a real terminal, so a wrong or missing answer means nothing was
written: run the step again. When you run `tools/sd/sd_card.sh` by hand it still asks for a typed
`ERASE <device>`, and on the camera `install_with_flash_tool.sh` asks for `YES` alone on its line.

## The camera has no network after removing the card

A camera that was never paired with the vendor app has no `/etc/config/wpa_supplicant.conf`. With the
toolkit card in, the card brings the Wi-Fi up; without it the camera boots with no network, and
`./akam step verify` times out after the power cycle ("the camera is not on the network"). The
wizard creates that file from the card's Wi-Fi settings during `flash` and `verify` (and never asks you
to remove the card before), so this mostly happens after a manual install or when the card holds a
password the wizard cannot copy (it contains a `"`, a `\` or a line break, or is not 8 to 63 characters).

Fix: put the card back, power-cycle, wait two minutes, and either run `./akam step verify` again, or on
the camera (telnet 24, or SSH) run the same script the wizard uses:

```sh
sh /tmp/sd/preflight_check.sh --ensure-wifi     # or /mnt/preflight_check.sh; prints WIFI=CREATED or the reason
```

By hand (replace the two values; the file must be only readable by root):

```sh
umask 077
cat > /etc/config/wpa_supplicant.conf <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=1
network={
    ssid="YOUR_WIFI_NAME"
    psk="YOUR_WIFI_PASSWORD"
    key_mgmt=WPA-PSK
}
EOF
chmod 600 /etc/config/wpa_supplicant.conf
sync
```

Then power-cycle without the card. Also check your router: the camera's address may have changed.

## VLC shows a black screen

VLC sometimes shows a black screen while the camera is fine (a VLC run of 25 minutes without a drop was
validated on hardware, so a persistent black screen is worth investigating). To tell a camera problem
from a VLC one, check with `ffplay`:

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

If `ffplay` works, the camera is fine and the problem is in VLC: see the next sections.

## VLC goes black or stops when its window is on another workspace

Symptom: the picture is fine, you switch to another Hyprland (or other Wayland) workspace, and when
you come back VLC shows a black screen and stays stopped; the log says `buffer deadlock prevented`.

Cause: while the window is not visible, the compositor stops sending frame callbacks. VLC's video
output then blocks, VLC stops reading the network, and the camera drops the stalled client after 15
seconds **by design** (a client that does not read must not hold the only RTSP slot).

Fix: keep the VLC window visible (a second monitor, a floating or pinned window), or put a relay in
front of the camera (go2rtc or MediaMTX) and open the relay's stream in VLC. The relay keeps reading
from the camera whatever the player does.

## The stream stops after about 13 minutes (VLC, live555)

Players that send RTCP receiver reports over the same TCP connection (VLC and anything built on
live555) were dropped after about 13 minutes by older images. This is fixed in the firmware (the
interleaved RTCP input is now read and discarded properly): update your camera with `./akam update`
(or `./akam flash`) using an image built from a current kit.

## Only one viewer works at a time

The camera serves **one RTSP client at a time**; a second client is refused or the first one is cut.
A client that stays silent before PLAY is dropped after 20 s (60 s at most to reach PLAY), so a player
that cannot connect right after another one only has to wait that long.
To watch from several places, or to record and watch together, put a relay in front of the camera
(go2rtc or MediaMTX): the relay is the only client of the camera and serves as many viewers as you
need.

## Talk (two-way audio) does not work

`./akam talk` and the go2rtc / Home Assistant microphone button use the ONVIF audio backchannel on port 554.
The camera offers it only with the video password on (`./akam rtsp-auth status`), audio out enabled and
`audio.talk_enable=1`; otherwise it answers `551 Option not supported` and clients fall back to plain
video. A second client waits while another one is connected (the same single-client limit as above: let
go2rtc be the only client). The full list of causes, the counters to read (`audio.talk_*`) and the
configuration examples are in [two-way audio](two_way_audio.md).

## VLC cannot open the stream, or hardware decoding fails

- Missing plugins (seen on Arch): install `vlc-plugin-live555` (RTSP) and `vlc-plugin-ffmpeg`
  (H.264 decoding).
- Hardware decoding errors or garbage picture: turn it off with

  ```bash
  vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/
  ```

- The camera serves **one RTSP client at a time** (see above): close the other viewer first.

## 401 Unauthorized, or a password prompt on the stream

The stream is protected by a password when you (or the wizard, at your request) switched on the RTSP
login. Check it:

```bash
./akam rtsp-auth status
```

- **`401 Unauthorized`, or the player asks for credentials:** use
  `rtsp://<user>:<password>@<camera-ip>:554/`. VLC may also ask in a dialog; ffplay and ffmpeg want the
  credentials in the address.
- **Still 401 with the right password:** special characters in the password must be URL-encoded in the
  address: `@` is `%40`, `:` is `%3A`, `/` is `%2F`, `?` is `%3F`, `#` is `%23`, `%` is `%25`, a space is
  `%20`. The quickest test is a password with letters and digits only
  (`./akam rtsp-auth set`). The user name takes letters, digits and `. _ @ + -`.
- **Several wrong tries in a row:** the camera slows the answers down (up to 2 seconds) and closes the
  connection after 5 failures; wait a minute, then try again with the right password.
- **Forgot the password:** it cannot be read back (only a hash is stored). Over SSH,
  `./akam rtsp-auth disable` removes it, `./akam rtsp-auth set` sets a new one.
- **`INVALID` in `status`:** the credentials file is damaged. The camera then refuses every request
  until you run `./akam rtsp-auth set` or `disable`.
- **A player that was working stops after you changed the password:** expected; it is disconnected and
  must log in again with the new one.
- **`rtsp-auth` says the command is missing:** your image is older than this feature; flash a new one
  (`./akam update`). Without it the stream is open, as before.
- Frigate and go2rtc: put the credentials in the stream URL as above (URL-encoded).

## A transfer from the camera hangs or fails

During the backup the camera listens and your PC connects to it (the PC never needs an inbound
connection, so its firewall is not the cause). Check that the PC and the camera are on the same network,
that the router's "AP isolation" or "client isolation" is off, and that the camera still answers
(`ping <camera-ip>`). The wizard retries each file three times and checks size and hash. If it still
fails, the dumps are on the card: copy them by hand and pass the folder with `--backup-dir`
([manual backup](akam_v1_backup_sd.md#9-copy-to-the-pc-and-verify-there)).

## `nc` stays waiting on a Telnet session

When driving the maintenance Telnet by hand (`nc <camera-ip> 24`), `nc` does not exit by itself: it
waits for the camera to close the connection. That is normal. Press `Ctrl+C` once you have the
output you wanted. Telnet is only open while a maintenance card is in the camera
([maintenance flag](akam_v1_ssh_and_lockdown.md#7-maintenance-flag)); with no card it is closed on
purpose.

## GitHub error 5xx while downloading (bootstrap or `get-kit`)

A `502` or `503` from GitHub is temporary. The bootstrap already retries by itself; if it still
fails, wait a few minutes and run the step again (`./akam step get-kit`). With no network, use a kit
you already have (`./akam get-kit --kit FILE`) or run the bootstrap offline
(`bash tools/build/bootstrap.sh --offline`).

## The router still shows the old camera name

The camera announces its name (`akam-XXXX`, or the one set with `akam_hostname.sh set NAME`) only when
it asks the router for an address, and it is deliberately not restarted live. After a change, power-cycle
the camera; the router lists the new name after its next DHCP lease (some routers cache the old name
until the lease expires: release it in the router, or wait). `ssh root@<camera-ip> akam_hostname.sh
status` shows the effective, configured and kernel names.

## SSH says "REMOTE HOST IDENTIFICATION HAS CHANGED"

The camera keeps its SSH host key in the CONFIG partition. If CONFIG was erased (factory reset, a
restore), the camera made a new key. If you know why, remove the old entry:

```bash
ssh-keygen -R <camera-ip>
```

A CONFIG wipe also removes your key from the camera, so you need the first-boot card again. Details:
[host key change after a CONFIG wipe](akam_v1_ssh_and_lockdown.md#host-key-change-after-a-config-wipe).

## OpenSSH warns "not using a post-quantum key exchange algorithm"

Newer OpenSSH versions print this when the server (Dropbear on the camera) does not offer a
post-quantum key exchange. The connection still works and is still encrypted. It is harmless on a
LAN; ignore it. The wizard hides it (`ssh -o LogLevel=ERROR`, real errors are still shown); to hide it in
your own ssh commands add `-o LogLevel=ERROR`.

## Speaker output: test the camera speaker by ear (step A)

Work in progress (specs/05-spec-audio-out.md): this image makes `ak_rtsp` drive the camera speaker, with a
built-in beep to check it. It is on by default and follows the sequence of the stock firmware (mono, 8 kHz,
route 4, gain 2, taken from a capture of the stock application). It is **not validated end to end on
hardware yet**: an earlier build used a wrong ioctl number for the amplifier and stayed silent, this one
should be audible. The route and amplifier are switched on at start, then off again after 2 seconds of
silence, and on during a beep and for 2 seconds after. Replace `<cam>` by the camera address. Every
command is one line to `nc` on port 8091; the answer starts with `OK` or `ERR`.

```bash
# 1. Is it running?  (expect audio.out_state=idle; also the other audio.out_* lines: underruns, drops, buf_ms)
printf 'GET audio.out_state\n' | nc -w 2 <cam> 8091
printf 'LIST\n' | nc -w 2 <cam> 8091 | grep audio.out_

# 2. Beep: volume 30, gain 2, then three short 1 kHz tones (about 1 s)
printf 'SET audio.out_volume 30\nSET audio.out_gain 2\nSET audio.out_test 1\n' | nc -w 3 <cam> 8091

# 3. Silence or a click only? Check the [audio_out] log lines for "failed", raise the gain, then replay.
#    Source 4 is the stock route and the default; 6 is an experiment, put 4 back if it changes nothing.
printf 'SET audio.out_gain 5\nSET audio.out_test 1\n' | nc -w 3 <cam> 8091
printf 'SET audio.out_source 6\nSET audio.out_test 1\n' | nc -w 3 <cam> 8091

# 4. Levels: hardware gain 0..5 at volume 100, then software volume 100, 50, 25, 10, 0
for g in 0 1 2 3 4 5; do printf 'SET audio.out_volume 100\nSET audio.out_gain %s\nSET audio.out_test 1\n' "$g" | nc -w 2 <cam> 8091; sleep 3; done
for v in 100 50 25 10 0; do printf 'SET audio.out_gain 2\nSET audio.out_volume %s\nSET audio.out_test 1\n' "$v" | nc -w 2 <cam> 8091; sleep 3; done

# 5. Amplifier hiss: hold your ear near the speaker in a quiet room
printf 'SET audio.out_amp on\n'   | nc -w 2 <cam> 8091   # amplifier always on: listen for hiss
printf 'SET audio.out_amp auto\n' | nc -w 2 <cam> 8091   # default: route and amplifier off 2 s after the last sound
```

What to note: the combination that is audible (source, gain), the comfortable gain and volume, hiss with
`on`, any pop when the amplifier switches, and whether the video and microphone of the RTSP stream stay
normal while a beep plays (the beep itself is heard on the stream, about a second late).

To see what the camera did, read the log lines on the same port (`nc <cam> 8091` and wait) or the serial
console: lines starting with `[audio_out]` show the device calls and every error.

- **`audio.out_state=disabled_crash`:** the previous start crashed while opening the speaker, so audio out
  stays off to avoid a reboot loop. Over SSH: `rm /etc/config/akam/audio_out.trying`, then restart `ak_rtsp`
  (or reboot) to try again. Send the log lines to the project if it happens twice.
- **A `SET_SPEAKER(0)` line in the log:** the stock firmware never switches the speaker off, only the route
  (`SET_SOURCES 0`), so a refused `SET_SPEAKER(0)` is logged once and is harmless; the muted route is
  what silences the speaker.
- **`audio.out_state=error`:** a device call failed; the `[audio_out]` lines say which. Video and microphone
  carry on. If it failed at start, the old silence path is used; if it failed later, the speaker stays
  silent until `ak_rtsp` restarts.
- **Saved settings:** `audio.out_volume`, `audio.out_gain` and `audio.out_source` are saved like `enable`
  (only when the value changes), so the last tuning survives a restart. `audio.out_amp` is not saved.
- **Turn it off:** `printf 'SET audio.out_enable 0\n' | nc -w 2 <cam> 8091` silences it at once and the choice is
  saved in `/etc/config/akam/audio_out` (`enable=0`), so it stays off after a restart. `SET audio.out_enable 1`
  turns it back on; the answer says `restart to apply` when `ak_rtsp` started with it off.
- **`ERR audio out is not running`:** it was disabled or failed at boot; see `audio.out_state`.
- **Sound over the network:** `./akam talk test`, `./akam talk` and `./akam say FILE` send sound to this speaker
  through the RTSP port ([two-way audio](two_way_audio.md)). A client that streams G.711 silence does not
  switch the amplifier on: the amp follows sound above about -56 dBFS.

## Still stuck

Run `./akam status`, read the end of the log in `~/.local/state/akam/`, and open an issue on the
project's repository with the step name and the (redacted) log. Never attach your backup or keys. For
a camera that no longer boots, see [recovery](akam_v1_flash_and_restore.md#5-restore).
