# akam: the assistant as a single binary (experimental, phase P1)

`akam` is the Go version of the guided setup program (`./akam`, bash, in the repository root). It is one
static executable with no runtime dependency: SSH, the control port, the RTSP talk client, G.711 and
WAV are built in. The plan is in [specs/09-spec-assistant-binary.md](../../specs/09-spec-assistant-binary.md).

**Status: phase P1, experimental.** It does the daily commands on a camera that already runs Akam. Backup,
image, flash, update, restore, the SD card steps and the telnet route stay in the bash wizard until phases
P2 and P3. Nothing in P1 flashes, reboots or power-cycles a camera. It was tested on a fake camera and
against the bash wizard's own output, **not yet on a real camera**.

## Build

Needs Go 1.26 or newer (`GO_VERSION` in `tools/build/deps.lock`; a host Go at least that minor is used as is, nothing
is installed system-wide). The modules are not stored in git: run `bash tools/build/bootstrap.sh` once (it does
`go mod download`, checked against `go.sum`); `build.sh` and the tests then work offline. Nothing is written
outside the repository: the module and build caches live in `tools/.toolchain/go/` (git-ignored).

```bash
tools/assistant/build.sh          # this computer  -> tools/assistant/bin/akam
tools/assistant/build.sh all      # linux/amd64 and linux/arm64 -> tools/assistant/bin/linux-<arch>/akam
tools/assistant/build.sh test     # go vet and go test ./...
tools/assistant/build.sh repro    # two builds, same sha256
VERSION=1.0.0 tools/assistant/build.sh   # version shown by `akam version`
```

`make -C tools/assistant host|all|test` calls the same script. The build is static (`CGO_ENABLED=0`), uses
`-trimpath -buildvcs=false -ldflags "-s -w"`, and checks the 12 MiB size budget of the spec (about 6.3 MiB for
amd64 and 5.9 MiB for arm64, start in about 2 ms).

## Use

The commands and options have the names and the texts of `./akam help` (`akam help` prints the same text,
plus the three commands that only exist here). From a source checkout, the bash script can hand over to the
binary for a side-by-side comparison:

```bash
AKAM_USE_BIN=1 ./akam led            # runs tools/assistant/bin/akam led (or $AKAM_BIN)
tools/assistant/bin/akam --ip 192.168.1.50 audio volume 30
```

Both programs keep their progress in the same folder (`~/.local/state/akam`, `AKAM_STATE_DIR`,
`--state-dir`) with the same file format, so you can use one after the other. **Do not run both at once**
(the binary takes a lock; the script does not). The first time the binary opens a folder that already holds a
state file, it keeps one untouched copy as `state.bak-before-binary`.

| Command | What it does in this phase |
|---|---|
| `help`, `version`, `self-test` | the text, the version, offline checks (G.711, Digest, state file, the safety rule below) |
| `status`, `cameras`, `rename-camera NEW`, `register` | progress table, camera list, rename, and record a camera that already runs Akam (`--ip`, optional `--backup-dir`) |
| `find-camera` | asks the address (or a name: DNS, then mDNS `<name>.local`), probes the four ports in parallel and tells stock + toolkit from Akam. With no address typed, two opt-in searches, each behind its own yes/no (default No): ONVIF discovery (one UDP packet to 239.255.255.250:3702) and a scan of your local /24 (only the four camera ports) |
| `led`, `audio`, `picture`, `debug`, `onvif` | the same settings, checks and messages as the bash commands; control-port lines go through an SSH forward to the camera's own loopback |
| `rtsp-auth [set\|disable\|status]` | the password is typed twice without echo and sent on stdin to `akam_rtsp_auth.sh set-creds` |
| `hostname [NAME]` | `akam_hostname.sh set NAME` (a note only on an older image) |
| `talk [SECONDS\|test]`, `say FILE` | RTSP + Digest + ONVIF audio backchannel + G.711 in Go; `talk test` and `say file.wav` need no other program (any rate or channel count; 8/16/24/32-bit PCM or float). The microphone and files that are not WAV need `arecord` or `ffmpeg` (optional, found automatically, nothing else uses them) |
| `forget-host-key IP` | forget a camera's pinned SSH host key (asks first) |
| `doctor` | what this program found on your computer (optional tools included) |
| menu (`akam` alone) | on a terminal, a full-screen menu (arrows or `j`/`k`, Enter, `?`, `q`); with `--plain`, `AKAM_PLAIN=1` or without a terminal, the status table |

Not in this phase (the command says so and points to the bash wizard): `get-kit`, `build`,
`prepare-backup-sd`, `backup`, `image`, `prepare-flash-sd`, `flash`, `verify`, `update`, `restore`, `wipe-sd`.

### SSH and host keys

Login is `root` with the key `~/.ssh/akam` (or `--identity FILE`; with `--pubkey`/the state key the private key
is the file without `.pub`), the key the bash wizard creates and puts on the flash card. A key with a
passphrase is signed through `ssh-agent` or asks the passphrase without echo. Only `ssh-ed25519` host keys
are accepted (what `akam_ssh.sh` generates). The camera's key is pinned in `<state dir>/known_hosts` under the
camera's address, so one pin also covers a second Dropbear on port 2222. `~/.ssh/known_hosts` is never written,
and `~/.ssh/config` is ignored.

* First contact: the SHA256 fingerprint is shown and one yes/no is asked (default No). Without a terminal it
  refuses; pass `--trust-new-host-key SHA256:...` to accept that exact key. If the same key is already in your
  `~/.ssh/known_hosts` the question says so (the bash wizard recorded it there).
* A key that **changed** is a hard error, `--trust-new-host-key` included. Only `akam forget-host-key IP`
  (it asks, and logs it) clears a pin.

### Safety rules (tested)

* Every yes/no defaults to No (Enter, Esc, `q`, end of input) and refuses without a terminal; `debug on`,
  `onvif enable` and `rtsp-auth disable` ask exactly as in the bash wizard, the pure reads and the small
  reversible settings (`led`, `audio`, `picture`, `hostname`, `rtsp-auth set`) do not, again as in bash.
* All camera command lines come from `internal/cli/remote.go`, and the one function that sends them refuses
  `reboot`, `poweroff`, `halt`, `init`, `kill`, anything about the watchdog or a flash write. A test checks the
  whole set and the guard itself.
* Passwords are read without echo (or from `AKAM_RTSP_PASSWORD` / `--password-file`, removed from the
  environment before any other process starts), never in an argument, the state file or the log; the log and
  the state refuse registered secrets. Tests grep the state, the log, the output and every command the fake
  camera received.
* A capture or conversion process (`arecord`, `ffmpeg`) runs in its own process group, is stopped on every
  way out (end, error, Ctrl-C, TERM, HUP) and, on Linux, is killed by the kernel if this program dies.

## Tests

`go test ./...` (or `tests/assistant_test.sh`, which `tools/ci/local_check.sh` runs; it is skipped with a
message when Go is missing) covers every P1 command end to end without a camera, a microphone, a speaker or
ffmpeg:

* `internal/testfake`: the fake camera. An in-process SSH server that runs the **real helper scripts**
  of `tools/firmware_patch_templates/` (`akam_rtsp_auth.sh`, `akam_hostname.sh`, `akam_debug.sh`) in a
  temporary root, a control port 8091 reached through `direct-tcpip`, and an RTSP server with Digest login
  against the credentials those scripts wrote and the ONVIF backchannel. `akam_onvif.sh` is emulated until it is
  part of this tree (it is on the ONVIF branch).
* golden files (`testdata/golden/*.golden`) made from the **bash** wizard by `testdata/gen_golden.sh`; the Go
  output must equal them byte for byte (`testdata/scenarios.txt` lists the scenarios);
* a state round trip with the real `./akam` in both directions;
* the bubbletea models driven by scripted keys and compared with `internal/ui/tui/testdata/*.golden`
  (`go test ./internal/ui/tui -update` rewrites them).

## Layout

```
cmd/akam/                 main: picks the UI, calls cli.Run
internal/cli/             dispatcher, options, one file per group of commands, steps and status
internal/ui/              the UI interface and the plain implementation; ui/tui = bubbletea
internal/state/           state files, cameras, log with redaction, lock
internal/camera/ssh/      SSH client, keys, host-key pinning
internal/camera/ctl/      control port 8091 client
internal/discover/        port probe, subnet list and scan, WS-Discovery, mDNS
internal/rtsp/            RTSP + Digest, ONVIF backchannel talk, G.711, WAV
internal/audio/           optional capture tools (external processes)
internal/testfake/        fake camera for the tests
```

## Known gaps against the bash wizard (P1)

* `find-camera` does not open the stock maintenance telnet shell (that is the telnet client of phase P3): a
  camera with port 24 open is recorded as "stock firmware with the toolkit", where bash also tells an Akam camera
  that still has the card in.
* `find-camera` keeps the bash rule that it needs `prepare-backup-sd` done first (use `register` for a camera
  that already runs Akam, from either program).
* `say` decodes WAV itself and uses `ffmpeg` only for other formats (the bash script always used `ffmpeg` when
  present), so its rehearsal line differs; `pw-record`/`parec` capture backends are not used yet.
* Messages say `./akam` (as the bash wizard does) even when the binary is run under another name.
* `onvif` follows the command of the ONVIF branch (`akam_onvif.sh`), which is not merged into `dev` yet.
* The Go toolchain is not pinned in `tools/build/deps.lock` yet.
