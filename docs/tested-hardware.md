# Cameras tested on real hardware

Akam has been run on real cameras of **one** model. Nothing else is known to work.

| Item | Value |
|---|---|
| Product | LSC Smart Indoor 1080p (sold at Action) |
| Model / article | `3215672.3` |
| Label on the camera | SI `D26228` |
| SoC | Anyka AK3918AV130 |
| Image sensor | GalaxyCore GC20C3 (2 MP, from the [hardware notes](hardware_and_uart.md)) |
| Flash | 8 MiB SPI NOR (XM25QH64D or Winbond equivalent), 8 MTD partitions; only the APP partition (`mtd7`, 5 242 880 bytes) is rewritten ([layout](flash_partition_map_and_rootfs.md)) |
| Stock firmware | Linux `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`, stock APP squashfs xz, block 131072 (fingerprint in `tools/release/supported_hardware.txt`; no retail firmware version number is recorded) |
| Units tested | 2, host names `akam-01` and `akam-02` |
| Akam version flashed | `akam-01`: development image built 2026-11-11 (ONVIF, encoder-stall watchdog, Wi-Fi supervisor, opt-in NTP, video config, quieter logs); `akam-02`: development image built 2026-11-10 ("v3 ctl"). Wizard `1.0.0-dev`; no release has been published yet |
| What was validated | see [testing status](testing-status.md): the hardware-validated list covers video, audio, talk, LEDs, day/night, port 8091, SSH, flash and boot; ONVIF was validated on `akam-01` with the check tool; ONVIF Device Manager on Windows and several robustness features are not yet tested on a camera |
| Date | 2026-11 |

Other models: the `3215672.2` (labels SI `B26101` / `C26101`) is untested with Akam. **Any other LSC or Tuya
camera with an Anyka chip is untested, may use a different flash layout or kernel, and may brick.** The wizard
warns on anything it does not recognise and refuses a different flash layout, but that is a safety net, not a
guarantee. **Always take a full backup first** and keep it on two media
([backup](akam_v1_backup.md)).

## Report your camera

Whether it works or not, a report helps. Please give:

- the model and article number and the code printed on the label (for example `3215672.3`, `SI D26228`);
- the SoC, from the camera: `cat /proc/cpuinfo` (over SSH on an Akam camera, or on the maintenance shell);
- the partition table: `cat /proc/mtd`, and the kernel line: `uname -a`;
- the wizard's hardware verdict, shown by `./akam backup` and `./akam image` (it compares the camera with
  `tools/release/supported_hardware.txt`), and the output of `./akam status`;
- on a camera that already runs Akam, the output of the read-only check script, which changes nothing:
  `ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh` (`./akam verify` runs it for you);
- what you did, what you saw, and the end of the log in `~/.local/state/akam/` (redacted).

Never attach your backup, images or keys, and remove Wi-Fi names, passwords and MAC addresses from anything you
post.
