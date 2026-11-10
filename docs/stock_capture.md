# Capturing what the stock firmware does to the speaker, the ISP and the day/night switch

> **Advanced / developer procedure.** Nothing here is needed to install or use Akam. It records how
> the vendor app `anyka_ipc` drives the speaker, the ISP (image quality, grain) and the IR-cut /
> IR LED day/night switch, so that `ak_rtsp` can do the same (see `specs/05-spec-audio-out.md`). It
> runs on a camera that is **still stock**, booted from the toolkit microSD, and writes **nothing to
> flash**: no `/etc/config`, no MTD partition. A power cycle returns the camera to exactly what it
> was.
>
> Status: the audio part ran on a real camera and worked. In two runs the capture ended 5 to 8 s
> after `anyka_ipc` was restarted with the hook, and it looked like a reboot. **It was not one: the
> camera kept running (its uptime did not restart).** The stock `anyka_ipc` kills `telnetd` when it
> starts, so the telnet session you run the script from drops, the script gets `SIGHUP` and restores
> everything (`status=signal`). The fix is to run the capture **detached**: `run --detach` (see "What
> happened in the first two runs" and "Procedure"). The reworked tools (lighter hook, full ISP blocks,
> day/night prompts, a per-second heartbeat, logs that survive a reboot, `--detach`, `status`, `watch`,
> `note`) are tested on the host only (fake camera root, ARM hook built and checked, the older hook
> loaded into the real `anyka_ipc` under `qemu-arm`, the `system()` / `popen()` forwarding run against the
> vendor uClibc with a small test program); the detached mode has **not yet been run on a camera**. The
> `dd conv=fsync` of the camera's BusyBox 1.30.1 is also unverified (the script falls back and says so).

## What you get

A folder `stock_capture/runNN/` on the card with:

| File | Content |
|---|---|
| `stock_hook.log` | One line per event: open/close of `/dev/pcm*`, `/sys/class/gpio/**`, the light sensor (`/sys/bus/iio/**`), PWM and LED sysfs files, `/dev/mem`, audio files; every ioctl on those fds with its number, decoded dir/type/nr/size, argument bytes before and after, return value and errno; write/read sizes; first bytes and 2 s statistics of the audio data; `mmap` of those fds; one short `ISP_BLOCK` line per distinct ISP parameter block and `ISP_STAT` counters every 10 s; `CHILD` lines for the commands `anyka_ipc` runs with `system()` and `popen()` (`via=system` / `via=popen`, with the command text). All with a timestamp (seconds since the hook started) and the thread id. |
| `isp_blocks.bin` | Every ISP parameter block IN FULL (real struct size, up to 46584 bytes for the sharpness block), once per distinct content: day/night changes of noise reduction, sharpness, CCM, gamma, AE, white balance. Binary, vendor data: keep it private. Read it with `parse_stock_capture.py` (diffs, `--export-isp`). |
| `isp_hook.log` | The older `isp_hook.c` decoders (AE windows, white balance, sensor registers, VENC `CREATE_ENC`), now with a time stamp on each line. |
| `timeline.log` | Written by the script: one line per second (camera uptime, anyka_ipc alive or GONE, load, free RAM, `/tmp/camera_start_times`), the prompts and the notes you made (`note`), each with its time. It is what tells the last moment alive after a reboot. |
| `console.txt` | What the script printed, for a detached run (`--detach`): the steps, the progress line every 5 s, the prompts (COVER / UNCOVER) as banners, the restore. Read it with `watch` / `status`, or on the PC. |
| `dmesg_live.txt`, `anyka_debug_live.txt`, `ps_live.txt` | Rolling snapshots every 10 s (last kernel lines, the stock debug log, the process list): the last ones before a reboot are on the card. |
| `stock_audio.raw` | The first 64 KiB of audio written to the playback device. Vendor content: keep it private, never publish it. |
| `run.meta` | Facts about the run, and `status=running` until the run ends (`finished`, `signal` = the script was told to stop, for instance by a lost telnet session or by `restore`; a run left `running` is one the script did not survive, for instance a camera that really rebooted). |
| `dmesg.txt`, `messages_tail.txt`, `anyka_debug_tail.txt`, `fd_*.txt`, `maps_end.txt`, `proc_*.txt`, `debug_gpio.txt` | State of the camera during the run (kernel messages of `ak_pcm.ko` such as `SPEAKER @69`, open descriptors, the stock debug log). |
| `config_before.txt`, `config_after.txt`, `config_diff.txt` | Proof that `/etc/config` did not change. |

Not captured: `fopen()`/`fwrite()` (stdio uses the libc-internal `open`; the stock binary imports
`open`, `write` and `ioctl` directly, which are caught), and register writes made through a `/dev/mem`
mapping (the mapping itself is logged).

The commands of `system()` and `popen()` (the stock binary imports both: `echo N > /tmp/camera_start_times`,
`sh /usr/sbin/recover_cfg.sh &`, `reboot -f` are in its strings) are logged by the hook from inside
`anyka_ipc`: it exports `system()` and `popen()`, writes one `CHILD` line with the command **before**
the call, and forwards to libc's function (found with `dlsym(RTLD_NEXT)`), so arguments, result and `errno` are
libc's. A command whose text has `reboot`, `recover`, `reset`, `poweroff`, `halt` or `kill` gets its line
fsync'd first, the only fsync the hook ever does, because nothing is flushed after a reboot. This
is **not** done through `LD_PRELOAD` inheritance: the camera's `/bin/sh` is a static BusyBox, which
ignores `LD_PRELOAD`, so a shell started by `anyka_ipc` never loads the hook. (A dynamically linked
child does load it and writes its own `CHILD` line with its command line; none was ever seen.) What is
not seen: a command run by something other than `anyka_ipc`, and a reboot started from
a place that is not `system()`/`popen()` (the `reboot` system call itself, the stock watchdog).

## How it stays safe

* The **watchdog is never touched**: it feeds the nowayout hardware watchdog, killing it reboots the
  board. It starts `anyka_ipc` with `execvp("anyka_ipc")`, and uClibc ignores `/etc/ld.so.preload`
  (checked in `ld-uClibc`: only `LD_PRELOAD` exists). So the script bind-mounts a 3-line wrapper over
  `/usr/bin/anyka_ipc`; it sets `LD_PRELOAD=/tmp/stock_capture/stock_hook.so` and `exec`s the real
  binary, which is reached through a second, non-recursive bind of `/usr`. Only `anyka_ipc` is
  killed (TERM); the watchdog respawns it, now with the hook.
* The hook only activates inside a process whose `/proc/self/exe` is `anyka_ipc` (a dynamically linked
  child that inherits `LD_PRELOAD` writes one `CHILD` line and stays silent), keeps its own descriptors at 900 and above,
  sets `errno` like libc, never changes a return value, and drops events when the card is missing
  (log placement: `/tmp/stock_capture/out` which links to the card folder, then
  `/tmp/sd/stock_capture` if that folder already exists, then RAM `/tmp` with a 1 MiB cap per file; the
  hook never creates a folder; caps below).
  It takes no lock and allocates nothing, so a busy multi-threaded `anyka_ipc` cannot deadlock on it.
* **Small footprint on the application's threads** (reworked after the first run): every hooked call
  owns at most ONE 512-byte line buffer, in a leaf function (the hooked entry points keep a frame of
  about 100 bytes). Measured on the ARM build (frame sizes from the generated code): about 0.8 KiB of
  the caller's stack in the steady state, about 1 KiB at most (the log file has to be reopened), about
  1.4 KiB once, at the very first call (the start banner); the first run's hook used up to about 1.8 KiB
  (a 1400-byte buffer). Polled
  ISP blocks are hashed and written at most once per second (written ones 10 per second), the payload
  goes to the card with one `writev()` straight from the application's memory (no copy, no big frame),
  an unmapped payload is never touched (`mincore`), identical sysfs writes and light-sensor polls are
  counted, not logged. The only `fsync` inside `anyka_ipc` is the one before a `system()` / `popen()`
  command that looks like a reboot or reset; the script fsyncs everything else from outside.
* **Size caps** on the card: `stock_hook.log` and `isp_hook.log` 12 MiB each, `isp_blocks.bin` 24 MiB
  (2 MiB per command), audio 64 KiB, so a run needs about 50 MiB (the script wants 80 MB free). The RAM
  fallback (no card) is 1 MiB per file; `/tmp` (about 22 MB of RAM) only holds the 80 KB hook and the
  state files.
* **Faithful settings.** The toolkit overrides `/etc/config/_ht_hw_settings.ini` and
  `_ht_sw_settings.ini` with files of another model (different speaker GPIO, no `[audio]` section)
  and mutes the factory prompts. For the capture the script stacks RAM copies of the pristine files of
  the APP partition on top of them and un-mutes the prompts, then pops all of it at the end
  (`--keep-mute` leaves the prompts muted). The camera stays in the toolkit's factory-test mode
  (`/tmp/_ak39_factory.ini`): the script does not leave it, because the normal consumer mode may start
  Wi-Fi provisioning.
* It refuses to run on anything but a stock camera: `anyka_ipc` and the watchdog running, no
  `ak_rtsp`, no Akam helper, and (when `sha256sum` exists) the content of `/dev/mtdblock7` equal to
  the known stock APP image `14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294`
  (`--force` overrides a mismatch).
* A foreground run restores on `SIGINT` (Ctrl-C), `SIGTERM` and `SIGHUP` (lost telnet). A **detached**
  run (`--detach`) lives in its own session (`busybox setsid`), without a terminal, ignores `SIGHUP`
  and restores on `SIGTERM`; that is what `restore` run from another shell sends to it (BusyBox ash
  starts a background job with `SIGINT` ignored and cannot trap it again, so a stray `SIGINT` does
  nothing to a detached run). Ctrl-C in `watch` only stops the watching. `restore` is idempotent, and the
  whole run is safe to repeat (each run gets its own `runNN` folder). If a bind cannot be unmounted,
  `restore` keeps the wrapper and its state and says so: run it again, or power-cycle. After a
  `SIGKILL` of the script itself, run `restore` by hand (`check` and `status` tell that a state is
  stale: the script's pid is gone, or the pid now belongs to another program, which is never
  signalled).
* **If the watchdog does not bring `anyka_ipc` back** within 60 s, the script gives up and restores.
  The camera may then reboot by itself (the stock watchdog is in charge of that); a reboot only
  returns it to the stock state with the toolkit card, which is what we want anyway.
* **If the camera does reboot during a run** (it did not in the two runs seen), nothing has to be undone: the binds, the wrapper and the
  state live in RAM and are gone with the reboot. The logs written so far stay on the card and are
  fsync'd file by file (`dd conv=fsync`, never a global `sync`, which would also touch the flash file
  systems; the end of a run flushes the run folder the same way) every 2 s, so at most the last ~2 s are missing. See "If telnet drops, or the camera really reboots".
  That the camera's BusyBox 1.30.1 `dd` honours `conv=fsync` is assumed, not verified on the camera; if
  `dd` fails twice the script says so and stops syncing.
* **The start counter.** The stock app counts its own starts in `/tmp/camera_start_times` (RAM). The
  script records its value (`run.meta`, every `timeline.log` line) and resets it to 0 (what the stock
  boot script writes at every boot) just before it restarts `anyka_ipc`; `--keep-start-counter` skips
  the reset. It was a suspect for the "reboot" of the first run and is **not needed for it** (there was
  no reboot, see below); it is kept because it is harmless: RAM only, the value a fresh boot has, and
  it keeps our extra restart of `anyka_ipc` from counting towards whatever limit the stock app has.
  The file is only written when it is a regular file and `/tmp` is a `tmpfs` mount (`/etc/config` is
  jffs2, flash): otherwise the script leaves it alone and says so (`start_times_reset=refused`).
* The stock `anyka_ipc` still does what it always does while it runs (including its own log files
  under `/etc/config`, if it writes any); the script writes nothing there, and `config_diff.txt` shows
  whether anything changed.

## What happened in the first two runs (the camera did not reboot)

What was seen: the audio capture was fine, then the capture stopped about 15 s after the hooked
`anyka_ipc` started (`stock_hook.log` ends at T=14.48 s in the first run) and the telnet session was
gone; it was taken for a reboot. Two runs on the hardware showed otherwise:

* **The camera did not reboot.** Its uptime kept running.
* **The stock `anyka_ipc` kills `telnetd` when it starts.** The toolkit's `telnet.sh` restart loop
  launches `telnetd` again a few seconds later (the same thing can be seen at a normal boot). So the
  telnet session drops 5 to 8 s after the hooked `anyka_ipc` starts.
* The script ran in that session, got `SIGHUP`, and its `HUP` trap ran the restore: the binds were
  popped and `anyka_ipc` was killed again (respawned without the hook). The logs end where the restore
  killed the hooked `anyka_ipc`; `run.meta` says `status=signal`. Nothing was wrong with the hook, the
  card or the camera.

The theories written down after the first run (a stack overflow in the hook, the logging starving
`anyka_ipc`, the start counter or the `TERM` handler of the stock app leading to `recover_cfg.sh; reboot -f`,
the stock watchdog) were leads from strings and imports of the binaries. They are **not needed to
explain this symptom, and none of them happened**: no reboot took place. They stay open questions
for a run that really does end in a reboot, which none did; the hook is lighter anyway and the tools
below would show it (`CHILD` lines, heartbeat). The start counter reset is kept for the reason given
above.

What this changes: run the capture detached (`run --detach`). The script then re-executes itself in
its own session with `busybox setsid` (the camera's BusyBox 1.30.1 has the applet; without it the
script says so and does not start), stdin from `/dev/null`, `SIGHUP` ignored, and its output in
`runNN/console.txt`. The telnet session may still drop; the capture does not care. You reconnect
and look at it with `watch` / `status`, and record what you see with `note`.

## Procedure

1. Prepare the toolkit card as usual: `./akam prepare-backup-sd`. There must be no `*.bin` file at the
   top of the card (the stock firmware would try to flash it); the packer in step 2 refuses to run if
   there is one, in any letter case.
2. With the card mounted on the PC, build and copy the kit:

   ```bash
   tools/build/bootstrap.sh                       # once
   tools/hw_tests/stock_capture_pack.sh /path/to/card
   ```

   This puts `stock_hook.so`, `stock_capture.sh` and a `SHA256SUMS` in `stock_capture/` on the card.
3. Boot the camera with the card, wait about two minutes, find its address and `telnet CAMERA_IP 24`.
4. In the telnet shell, one command at a time. First the check, which changes nothing on the camera
   (stock check, kit check, free space; on the card it only marks an earlier run that never finished,
   and it says when a capture is running or a stale state is left). Read the lines it prints: it
   must end with `CHECK: OK`:

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh check
   ```

5. Start the capture, **detached**. It checks everything first (you see the lines), then starts the
   run in the background and returns at once:

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh run --detach 120
   ```

   It prints the run folder (`runNN`) and the commands below. **Start a stopwatch when you press
   Enter.** About 10 to 40 s later the stock `anyka_ipc` is restarted with the hook, kills `telnetd`, and
   **this telnet session drops: that is expected and harmless** (the camera does not reboot, the
   capture goes on). The camera may speak loudly (the factory prompts are un-muted on purpose). A run
   is 10 to 3600 s.
6. Wait a few seconds and telnet in again (`telnet CAMERA_IP 24`; if it is refused, wait and retry:
   `telnetd` is being relaunched). Then, one command at a time:

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh watch
   ```

   `watch` shows the state, then one short line every 2 s (time `t`, whether `anyka_ipc` is alive, the
   next prompt), shows each prompt as a banner, and ends when the run ends. **Ctrl-C only stops the
   watching**: the run is in another session and does not notice. Run `watch` again at any time.
   `status` is the one-shot version (elapsed `t`, the prompts due or done with the seconds left,
   `anyka_ipc` alive, the last lines of `console.txt`):

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh status
   ```

   `status` also shows `clock: t=0 came N s after you pressed Enter`: the capture clock starts after
   the restart of `anyka_ipc`, so a prompt at `t=60` is at stopwatch `60 + N`. The prompts are also in
   `runNN/console.txt` and `timeline.log` whatever you do. Anything you see or do that the logs cannot
   know (the room went dark, you covered the lens) can be recorded from any telnet session, with its
   time:

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh note "lens covered, room dark"
   ```

7. To stop early, or if something looks wrong (the run restores itself and `anyka_ipc` is respawned
   without the hook):

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh restore
   ```

8. When `status` says `finished rc=0` (the script has flushed the files itself), power the camera off,
   take the card, copy `stock_capture/runNN/` to the PC and summarise it:

   ```bash
   python3 -I tools/hw_tests/parse_stock_capture.py /path/to/run01
   ```

`run 120` without `--detach` still exists (progress lines on screen, a typed line is a note), but it
dies with the telnet session: when `telnetd` is killed, it receives `SIGHUP` and restores. It is only
useful from a shell that does not drop (for instance a serial console).

### An image / day-night run (grain, IR cut, IR LEDs)

Do this after a plain `run --detach 120` finished (`status=finished`). Same steps 1 to 4 (the check
must end with `CHECK: OK`), then, one command at a time:

```sh
sh /tmp/sd/stock_capture/stock_capture.sh run --daynight --detach
```

Start the stopwatch when you press Enter, wait for the telnet session to drop, reconnect and run
`watch` (or `status`). `--daynight` is a 300 s run with two prompts written in large letters, shown
by `watch` and written in `console.txt`: **at 60 s "COVER THE LENS NOW"** (cover it completely and
keep the room dark: the camera should switch to night and turn the IR LEDs on) and **at 180 s
"UNCOVER THE LENS NOW"** (the camera should switch back to day). If you cannot watch, use the
stopwatch (`t=60` is at stopwatch `60 + N`, N from `status`). Other timings:

```sh
sh /tmp/sd/stock_capture/stock_capture.sh run 400 --phase 90:cover_the_lens --phase 250:uncover_the_lens --detach
```

(`_` shows as a space; `--phase` can be repeated; a `--phase` at or after the end is refused.) Record what
you see with `note`, for instance `note "room dark"` or `note "IR LEDs visible"`: it is saved with its
time. The switch itself may take tens of seconds (the stock app waits for several stable frames
before it decides); if the camera has not switched after 60 s of darkness, cover it better and do not
stop the run, the log of the non-switch is useful too.

Then on the PC:

```bash
python3 -I tools/hw_tests/parse_stock_capture.py /path/to/run02 | less
python3 -I tools/hw_tests/parse_stock_capture.py /path/to/run02 --export-isp /path/to/blocks   # every block as a file
```

The summary has the **day/night time line** (your prompts and notes, every GPIO / PWM / LED write,
the light-sensor value when it changes, the AE state, every ISP parameter write with the words that
changed) and **ISP blocks that differ between the phases**: that is the list of noise-reduction,
sharpness, gamma, CCM, white-balance and exposure parameters the stock app changes for night. Keep
`isp_blocks.bin` and the exported blocks private (vendor data); share the summary text only after
checking it.

### If telnet drops, or the camera really reboots

1. **A dropped telnet session is normal** (the stock `anyka_ipc` kills `telnetd` when it starts). Wait a
   few seconds, telnet in again, run `status`. With `run --detach` the capture goes on. If you ran
   without `--detach`, the run was stopped and restored by the `SIGHUP` (`status` says `signal`); that
   run is short, keep or delete it, and start again with `--detach`.
2. To tell a dropped session from a reboot, right after reconnecting:

   ```sh
   cat /proc/uptime
   ```

   An uptime that kept growing since you booted the camera (a few minutes, not a few seconds) means no
   reboot. A small uptime means the camera did reboot.
3. After a real reboot nothing has to be restored: the bind mounts, the wrapper and the state were in
   RAM. `stock_capture.sh restore` says "nothing to restore" and is harmless. Write down **how long
   after the start** it happened.
4. Take the card and run the summary on that `runNN`. It says "RUN INTERRUPTED", the last heartbeat
   (camera uptime, whether `anyka_ipc` was alive, load, free RAM, the start counter), whether a
   `reboot` / `recover_cfg` process was started by `anyka_ipc` (`CHILD` line), and the last 60 kernel
   lines and the stock debug log are in `dmesg_live.txt` / `anyka_debug_live.txt`.
5. The next `check` or `run` also reports the run that never finished and marks it
   (`status=interrupted`). Do not repeat the run after a real reboot until the cause is understood.

If something goes wrong (or a run was killed hard and `check` / `status` say the state is stale):

```sh
sh /tmp/sd/stock_capture/stock_capture.sh restore
```

or just power-cycle the camera: everything the script mounts lives in RAM.

## Reading the summary

`parse_stock_capture.py` starts with the **state of the run** (finished, or interrupted with the last
heartbeat) and the processes `anyka_ipc` started, then prints the ordered ioctl sequence on the PCM devices with decoded arguments
(`PREPARE` = rate, channels, bits, period bytes, periods; `SET_SOURCES`; `SET_GAIN`; `SET_SPEAKER`;
`RESUME`). **Every** ioctl on a PCM descriptor is listed with its raw request number (`req=`) and
argument bytes, known or not: `0x80045081` for `SET_SPEAKER` is known to be wrong on the hardware, so
read the `req=` the stock app really sends. A call repeated with the same argument is logged 24 times,
then again whenever its argument changes and every 200th time. The summary also gives the values of
gain, source and speaker, the PCM format and period length, byte rate and content statistics of the audio written (and what format they suggest), GPIO activity, audio files
read, `mmap` windows, ISP parameter blocks, everything that failed, and the kernel lines about the
speaker. Names of the `'P'` ioctls come from the spec (two names are shown where the spec is unsure);
the numbers and bytes are the facts.

## Tests

`tests/stock_hook_test.sh` (hook, built for the host with an x86_64 syscall branch: full and rate
limited ISP blocks, partly mapped and unmapped payloads, sysfs throttling, `CHILD` lines, `system()` / `popen()` forwarding, caps, threads;
plus the ARM build checked warning-free and free of undefined symbols), `tests/stock_capture_script_test.sh`
(script on a fake root under BusyBox: prompts, heartbeat, fsync that does not truncate, start counter,
a run killed in the middle, a typed note through a pseudo-terminal, `--detach` with a stub `busybox setsid`
(own session, stdin `/dev/null`, `SIGHUP` ignored, the run survives and finishes, refusal when `setsid` is missing),
`status`, `watch` (Ctrl-C leaves the run alone), `note`, `restore` stopping a detached run, a stale state
after `SIGKILL`),
`tests/stock_capture_parse_test.sh` (including a synthetic interrupted day/night capture),
`tests/stock_capture_pack_test.sh`.
