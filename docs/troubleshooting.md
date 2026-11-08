# Troubleshooting

Real problems met while installing and using Akam, with their fixes. Start with `./akam doctor`
(checks the tools on your PC) and `./akam status` (shows which step is done, todo or blocked). The
wizard keeps a log in `~/.local/state/akam/` (it never contains your Wi-Fi password).

Back to the [README](../README.md).

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

## A multi-line paste ate the `YES` prompt

Pasting several lines into a terminal can feed the next line to a prompt (a `YES` or `ERASE <device>`
question) before you read it. Paste **one command at a time**. The wizard refuses destructive actions
without a real terminal and wants `YES` or `ERASE <device>` typed by you, so a failed prompt means
nothing was written: run the step again. On the camera, type `YES` alone on its line.

## VLC shows a black screen

VLC sometimes shows a black screen while the camera is fine. A fix for the RTP timestamps is being
confirmed on hardware and is not validated yet. To tell a camera problem from a VLC one, check with
`ffplay`:

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

If `ffplay` works, the camera is fine and the problem is in VLC: see the next two sections.

## VLC cannot open the stream, or hardware decoding fails

- Missing plugins (seen on Arch): install `vlc-plugin-live555` (RTSP) and `vlc-plugin-ffmpeg`
  (H.264 decoding).
- Hardware decoding errors or garbage picture: turn it off with

  ```bash
  vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/
  ```

- The camera serves **one RTSP client at a time**. Close the other viewer first, or put go2rtc or
  MediaMTX in front of the camera.

## Transfers from the camera never arrive (PC firewall)

During the backup the camera sends files to your PC over the network. A PC firewall that blocks
inbound connections makes the transfer hang or fail. Allow the incoming connection from the camera's
address for the duration of the backup (or pause the firewall), then turn it back on. The manual way
of getting files without a network transfer is in the [backup guide](akam_v1_backup.md).

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
LAN; ignore it.

## Still stuck

Run `./akam status`, read the end of the log in `~/.local/state/akam/`, and open an issue on the
project's repository with the step name and the (redacted) log. Never attach your backup or keys. For
a camera that no longer boots, see [Recovery](../README.md#recovery).
