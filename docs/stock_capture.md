# Capturing what the stock firmware does to the speaker, the ISP and the day/night switch

> **Advanced / developer procedure.** Nothing here is needed to install or use Akam. It records how
> the vendor app `anyka_ipc` drives the speaker, the ISP (image quality, grain) and the IR-cut /
> IR LED day/night switch, so that `ak_rtsp` can do the same (see `specs/05-spec-audio-out.md`). It
> runs on a camera that is **still stock**, booted from the toolkit microSD, and writes **nothing to
> flash**: no `/etc/config`, no MTD partition. A power cycle returns the camera to exactly what it
> was.
>
> Status: the audio part ran on a real camera and worked. **The first run also rebooted the camera
> about 15 s after `anyka_ipc` was restarted with the hook; the cause is not established** (see
> "The first run rebooted the camera" below). The tools were reworked for that: a lighter hook, full
> ISP blocks, day/night prompts, a per-second heartbeat and logs that survive a reboot. The reworked
> version is tested on the host only (fake camera root, ARM hook built and checked, the older hook
> loaded into the real `anyka_ipc` under `qemu-arm`); it has **not yet been run on a camera**.

## What you get

A folder `stock_capture/runNN/` on the card with:

| File | Content |
|---|---|
| `stock_hook.log` | One line per event: open/close of `/dev/pcm*`, `/sys/class/gpio/**`, the light sensor (`/sys/bus/iio/**`), PWM and LED sysfs files, `/dev/mem`, audio files; every ioctl on those fds with its number, decoded dir/type/nr/size, argument bytes before and after, return value and errno; write/read sizes; first bytes and 2 s statistics of the audio data; `mmap` of those fds; one short `ISP_BLOCK` line per distinct ISP parameter block and `ISP_STAT` counters every 10 s; `CHILD` lines for processes `anyka_ipc` starts (`system()`, `popen()`). All with a timestamp (seconds since the hook started) and the thread id. |
| `isp_blocks.bin` | Every ISP parameter block IN FULL (real struct size, up to 46584 bytes for the sharpness block), once per distinct content: day/night changes of noise reduction, sharpness, CCM, gamma, AE, white balance. Binary, vendor data: keep it private. Read it with `parse_stock_capture.py` (diffs, `--export-isp`). |
| `isp_hook.log` | The older `isp_hook.c` decoders (AE windows, white balance, sensor registers, VENC `CREATE_ENC`), now with a time stamp on each line. |
| `timeline.log` | Written by the script: one line per second (camera uptime, anyka_ipc alive or GONE, load, free RAM, `/tmp/camera_start_times`), the prompts and the notes you typed, each with its time. It is what tells the last moment alive after a reboot. |
| `dmesg_live.txt`, `anyka_debug_live.txt`, `ps_live.txt` | Rolling snapshots every 10 s (last kernel lines, the stock debug log, the process list): the last ones before a reboot are on the card. |
| `stock_audio.raw` | The first 64 KiB of audio written to the playback device. Vendor content: keep it private, never publish it. |
| `run.meta` | Facts about the run, and `status=running` until the run ends (`finished`, `signal`; a run left `running` is one the camera did not survive). |
| `dmesg.txt`, `messages_tail.txt`, `anyka_debug_tail.txt`, `fd_*.txt`, `maps_end.txt`, `proc_*.txt`, `debug_gpio.txt` | State of the camera during the run (kernel messages of `ak_pcm.ko` such as `SPEAKER @69`, open descriptors, the stock debug log). |
| `config_before.txt`, `config_after.txt`, `config_diff.txt` | Proof that `/etc/config` did not change. |

Not captured: `fopen()`/`fwrite()` (stdio uses the libc-internal `open`; the stock binary imports
`open`, `write` and `ioctl` directly, which are caught), the `system()` / `popen()` calls themselves
(the processes they start are logged: a `CHILD` line with the command line, because they inherit the
hook; a child whose command line has `reboot`, `recover`, `reset`, `kill` ... fsyncs its line, the only
fsync the hook ever does, because nothing is flushed after a reboot), and register writes made through a `/dev/mem` mapping (the mapping itself is logged).

## How it stays safe

* The **watchdog is never touched**: it feeds the nowayout hardware watchdog, killing it reboots the
  board. It starts `anyka_ipc` with `execvp("anyka_ipc")`, and uClibc ignores `/etc/ld.so.preload`
  (checked in `ld-uClibc`: only `LD_PRELOAD` exists). So the script bind-mounts a 3-line wrapper over
  `/usr/bin/anyka_ipc`; it sets `LD_PRELOAD=/tmp/stock_capture/stock_hook.so` and `exec`s the real
  binary, which is reached through a second, non-recursive bind of `/usr`. Only `anyka_ipc` is
  killed (TERM); the watchdog respawns it, now with the hook.
* The hook only activates inside a process whose `/proc/self/exe` is `anyka_ipc` (children started
  with `system()` inherit `LD_PRELOAD` and stay silent), keeps its own descriptors at 900 and above,
  sets `errno` like libc, never changes a return value, and drops events when the card is missing
  (log placement: `/tmp/stock_capture/out` which links to the card folder, then
  `/tmp/sd/stock_capture` if that folder already exists, then RAM `/tmp` with a 1 MiB cap per file; the
  hook never creates a folder; caps below).
  It takes no lock and allocates nothing, so a busy multi-threaded `anyka_ipc` cannot deadlock on it.
* **Small footprint on the application's threads** (reworked after the first reboot): the deepest hook
  path uses about 0.7 KiB of the caller's stack (it was about 1.8 KiB: a 1400-byte line buffer; measured
  with `-fstack-usage` on the host build, ARM is of the same order), polled
  ISP blocks are hashed and written at most once per second (written ones 10 per second), the payload
  goes to the card with one `writev()` straight from the application's memory (no copy, no big frame),
  an unmapped payload is never touched (`mincore`), identical sysfs writes and light-sensor polls are
  counted, not logged. No `fsync` is ever done inside `anyka_ipc`; the script does it from outside.
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
* A `SIGINT`, `SIGTERM` or `SIGHUP` (lost telnet) runs the restore. `restore` is idempotent, and the
  whole run is safe to repeat (each run gets its own `runNN` folder). If a bind cannot be unmounted,
  `restore` keeps the wrapper and its state and says so: run it again, or power-cycle. After a
  `SIGKILL` of the script itself, run `restore` by hand.
* **If the watchdog does not bring `anyka_ipc` back** within 60 s, the script gives up and restores.
  The camera may then reboot by itself (the stock watchdog is in charge of that); a reboot only
  returns it to the stock state with the toolkit card, which is what we want anyway.
* **If the camera reboots during the run**, nothing has to be undone: the binds, the wrapper and the
  state live in RAM and are gone with the reboot. The logs written so far stay on the card and are
  fsync'd file by file (`dd conv=fsync`, never a global `sync`, which would also touch the flash file
  systems) every 2 s, so at most the last ~2 s are missing. See "If the camera reboots during a run".
  That the camera's BusyBox 1.30.1 `dd` honours `conv=fsync` is assumed, not verified on the camera; if
  `dd` fails twice the script says so and stops syncing.
* **The start counter.** The stock app counts its own starts in `/tmp/camera_start_times` (RAM). The
  script records its value (`run.meta`, every `timeline.log` line) and resets it to 0 (what the stock
  boot script writes at every boot) just before it restarts `anyka_ipc`; `--keep-start-counter` skips
  the reset. This is a precaution for an unproven hypothesis, not a known fix.
* The stock `anyka_ipc` still does what it always does while it runs (including its own log files
  under `/etc/config`, if it writes any); the script writes nothing there, and `config_diff.txt` shows
  whether anything changed.

## The first run rebooted the camera

What was seen: the audio capture was fine, then the camera rebooted about 15 s after the hooked
`anyka_ipc` started (`stock_hook.log` ends at T=14.48 s). What is **known**, what is **not**:

* The hook was not busy: 463 events and 416 `isp_hook.log` lines in 14.5 s (about 200 KB, under 12 KB/s;
  the per-command throttle would have stopped the ISP lines at their 64th occurrence). "The logging
  starved `anyka_ipc`" is therefore not supported by the numbers, but a stalled SD write cannot be
  excluded either.
* All the polling threads (ISP statistics, exposure, scene statistics) stop being logged at the same
  moment. That is a process-wide or system-wide event, not a throttle of the hook.
* **The end of the log is not the time of the reboot.** `write()` to the FAT card only fills the page
  cache; what reached the card is whatever the kernel had flushed (or a `sync` done during the reboot).
  The reboot may have come later than T=14.5 s. Ask whoever watched the telnet session when it died.
* The hook used up to about 1.8 KiB of stack per call on the vendor's threads (the application sets
  the stack size of its threads itself). A stack overflow would kill `anyka_ipc`; what the stock app
  does after a crash is the next point. Not shown, not excluded.
* From the strings and imports of the stock binaries (no disassembly, so these are leads, not
  findings): the stock `watchdog` has a software watchdog per registered process (a timeout ends in
  `reboot`); `anyka_ipc` has a start counter in `/tmp/camera_start_times`, a signal handler that logs
  "do ipc reset", and `sh /usr/sbin/recover_cfg.sh &` / `reboot -f` calls through `system()`. Our
  restart adds one start; the stock boot script only resets the counter at boot.

What was changed for the next run: the lighter hook (stack, rate limits), the start counter recorded
and reset, `CHILD` lines (a `reboot -f` started by `anyka_ipc` is then visible with its command line),
the per-second heartbeat and the fsync'd logs (the exact last second alive is known). **If it reboots
again**, the summary tells which of these it was; if `anyka_ipc` shows GONE or RESTARTED before the
last heartbeat, it was a crash; if a `reboot` child appears, the stock app asked for it; if neither,
suspect the stock watchdog or the kernel. Do not widen the capture before that is understood.

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
4. In the telnet shell, one command at a time:

   ```sh
   sh /tmp/sd/stock_capture/stock_capture.sh check        # read-only: stock check, kit check, free space
   sh /tmp/sd/stock_capture/stock_capture.sh run 120      # capture 120 s, then restore
   ```

   The camera may speak loudly while it runs (the factory prompts are un-muted on purpose). The script
   prints each step, the capture progress (one line per 5 s: log size, anyka_ipc alive, start counter,
   free RAM) and, at the end, the folder with the logs. A run is 10 to 3600 s.
5. `sync`, power the camera off, take the card, copy `stock_capture/runNN/` to the PC and summarise it:

   ```bash
   python3 -I tools/hw_tests/parse_stock_capture.py /path/to/run01
   ```

### An image / day-night run (grain, IR cut, IR LEDs)

Do this only after a plain `run 120` finished without a reboot. Same steps 1 to 3, then, one command at
a time:

```sh
sh /tmp/sd/stock_capture/stock_capture.sh check
sh /tmp/sd/stock_capture/stock_capture.sh run --daynight
```

`--daynight` is a 300 s run with two prompts written in large letters: **at 60 s "COVER THE LENS NOW"**
(cover it completely and keep the room dark: the camera should switch to night and turn the IR LEDs
on) and **at 180 s "UNCOVER THE LENS NOW"** (the camera should switch back to day). Other timings:

```sh
sh /tmp/sd/stock_capture/stock_capture.sh run 400 --phase 90:cover_the_lens --phase 250:uncover_the_lens
```

(`_` shows as a space; `--phase` can be repeated; a `--phase` at or after the end is refused.) You can
also type a line and press Enter at any moment ("room dark", "IR LEDs visible"): it is saved with its
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

### If the camera reboots during a run

1. Wait for it to come back (the toolkit card boots it as before). Nothing has to be restored: the
   bind mounts, the wrapper and the state were in RAM. `stock_capture.sh restore` says "nothing to
   restore" and is harmless.
2. Write down **how long after the start** the telnet session died and what the screen showed last.
3. Take the card and run the summary on that `runNN`. It says "RUN INTERRUPTED", the last heartbeat
   (camera uptime, whether `anyka_ipc` was alive, load, free RAM, the start counter), whether a
   `reboot` / `recover_cfg` process was started by `anyka_ipc` (`CHILD` line), and the last 60 kernel
   lines and the stock debug log are in `dmesg_live.txt` / `anyka_debug_live.txt`.
4. The next `check` or `run` also reports the run that never finished and marks it
   (`status=interrupted`). Do not repeat the run until the cause is understood.

If something goes wrong (or the telnet session was lost and killed hard):

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
limited ISP blocks, partly mapped and unmapped payloads, sysfs throttling, `CHILD` lines, caps, threads;
plus the ARM build checked warning-free and free of undefined symbols), `tests/stock_capture_script_test.sh`
(script on a fake root under BusyBox: prompts, heartbeat, fsync that does not truncate, start counter,
a run killed in the middle like a reboot, a typed note through a pseudo-terminal),
`tests/stock_capture_parse_test.sh` (including a synthetic interrupted day/night capture),
`tests/stock_capture_pack_test.sh`.
