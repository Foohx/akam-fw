# MQTT and Home Assistant

*What this page is for: connecting the camera to Home Assistant or another home automation system. Skip it if you
only want to watch the video.*

> **Two cameras, the same commands.** Turn it on with `akam mqtt set` (then `mqtt test`, `status`, `disable`, `forget`) on either camera.
> Sections 1 to 8 describe the **LSC Indoor 1080p** (`akamd`, `/etc/config/akam/mqtt`, the entity table of section 3). The
> **LSC Rotatable 2K** has its own files and its own entities: see [LSC Rotatable 2K](#lsc-rotatable-2k) at the end.

**MQTT** is a light messaging protocol for home automation; a **broker** (for example Mosquitto) is the server that
passes the messages around. Akam can publish the camera to an MQTT broker and take a few commands from it. Home Assistant finds the camera by itself
(MQTT discovery): one device with its sensors and controls. It is **optional and off by default**. Without it the camera
has no MQTT code running: no thread, no socket, no timer. The video stream (RTSP) and ONVIF work without it.

Status (both cameras): **tested on the host only** (packet codec, discovery documents, a fake broker, the helper script, the assistant
and the wizard). It has not run on a real camera yet: the checks to make are in [section 8](#8-hardware-checks).

## 1. What it is

- A small MQTT 3.1.1 client inside `akamd` (about 1000 lines of C, MIT, no library). **The camera is never a broker.**
- **Outbound only.** The camera opens one TCP connection to the broker you give it. No new port is opened on the camera.
- **Plain TCP** (usually port 1883). TLS is not available yet (it is a separate, later step). The broker password therefore
  travels in clear on your network: use a broker on your own network and a user that only has the rights it needs.
- One extra thread, started only when MQTT is on. It never touches the video path. If the broker is down, the camera
  retries with a growing delay (5 s doubling to 60 s) and nothing else is affected.

## 2. Turn it on

You need a broker on your network (the Mosquitto add-on of Home Assistant, or your own Mosquitto) and the MQTT integration
of Home Assistant pointed at it. Then, from your computer:

```sh
akam mqtt set
```

It asks, in order: the broker address (an IPv4 address, or a host name your router's DNS resolves; names ending in `.local` do not work, the camera has no mDNS), the port (Enter = 1883), the
user name (empty = none), the password (not shown), and finally **"Turn MQTT on with these settings? [y/N]"** (Enter = No).
Nothing is sent before that yes. The password goes to the camera on the standard input of the SSH command, never in a
command line, a log or the saved state. Then it can run a connection test.

| Command | What it does |
|---|---|
| `mqtt status` | on or off, the broker, whether a password is set (never its value), and what `akamd` reports (`off`, `connecting`, `connected`, `error:<reason>`) |
| `mqtt set` | asks, then saves the settings and turns MQTT on at once (no reboot) |
| `mqtt test` | one connection attempt with the saved settings; says why it failed (wrong address, refused login, not MQTT...). Works while MQTT is off |
| `mqtt disable` | turns MQTT off (the settings and the password stay for later) |
| `mqtt forget` | turns it off and deletes the settings and the password file (asks first, default No) |

In the assistant's **Camera options > Network** screen there is a "MQTT / Home Assistant" entry that does the same.

Optional settings (base topic, client id, discovery on/off, discovery prefix, sensor interval) are changed on the camera
with `akam_mqtt.sh option NAME VALUE` over SSH; the defaults suit Home Assistant and need no change.

### Files on the camera

| File | Content |
|---|---|
| `/etc/config/akam/mqtt` (mode 0600) | `enabled=1`, `host=`, `port=`, `user=`, optional `client_id=`, `base_topic=`, `discovery=on\|off`, `discovery_prefix=`, `interval=` |
| `/etc/config/akam/mqtt_pass` (mode 0600) | the broker password alone on one line; absent = no password. A password file that other users can read is ignored |

An invalid or missing file means MQTT stays off. The files survive reflashes of the APP partition. To remove everything:
`akam mqtt forget`.

## 3. What Home Assistant gets

One device named after the camera (`akam-XXXX`), identified by the Wi-Fi MAC address (so it merges with the ONVIF
integration's device when both are used). The MAC is part of the discovery messages, which go only to your broker.

| Entity | Type | Camera key | Values |
|---|---|---|---|
| Status LED | select | `led.status` | on, off, auto |
| IR LEDs | select | `night.ir_led` | auto, off |
| Day/night mode | select | `night.mode` | auto, day, night |
| Speaker volume | number | `audio.out_volume` | 0 to 100 |
| Brightness, Contrast, Saturation, Sharpness | number | `isp.*` | -50 to 50 |
| Talk enabled | switch | `audio.talk_enable` | ON, OFF |
| Image flip, Image mirror | switch | `image.flip`, `image.mirror` | ON, OFF (vertical, horizontal; saved on the camera; not yet hardware-validated on the 1080p) |
| Beep | button | `audio.out_test` | PRESS |
| Day/night state | sensor | `night.state` | day, night |
| Video viewers | sensor | `rtsp.clients` | count |
| Wi-Fi signal | sensor | `net.wifi_rssi` | dBm (only if the camera reports it) |
| Uptime, CPU load, Free memory | sensors (diagnostic) | `sys.uptime`, `sys.cpu`, `sys.mem_free` | s, %, kB |
| Temperature | sensor | `sys.temp` | degrees C (only if the camera reports it) |
| Frame rate, Video bitrate | sensors | `video.fps_actual`, `video.bitrate` | fps, kbit/s |
| Clock set | binary sensor (diagnostic) | `sys.time_set` | ON when the camera clock is believable (the camera has no clock of its own: see `akam ntp`) |

The LED, IR, night mode and talk choices are saved on the camera like the same settings made with `akam led`. The
volume and the four picture sliders are applied **in RAM only** (a slider drag must not write flash many times): they go
back to the saved values at the next reboot, and a later saved change of another setting does not make them permanent; save
them with `akam audio volume N` or `akam picture ...`.

**Rate limits** (all are token buckets; a command refused by a limit is counted in `rejected` and never applied):

| Limit | Rule |
|---|---|
| Every command (any entity) | burst of 10, then 5 per second. An excess command is dropped **without** a state publish, so a flood cannot make the camera publish back |
| Saved (flash) changes: LED, IR, night mode, talk | burst of 4, then one more every 30 seconds. A Home Assistant scene that sets all four fits in the burst. A refused one is published again with its real value, so Home Assistant snaps back |
| Beep | at most one every 2 seconds |

A command that asks for the value the camera already has is acknowledged without writing anything and costs no token.
The limits survive a reconnect (a new session does not refill the buckets), so reconnecting is no way around them.

### Topics

`BASE` is `akam/<hostname>` unless you set a base topic.

| Topic | Content |
|---|---|
| `BASE/status` | `online` (retained) while connected; `offline` (retained) when the camera stops or the connection breaks (the broker publishes it for the camera: it is the MQTT "last will") |
| `BASE/detections` | **2K only.** The boxes of the people in view, JSON, not retained, QoS 0 (see below). No Home Assistant entity belongs to it |
| `BASE/<entity>/state` | the value. Controls are retained (Home Assistant restores them); sensors are not |
| `BASE/<entity>/set` | commands (select, number, switch and button entities only) |
| `homeassistant/<component>/akam_<mac>/<entity>/config` | the retained discovery documents |

**`BASE/detections`** carries the same JSON as the `detections` event of the local control API (see
[control-api.md](porting/lsc-rotatable-2k/control-api.md), section 6): `{"ts_ms":..,"w":320,"h":192,"pan":..,"tilt":..,"boxes":[{"x":..,"y":..,"w":..,"h":..,"score":..,"id":..,"target":..,"ignored":..}]}`,
the boxes in per-mille of the frame. It is published when the boxes change and every second while a person is in view, at most `detect.publish_hz` times a second (default 2, 0 = off; set it with the API;
never more than 5 a second), and once with `"boxes":[]` when the last person has left. Nothing while the camera is parked or the privacy mode hides the video. It exists only while MQTT is on and
connected (lists made while the broker is away are dropped, never queued) and has **no discovery document**: Home Assistant does not create an entity for it; subscribe from an automation, Node-RED or your own script,
for example `mosquitto_sub -t 'akam/<hostname>/detections' | jq -c '.boxes[]'`. Nothing is written to the camera's flash.

Controls are published at once when they change, also when the change was made elsewhere (`akam led`, ONVIF, an app):
the camera checks them every 2 seconds. Sensors are published every 30 seconds.

## 4. Security

- **Who can command the camera is whoever can publish to `BASE/+/set` on your broker.** Protect the broker with users and
  topic permissions (ACL) and, if you can, give the camera its own user that may publish `BASE/#` and read `BASE/+/set`
  only. Anyone with publish rights can change the LEDs, the volume, the picture and play the beep: an annoyance, not a
  danger; there is nothing in the list that reboots the camera, changes the network, the hostname, the stream password or
  any other password.
- The list of commands is **fixed in the firmware** (the table above). A message for any other name is ignored and counted.
- Every command payload is checked before anything happens: select values must be one of the listed words, numbers must be
  whole numbers inside their range, switches `ON` or `OFF`, the button `PRESS`; anything else, longer than 23 bytes, or with
  control characters is ignored. A command with the broker's **retain flag** is ignored (it would replay at every
  reconnect).
- The password is read from its file when the connection is made. It is not in the log, not in `status`, not in the
  discovery messages, and not on any command line.
- Discovery messages hold the camera name, the model, the firmware version and the MAC address; no password, Wi-Fi name or
  IP address.

## 5. Try it without a broker (a temporary Mosquitto on your computer)

This is **documentation only**: nothing here is installed by Akam. If you have no broker yet and want to see the
entities, run a throw-away Mosquitto on your own computer, on the same network as the camera.

With Docker or Podman:

```sh
mkdir -p /tmp/mosq && cat > /tmp/mosq/mosquitto.conf <<'EOF'
listener 1883
allow_anonymous true
EOF
docker run --rm -p 1883:1883 -v /tmp/mosq/mosquitto.conf:/mosquitto/config/mosquitto.conf eclipse-mosquitto:2
```

Without a container tool, install Mosquitto from your system's package manager on your computer, put the same two lines
in a file and run `mosquitto -c that-file`. Open port 1883 on your computer's firewall for your local network only.

Then `akam mqtt set` with your computer's address as the broker, port 1883, **no user name**. Watch what the camera
publishes:

```sh
mosquitto_sub -h 127.0.0.1 -t 'akam/#' -t 'homeassistant/#' -v
```

and send a command, for example to set the speaker volume to 30:

```sh
mosquitto_pub -h 127.0.0.1 -t 'akam/<camera-name>/volume/set' -m 30
```

When you are done: `akam mqtt forget`, stop the broker, close the firewall port. An open broker with
`allow_anonymous true` lets anyone on the network command the camera: never leave it running.

## 6. Troubleshooting

`akam mqtt test` names the problem. The reasons also appear in `akam mqtt status` as `error:<reason>`:

| Reason | Meaning |
|---|---|
| `dns` | the name could not be resolved. Use the broker's IPv4 address (a `.local` name does not work: the camera has no mDNS) |
| `connect` | nothing answered at that address and port: wrong address, wrong port, broker down, firewall |
| `timeout` | the broker did not answer in time (also: a TLS port, where the broker waits for a handshake) |
| `mac` | the camera does not know its Wi-Fi MAC address yet (Wi-Fi not up): nothing is published until it does, retried automatically |
| `subscribe` | the broker refused the command subscription (an ACL that forbids `BASE/+/set`) |
| `closed` | the broker closed the connection (a user limit, a client id clash, an ACL) |
| `protocol` | the answer was not MQTT |
| `refused_credentials`, `refused_not_authorized` | wrong user or password, or the user has no rights |
| `refused_id`, `refused_protocol`, `refused_unavailable` | the broker refused the client id, the protocol version, or is not ready |

If the device does not appear in Home Assistant: the MQTT integration must use the same broker, and discovery must be on
(Settings, Devices, MQTT, Configure). The device shows as unavailable when the camera is off or disconnected (the
`offline` message above).

To remove the camera from Home Assistant after `akam mqtt forget`, delete the device in its MQTT integration page.

## 7. Cost on the camera (measured on the host, build for the camera)

| | |
|---|---|
| `akamd` binary (stripped) | +39 KB with name resolution, **+23 KB** when built with `make EXTRA_CFLAGS=-DMQTT_NO_DNS` (then the broker must be an IPv4 address) |
| Static data (`bss`) | +3 KB (one 1.4 KB send buffer, one 0.9 KB entity state, the settings) |
| Thread | one, 64 KB stack reserved, only a few KB touched; lower priority than the video threads |
| CPU | a check of about 13 values every 2 s and a publish of about 10 values every 30 s: well under 0.1 % of the single core |
| Network | a few hundred bytes every 30 s, about 10 KB of discovery messages at each (re)connect |

See `specs/audio.md` (status notes) for the exact figures and what is still to measure on hardware.

## 8. Hardware checks

These need a camera and a broker; they are the M checks of the [hardware checklists](hardware-test-checklists.md#mqtt-optional-needs-a-camera-and-a-broker).

## LSC Rotatable 2K

The 2K runs the same MQTT client design (outbound only, plain TCP, no broker on the camera, one Home Assistant device, off
until you configure it) inside `akamd`. Status: host tests only (golden discovery documents, fuzzed parser, fake broker);
not run on the camera.

**Turn it on.** `akam mqtt set` (through the signed API, the admin key of `key-2k` is needed; applies at the next restart of the app). By hand: two files
in `/config/akam` written over SSH, then restart the app: [using the 2K](porting/lsc-rotatable-2k/using-the-2k.md#2-mqtt-and-home-assistant).

| File | Content |
|---|---|
| `/config/akam/mqtt` | `key=value` lines, strict (an unknown key or a bad value makes the file invalid, MQTT stays off): `enabled=0\|1` and `host=` required; `port=` (1883), `user=`, `client_id=` (`akam-rot2k`), `base_topic=` (`akam/<hostname>`), `discovery=on\|off`, `discovery_prefix=` (`homeassistant`), `interval=` (10 to 300 s, default 30) |
| `/config/akam/mqtt_pass` | the broker password alone on one line; absent = none; ignored without a `user=` |

The broker address is an IPv4 address or a host name your DNS resolves (no mDNS). The password is never logged or published.

**What Home Assistant gets** (discovery only; the device is named after the camera and identified by its Wi-Fi MAC): person
presence, last detection score, video viewers and the firmware version; one presence sensor per detection zone; a JPEG snapshot
image when a person appears; the day/night state; the diagnostic system sensors; the status LED and IR LED selects and the LED
rule patterns; the PTZ buttons and position sensors; the video settings (bit rate, frame rate, ...); the controls of person
tracking (switch, pan-only switch), the zone and tracking-area texts; the front button as an event and as device triggers; and
the three picture switches **Image flip**, **Image mirror** and **Ceiling mount** (host-tested only, not verified on the camera:
[checklist](hardware-test-checklists.md#2k-picture-orientation-flip-mirror-ceiling-mount)). Commands go to `BASE/<entity>/set`,
states to `BASE/<entity>/state` (payloads are validated by the camera). The exact entity list is in
`platforms/anyka-ak3918av130/mqtt_cfg.h`.

**`BASE/detections`** (section 3, topics) exists only on the 2K: the boxes of the people in view, same JSON as the `detections`
event of the [2K API](porting/lsc-rotatable-2k/control-api.md).

**No speaker entities on the 2K yet** (the speaker itself works: `akam audio`, `akam talk`): the 1080p entities of section 3 that depend on the speaker (volume, talk, beep) and the 1080p's `akam_mqtt.sh`
options. The security advice of section 4 applies: whoever can publish to `BASE/+/set` on your broker commands the camera.
