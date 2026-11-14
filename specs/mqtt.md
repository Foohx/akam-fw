# MQTT and Home Assistant

`ak_rtsp` contains a small outbound MQTT 3.1.1 client that publishes the camera to a broker, announces it to Home
Assistant through MQTT discovery, and accepts a fixed list of commands. It is **optional and off by default**. While off
there is no thread, socket, timer or pipe.

Status: **host-tested only** (packet codec with fuzzing, discovery documents, a fake broker, the helper script, the
installers). It has never run on a real camera or against a real broker. The checks to make are in
`docs/akam_v1_test_plan.md`. MQTT over TLS is not implemented.

## 1. Properties

- The camera is only a client. It never runs a broker and opens no new listening port.
- One outbound plain-TCP connection to `host:port`. **No TLS**: the broker password travels in clear on the network.
  Use a broker on a trusted network and a user with minimal rights.
- One thread (stack 64 KiB reserved, a few KB touched), lower priority than the video threads. It never touches the video
  path. A dead broker costs one failed connect per backoff period.
- Codec: MQTT 3.1.1. The camera connects (with a will), publishes at QoS 0 (retained or not), subscribes at QoS 0, answers
  a QoS 1 delivery with PUBACK, sends PINGREQ and DISCONNECT. Everything else a broker may send is ignored or rejected.
- Cost measured on the host build: about +39 KB stripped (+23 KB with `-DMQTT_NO_DNS`, then the broker must be an IPv4
  address), +3 KB bss. RSS on the camera is not measured.

## 2. Configuration

Two files in `/etc/config/akam/` (both mode 0600). Written by `akam_mqtt.sh`, never by the control port.

`mqtt`: key=value lines, LF (a trailing CR is tolerated), at most 512 bytes, any order, no repeated key, an unknown key
makes the file invalid:

| Key | Values | Default |
|---|---|---|
| `enabled` | 0 or 1 | required |
| `host` | host name or IPv4 address (`A-Z a-z 0-9 . -`), 1 to 63 characters | required. `.local` names do not work (no mDNS) |
| `port` | 1 to 65535 | 1883 |
| `user` | 0 to 32 of `A-Z a-z 0-9 . _ @ + -` | none |
| `client_id` | 1 to 32 of `A-Z a-z 0-9 _ -` | the host name |
| `base_topic` | `A-Z a-z 0-9 _ - /`, 1 to 64, no leading, trailing or double `/` | `akam/<hostname>` |
| `discovery` | `on` or `off` | `on` |
| `discovery_prefix` | same character rules, 1 to 32 | `homeassistant` |
| `interval` | 10 to 300 (seconds between sensor publishes) | 30 |

`mqtt_pass`: one line, at most 128 printable characters, read only when a connection is made, never logged, never
published. An absent file means no password. A file that other users can read is refused (the module stays off, with a
log line).

An absent or invalid `mqtt` file, or `enabled=0`, means OFF. `MQTT reload` (control port, loopback only) re-reads both
files and starts, stops or restarts the thread; `MQTT test` makes one short-lived connection with client id
`<id>-test`, waits for CONNACK and disconnects, and works while the service is off.

`akam_mqtt.sh` (run over SSH): `set HOST PORT USER` (the password on stdin), `option NAME VALUE`, `disable`, `forget`
(delete both files), `status`, `test`. Each change ends with `ak_rtsp ctl MQTT reload`.

## 3. Connection behaviour

- State (`GET mqtt.state`): `off`, `connecting`, `connected`, `error:<reason>`. `error:*` is the reason of the last
  failed attempt while the thread backs off and retries.
- Backoff: 5 s, doubling to 60 s, spread by 20 %. A session must last 60 s before the backoff starts again from its
  minimum. MQTT keep-alive 60 s; a PINGREQ or the CONNECT may stay unanswered for 10 s. The change scan runs every 2 s.
- Reasons: `dns`, `connect`, `timeout`, `closed`, `protocol`, `mac` (the Wi-Fi MAC is not known yet: nothing is published
  until it is, retried automatically), `config` (the topics or the password file are unusable), `subscribe` (the broker refused the command subscription), `refused_protocol`,
  `refused_id`, `refused_unavailable`, `refused_credentials`, `refused_not_authorized`, `refused`.
- `GET mqtt.stats`: `published=N received=N applied=N rejected=N connects=N`.
- At stop the client publishes `offline`, sends DISCONNECT and joins the thread.

## 4. Topics

`BASE` is the base topic. `NODE` is `akam_<mac without colons>`. `PREFIX` is the discovery prefix.

| Topic | Content |
|---|---|
| `BASE/status` | `online` (retained) while connected; `offline` (retained), also the will |
| `BASE/<entity>/state` | State of the entity. Controls are retained (Home Assistant restores them); sensors are not |
| `BASE/<entity>/set` | Command topic (select, number, switch, button only) |
| `PREFIX/<component>/NODE/<entity>/config` | Retained discovery document |

The only subscription is `BASE/+/set`. One device is announced, named after the host name and identified by the Wi-Fi MAC
(so it merges with the ONVIF integration's device). A discovery document holds the name, model, firmware version and MAC;
never a password, Wi-Fi name or IP address. An entity that the camera does not have (the optional ones below) gets an
empty retained payload on its config topic, which removes a document an earlier run left in the broker.

Controls are published when they change, whoever changed them (the camera checks about every 2 s). Sensors are published
every `interval` seconds.

## 5. Entities

| Entity id | Type | Camera key | Values | Saved on a command |
|---|---|---|---|---|
| `status_led` | select | `led.status` | on, off, auto | yes |
| `ir_led` | select | `night.ir_led` | auto, off | yes |
| `night_mode` | select | `night.mode` | auto, day, night | yes |
| `volume` | number | `audio.out_volume` | 0 to 100 (%) | no (RAM) |
| `brightness`, `contrast`, `saturation`, `sharpness` | number | `isp.*` | -50 to 50 | no (RAM) |
| `talk` | switch | `audio.talk_enable` | ON, OFF | yes |
| `beep` | button | `audio.out_test` | PRESS | no |
| `night_state` | sensor | `night.state` | day, night | |
| `clock_set` | binary sensor (diagnostic) | `sys.time_set` | ON when the clock is believable | |
| `viewers` | sensor | `rtsp.clients` | count | |
| `wifi_signal` | sensor, optional (diagnostic) | `net.wifi_rssi` | dBm | |
| `uptime` | sensor (diagnostic) | `sys.uptime` | s | |
| `cpu` | sensor (diagnostic) | `sys.cpu` | % | |
| `mem_free` | sensor (diagnostic) | `sys.mem_free` | kB | |
| `temperature` | sensor, optional (diagnostic) | `sys.temp` | degrees C | |
| `fps` | sensor | `video.fps_actual` | fps (tenths) | |
| `bitrate` | sensor | `video.bitrate` | kbit/s | |

"Optional" entities exist only when the camera reports the value. A volume or picture command changes RAM only: a
slider drag must not write flash many times, and a later saved change of another setting does not make it permanent.

## 6. Security

- Who can command the camera is whoever can publish to `BASE/+/set` on the broker. Protect the broker with users and
  topic permissions.
- The command list is fixed in the firmware (the table above). Nothing reboots the camera or changes the network, the host
  name or any password. A message for another name is ignored and counted.
- Every payload is validated before the setter runs: a select value must be one of the listed words, a number a whole
  number inside its range, a switch `ON` or `OFF`, a button `PRESS`. Anything else, longer than 23 bytes, or with control
  characters is ignored and counted (`rejected`).
- A command with the broker's retain flag is never obeyed (it would replay at every reconnect).
- The password is not in the log, in `status`, in discovery messages or on any command line.

### Rate limits

All are token buckets (they survive a reconnect, so reconnecting does not refill them). A command refused by a limit is
counted in `rejected` and not applied.

| Limit | Rule |
|---|---|
| Every command | Burst of 10, then 5 per second. An excess command is dropped without a state publish, so a flood cannot make the camera publish back |
| Saved (flash) changes: LED, IR, night mode, talk | Burst of 4, then one more every 30 s (at most about 2900 writes a day). A refused one is published again with the real value, so Home Assistant snaps back |
| Beep | At most one per 2 s |

A command that asks for the value the camera already has is acknowledged without writing and costs no token.

## 7. Tests

`tests/mqtt_codec_test.sh`, `mqtt_ha_test.sh`, `mqtt_svc_test.sh` (a fake broker; AddressSanitizer and UBSan),
`akam_mqtt_test.sh`, `wizard_mqtt_test.sh`, `control_test.sh`, `hwcheck_test.sh` (`MQTT is on: ak_rtsp holds one outbound
TCP connection`).
