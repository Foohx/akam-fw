# MQTT and Home Assistant

Akam can publish the camera to an MQTT broker and take a few commands from it. Home Assistant finds the camera by itself
(MQTT discovery): one device with its sensors and controls. It is **optional and off by default**. Without it the camera
has no MQTT code running: no thread, no socket, no timer. The video stream (RTSP) and ONVIF work without it.

Status: **tested on the host only** (packet codec, discovery documents, a fake broker, the helper script, the assistant
and the wizard). It has not run on a real camera yet: the checks to make are in [section 8](#8-hardware-checks).

## 1. What it is

- A small MQTT 3.1.1 client inside `ak_rtsp` (about 1000 lines of C, MIT, no library). **The camera is never a broker.**
- **Outbound only.** The camera opens one TCP connection to the broker you give it. No new port is opened on the camera.
- **Plain TCP** (usually port 1883). TLS is not available yet (it is a separate, later step). The broker password therefore
  travels in clear on your network: use a broker on your own network and a user that only has the rights it needs.
- One extra thread, started only when MQTT is on. It never touches the video path. If the broker is down, the camera
  retries with a growing delay (5 s doubling to 60 s) and nothing else is affected.

## 2. Turn it on

You need a broker on your network (the Mosquitto add-on of Home Assistant, or your own Mosquitto) and the MQTT integration
of Home Assistant pointed at it. Then, from your computer:

```sh
./akam mqtt set        # or: akam mqtt set   (the native assistant)
```

It asks, in order: the broker address (an IPv4 address, or a host name your router's DNS resolves; names ending in `.local` do not work, the camera has no mDNS), the port (Enter = 1883), the
user name (empty = none), the password (not shown), and finally **"Turn MQTT on with these settings? [y/N]"** (Enter = No).
Nothing is sent before that yes. The password goes to the camera on the standard input of the SSH command, never in a
command line, a log or the saved state. Then it can run a connection test.

| Command | What it does |
|---|---|
| `mqtt status` | on or off, the broker, whether a password is set (never its value), and what `ak_rtsp` reports (`off`, `connecting`, `connected`, `error:<reason>`) |
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
`./akam mqtt forget`.

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
| Beep | button | `audio.out_test` | PRESS |
| Day/night state | sensor | `night.state` | day, night |
| Video viewers | sensor | `rtsp.clients` | count |
| Wi-Fi signal | sensor | `net.wifi_rssi` | dBm (only if the camera reports it) |
| Uptime, CPU load, Free memory | sensors (diagnostic) | `sys.uptime`, `sys.cpu`, `sys.mem_free` | s, %, kB |
| Temperature | sensor | `sys.temp` | degrees C (only if the camera reports it) |
| Frame rate, Video bitrate | sensors | `video.fps_actual`, `video.bitrate` | fps, kbit/s |
| Clock set | binary sensor (diagnostic) | `sys.time_set` | ON when the camera clock is believable (the camera has no clock of its own: see `./akam ntp`) |

The LED, IR, night mode and talk choices are saved on the camera like the same settings made with `./akam led`. To protect the flash, **at most one saved change is accepted per 10 seconds**: a second one inside the window is ignored (counted as rejected) and the entity is published again with its real value, so Home Assistant snaps back; wait 10 seconds and repeat. The
volume and the four picture sliders are applied **in RAM only** (a slider drag must not write flash many times): they go
back to the saved values at the next reboot; save them with `./akam audio volume N` or `./akam picture ...`.

### Topics

`BASE` is `akam/<hostname>` unless you set a base topic.

| Topic | Content |
|---|---|
| `BASE/status` | `online` (retained) while connected; `offline` (retained) when the camera stops or the connection breaks (the broker publishes it for the camera: it is the MQTT "last will") |
| `BASE/<entity>/state` | the value. Controls are retained (Home Assistant restores them); sensors are not |
| `BASE/<entity>/set` | commands (select, number, switch and button entities only) |
| `homeassistant/<component>/akam_<mac>/<entity>/config` | the retained discovery documents |

Controls are published at once when they change, also when the change was made elsewhere (`./akam led`, ONVIF, an app):
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

Then `./akam mqtt set` with your computer's address as the broker, port 1883, **no user name**. Watch what the camera
publishes:

```sh
mosquitto_sub -h 127.0.0.1 -t 'akam/#' -t 'homeassistant/#' -v
```

and send a command, for example to set the speaker volume to 30:

```sh
mosquitto_pub -h 127.0.0.1 -t 'akam/<camera-name>/volume/set' -m 30
```

When you are done: `./akam mqtt forget`, stop the broker, close the firewall port. An open broker with
`allow_anonymous true` lets anyone on the network command the camera: never leave it running.

## 6. Troubleshooting

`./akam mqtt test` names the problem. The reasons also appear in `./akam mqtt status` as `error:<reason>`:

| Reason | Meaning |
|---|---|
| `dns` | the name could not be resolved. Use the broker's IPv4 address (a `.local` name does not work: the camera has no mDNS) |
| `connect` | nothing answered at that address and port: wrong address, wrong port, broker down, firewall |
| `timeout` | the broker did not answer in time (also: a TLS port, where the broker waits for a handshake) |
| `closed` | the broker closed the connection (a user limit, a client id clash, an ACL) |
| `protocol` | the answer was not MQTT |
| `refused_credentials`, `refused_not_authorized` | wrong user or password, or the user has no rights |
| `refused_id`, `refused_protocol`, `refused_unavailable` | the broker refused the client id, the protocol version, or is not ready |

If the device does not appear in Home Assistant: the MQTT integration must use the same broker, and discovery must be on
(Settings, Devices, MQTT, Configure). The device shows as unavailable when the camera is off or disconnected (the
`offline` message above).

To remove the camera from Home Assistant after `./akam mqtt forget`, delete the device in its MQTT integration page.

## 7. Cost on the camera (measured on the host, build for the camera)

| | |
|---|---|
| `ak_rtsp` binary (stripped) | +39 KB with name resolution, **+23 KB** when built with `make EXTRA_CFLAGS=-DMQTT_NO_DNS` (then the broker must be an IPv4 address) |
| Static data (`bss`) | +3 KB (one 1.4 KB send buffer, one 0.9 KB entity state, the settings) |
| Thread | one, 64 KB stack reserved, only a few KB touched; lower priority than the video threads |
| CPU | a check of about 13 values every 2 s and a publish of about 10 values every 30 s: well under 0.1 % of the single core |
| Network | a few hundred bytes every 30 s, about 10 KB of discovery messages at each (re)connect |

See `specs/07-spec-standard-protocols.md` (status notes) for the exact figures and what is still to measure on hardware.

## 8. Hardware checks

These need a camera and a broker; they are listed in the [test plan](akam_v1_test_plan.md#mqtt-optional-needs-a-camera-and-a-broker).
