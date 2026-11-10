# Capturing what the stock firmware does to the speaker (and the ISP)

> **Advanced / developer procedure.** Nothing here is needed to install or use Akam. It records how
> the vendor app `anyka_ipc` drives the speaker, so that `ak_rtsp` can do the same (see
> `specs/05-spec-audio-out.md`). It runs on a camera that is **still stock**, booted from the
> toolkit microSD, and writes **nothing to flash**: no `/etc/config`, no MTD partition. A power cycle
> returns the camera to exactly what it was.
>
> Status: written and tested on the host (fake camera root, ARM hook loaded into the real
> `anyka_ipc` under `qemu-arm` with the uClibc loader). **Not yet run on a real camera.**

## What you get

A folder `stock_capture/runNN/` on the card with:

| File | Content |
|---|---|
| `stock_hook.log` | One line per event: open/close of `/dev/pcm*`, `/sys/class/gpio/**`, `/dev/mem`, audio files; every ioctl on those fds with its number, decoded dir/type/nr/size, argument bytes before and after, return value and errno; write/read sizes; first bytes and 2 s statistics of the audio data; `mmap` of those fds; ISP parameter blocks. All with a timestamp (seconds since the hook started) and the thread id. |
| `isp_hook.log` | The older `isp_hook.c` decoders (AE windows, white balance, sensor registers, VENC `CREATE_ENC`). |
| `stock_audio.raw` | The first 64 KiB of audio written to the playback device. Vendor content: keep it private, never publish it. |
| `run.meta`, `dmesg.txt`, `messages_tail.txt`, `anyka_debug_tail.txt`, `fd_*.txt`, `maps_end.txt`, `proc_*.txt`, `debug_gpio.txt` | State of the camera during the run (kernel messages of `ak_pcm.ko` such as `SPEAKER @69`, open descriptors, the stock debug log). |
| `config_before.txt`, `config_after.txt`, `config_diff.txt` | Proof that `/etc/config` did not change. |

Not captured: `fopen()`/`fwrite()` and `system()`/`popen()` (stdio uses the libc-internal `open`; the
stock binary imports `open`, `write` and `ioctl` directly, which are caught), and register writes made
through a `/dev/mem` mapping (the mapping itself is logged).

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
  hook never creates a folder; SD caps are 12 MiB per log, so a run needs about 25 MiB on the card).
  It takes no lock and allocates nothing, so a busy multi-threaded `anyka_ipc` cannot deadlock on it.
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
* The stock `anyka_ipc` still does what it always does while it runs (including its own log files
  under `/etc/config`, if it writes any); the script writes nothing there, and `config_diff.txt` shows
  whether anything changed.

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
   prints each step, the capture progress and, at the end, the folder with the logs.
5. `sync`, power the camera off, take the card, copy `stock_capture/runNN/` to the PC and summarise it:

   ```bash
   python3 -I tools/hw_tests/parse_stock_capture.py /path/to/run01
   ```

If something goes wrong (or the telnet session was lost and killed hard):

```sh
sh /tmp/sd/stock_capture/stock_capture.sh restore
```

or just power-cycle the camera: everything the script mounts lives in RAM.

## Reading the summary

`parse_stock_capture.py` prints the ordered ioctl sequence on the PCM devices with decoded arguments
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

`tests/stock_hook_test.sh` (hook, built for the host with an x86_64 syscall branch, plus the ARM build
checked warning-free and free of undefined symbols), `tests/stock_capture_script_test.sh` (script on a
fake root under BusyBox), `tests/stock_capture_parse_test.sh`, `tests/stock_capture_pack_test.sh`.
