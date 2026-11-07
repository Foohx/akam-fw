# Camyn V1: kernel time64 syscalls (the clock bug)

**TL;DR.** The camera's vendor kernel (Linux 4.4.302-cip94, ARM926, no vDSO) does not implement the
time64 system calls, but it answers `clock_gettime64` (syscall 403) with "success" **without writing the
result**. musl 1.2 trusts that answer and never falls back to the 32-bit call, so every
`clock_gettime(CLOCK_MONOTONIC)` in a static musl binary returned whatever was already in the buffer.
Camyn fixes this at the root with a small shim linked into every static binary
(`src/common/time32_shim.c`).

## 1. The proof (measured on the camera)

`time_probe`, run on the real camera, three rounds one second apart:

| Observation | Result |
|---|---|
| `sizeof(time_t)`, `sizeof(struct timespec)`, `sizeof(long)` | 8, 16, 4 (time64 ABI) |
| `AT_SYSINFO_EHDR` | 0: there is no vDSO |
| libc `clock_gettime(CLOCK_MONOTONIC)` and `CLOCK_BOOTTIME` | `ret=0 errno=0` but the buffer is **not written** (still the `0x5a` fill pattern) in every round |
| libc `clock_gettime(CLOCK_REALTIME)` | correct (about 946685793.589), +1 s per round |
| raw syscall 263 (`clock_gettime32`), MONOTONIC and REALTIME | correct, +1 s per round |
| raw syscall 403 (`clock_gettime64`), MONOTONIC | `ret=0`, buffer **not written** |
| `gettimeofday`, `times()` (100 Hz), `/proc/uptime` | correct |

So the failure mode is not `ENOSYS` (which musl handles by falling back to syscall 263). It is
"returns 0 and does nothing", and that makes every time64 syscall suspect on this kernel.

## 2. Why it matters

* musl `clock_gettime()` and the internal `__clock_gettime()` call syscall 403 first when
  `time_t` is 64-bit and fall back to 263 only on `ENOSYS`. `__clock_gettime()` also serves `time()`,
  `clock()`, `timespec_get()`, `pthread_cond_timedwait`, `sem_timedwait` and `pthread_mutex_timedlock`.
* Dropbear drops sessions with "Timeout before auth" immediately (its monotonic clock never advances or
  is garbage).
* The `ak_rtsp` day/night anti-flap lock (60 s) expired within seconds.
* Other time64 syscalls are issued by musl **only when a value does not fit in 32 bits** (`nanosleep`,
  `clock_nanosleep`, `ppoll`/`poll`, `pselect6`/`select`, futex timed waits, `utimensat`,
  `clock_settime`, `mq_*`, `sigtimedwait`). Our programs never pass such values, so they use the working
  32-bit syscalls. The unconditional ones besides `clock_gettime64` are the get-paths and the "old value"
  paths of `timerfd_*`/`timer_*`, `clock_adjtime64` and `recvmmsg_time64`.

## 3. The fix

`src/common/time32_shim.c` defines, with the 32-bit syscalls only (263, 350, 353, 354):

| Function | Notes |
|---|---|
| `clock_gettime`, `__clock_gettime` (musl's internal name) | Covers `time()`, `clock()`, `timespec_get()`, the timed waits of pthread/semaphores |
| `timerfd_settime`, `timerfd_gettime` | musl always uses the time64 syscalls for the get path and for the old value |
| `timerfd_create` | No time argument; defined only because it is in the same musl archive member as the two above |

**Why linking beats libc.a.** The shim is a plain object file on the link line. In a static link, libc.a
members are extracted only to satisfy an undefined symbol; musl's `clock_gettime.o` defines nothing but
the symbols the shim already defines, so it is never pulled in and there is no duplicate definition.
(`timerfd.o` also defines `timerfd_create`, which is why the shim defines it too: otherwise the member
is extracted and the link fails with duplicates.) `-Wl,--wrap` was not needed. Note that musl's headers
redirect the public names on 32-bit time64 builds, so the symbols that end up defined are
`__clock_gettime64`, `__timerfd_settime64`, `__timerfd_gettime64` and `__clock_gettime`; including
`<time.h>` in the shim makes the compiler emit exactly those.

**Integration.**

* `src/ak_rtsp/Makefile`: `../common/time32_shim.o` is linked into `ak_rtsp`, `isp_dump`, `flash_tool`
  and `patch_div0` (a binary that does not read a clock simply drops it at link time).
* `tools/dropbear/build_dropbear.sh`: the shim is compiled with the Dropbear flags and passed to the
  multi-call link; the final binary is checked with `tools/build/check_time32.sh` (hard failure, `gdb` is required).
* `src/ak_rtsp/build_firmware.sh`: the same check runs again on the stripped `ak_rtsp` and `dropbearmulti`
  that are actually installed in the image (forbidden time64 syscall, or no `clock_gettime32`, or no `gdb`:
  the build stops). `CAMYN_ALLOW_UNCHECKED_TIME64=1` skips it with a loud warning (stub binaries in tests only).
* Limits: seconds are 32-bit on the wire, so `CLOCK_REALTIME` wraps in 2038 (this kernel cannot do
  better). `utimensat`, `ppoll` and friends are not wrapped (see above) and are verified by the probe instead.
  `clock_getres` needs nothing: the musl bundled with Zig 0.14.1 calls the 32-bit `clock_getres` syscall
  directly on this target and never the time64 one (`lib/libc/musl/src/time/clock_getres.c`).
* The gate also counts syscall numbers loaded into `r7` by conditional `mov`/`orr`. It cannot see a number
  that is not an immediate in `r7` just before the `svc` (such sites are reported as `?`); it is a
  regression net for musl's code generation, not a proof: the camera probe is the proof.
* Link robustness: the shim is a plain object on the link line, so musl's `clock_gettime.o` is never
  extracted. If a musl/Zig update made another member need a symbol only that object defines, the link
  would fail with a duplicate definition (loud), and if it ever silently won, the gate above would see
  `clock_gettime64` (403) in the image and stop the build.

**Defence in depth.** The earlier workarounds stay: the Dropbear patch that takes its monotonic
seconds from `times(2)` ticks, and the tick based clock plus the iteration-counted lock in `night.c`.
They cost nothing, they do not depend on `time_t`, and the anti-flap lock should not rely on a single
clock source. They are no longer the only protection.

## 4. Verification

On the build host (`tests/time32_shim_test.sh`, needs the bootstrap toolchain, `gdb`, `qemu-arm`):

* `tools/build/check_time32.sh` disassembles `.text` and finds every `svc` with its `r7` constant. The
  final `ak_rtsp`, `flash_tool`, `isp_dump`, `patch_div0` and `dropbearmulti` contain no syscall 403, 405,
  408, 409, 410, 411 or 417, and `ak_rtsp` and `dropbearmulti` contain 263. A control build without the
  shim *is* flagged (it loads 403).
* `qemu-arm -strace` of a program using `clock_gettime`, `time`, `clock`, timerfd and the timed waits:
  with the shim, only `clock_gettime` and `timerfd_*` (32-bit) appear; without it, `clock_gettime64`.
* The probe's verdict logic is exercised with `-DPROBE_SIMULATE_NOOP64` (raw time64 syscalls pretend to
  succeed without effect: every one reports NOOP, the 32-bit controls stay WORKS).

qemu-user cannot reproduce the 4.4 kernel (it implements time64), so none of this says the camera now
works: the hardware check below does.

## 5. Hardware check: `time64_probe`

Build (host): `tests/native/build_probe.sh <output>` (static, shim linked).
`PROBE_SHIM=0` builds the comparison binary with stock musl behaviour.

Run on the camera (about 10 s, changes nothing except one temporary empty file and one timerfd):

```
./time64_probe [writable-dir] | tee /mnt/time64_probe.txt
```

It prints, per syscall, a verdict:

| Verdict | Meaning |
|---|---|
| `WORKS` | the kernel really executed the call (cross-checked with the 32-bit twin or by elapsed time) |
| `NOOP` | returned success but did nothing: the camera's failure mode |
| `ENOSYS` | the kernel says it has no such call (musl would fall back) |
| `WRONG` / `ERR` / `HANG` | anything else; `HANG` means no return within 4 s (alarm guard) |

Section 2 tests the raw time64 syscalls (`clock_gettime64` for MONOTONIC/REALTIME/BOOTTIME,
`clock_getres_time64`, `clock_nanosleep_time64`, `ppoll_time64`, `pselect6_time64`, `futex_time64`,
`timerfd_settime64`/`gettime64`, `utimensat_time64`), each next to its 32-bit control. Section 3 tests
what our programs call (libc clocks, `time`, `clock`, sleeps, `poll`/`select` family, condition
variable, semaphore, mutex timed waits, timerfd, `utimensat`) through the shim. The exit status is the
number of failing libc checks; the last line is `RESULT PASS` or `RESULT FAIL`. NOOP lines in section 2
are expected on the camera: they are the reason for the shim.

Acceptance: with the shim build, `RESULT PASS` on the camera; with `PROBE_SHIM=0`, the libc
`clock_gettime(MONOTONIC)` line shows the original bug (`NOOP`).
