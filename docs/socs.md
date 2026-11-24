# SoCs of the supported cameras: what the evidence says

Short, evidence-based answer to "which Anyka chip is in each camera?". Platform directory names are **not** renamed by this note.

## Result

Both cameras carry the **Anyka AK3918AV130** (the `AV130` die of the AK3918 family: ARM926EJ-S, soft-float). The two cameras differ in
board, sensor, vendor userspace and kernel interface, not in the SoC part number.

| | LSC Smart Indoor 1080p (`lsc-indoor-1080p-ak3918`) | LSC Rotatable 2K (`lsc-rotatable-2k-ak3918av130`) |
|---|---|---|
| U-Boot strings | SDK path `.../AK3918AV130/SDK/AnyCloud39AV130_SDK_V1.01/os/uboot/.../ak3918av130/ak_module_reset.c`; device tree nodes `anyka,ak3918av130-{pinctrl,gpio,mmc0..2}` | path `.../yyos_v1.6/accessory/av130_v1.02/opensource/uboot/.../ak3918av130/ak_module_reset.c`; same `anyka,ak3918av130-*` nodes, plus `anyka,ak3918av130-ethernet` |
| Kernel device tree (mtd3 / mtd1 of the own backups) | board string `EVB_CBDM_AK3918av130_V1.0.0`, compatible `anyka,ak3918av130`, nodes `anyka,ak3918av130-{wdt,vi,venc,venc-bridge,usb,uio-cnn,uart1,uart2,...}` | board string `YY_AK3918AV130L`, same node family |
| Stock userspace | raw ioctls on kernel drivers, musl-compatible static binary works | vendor libraries (`libplat_*`, `libmpi_venc`), dynamic uClibc 1.0.31 |

Source of the table: `strings` over the partitions of the two own backups (1080p: `mtdblock0` U-Boot and `mtdblock3` kernel; 2K: `mtd0` U-Boot and `mtd1`
kernel). The same strings show that the AV130 device tree reuses blocks of older dies (`anyka,ak3918av100-spi0`, `anyka,ak3918av100plus-uart0/2`): the
family names `AV100`, `AV100plus` and `AV130` are die revisions of one product line, and the kernel module tree of the camera also lists compatibles
of other Anyka SoCs (`ak37d`, `ak37e`, `ak39ev330`, `ak3918av100`) because one driver set serves several chips. None of those is the chip on these boards.

## What the names mean in this repository

- `anyka-ak3918` (platform of the 1080p) and `anyka-ak3918av130` (platform of the 2K) are **not** two different chips. They are two build
  stacks of the same chip family, split by the kernel and library interface: raw ioctls plus static musl (1080p) versus vendor libraries plus
  dynamic uClibc and the 2018.11 gcc (2K). "AK3918" is the family prefix, "AV130" the die.
- A third camera is placed by interface first (which platform stack can drive it), then by SoC die.
- The documents say "AK3918AV130" when they mean the chip and "AK3918" only inside platform and model ids (and where they mean the family).

## Not settled

- The chip top marking was not read from a package in this repository (the physical markings are not documented anywhere under `docs/`).
  The identification rests on the firmware strings above, which name the SDK and the device tree of the SoC the firmware is built for.
- The 1080p board string `EVB_CBDM_AK3918av130_V1.0.0` is the SDK's evaluation-board name left in the device tree, not the retail board
  (`SI D26228`, article `3215672.3`).
- Whether the platform stacks should be renamed after the die (`anyka-ak3918av130` for both) or after the interface was a naming decision for
  step 7 of the repository reorganisation. The decision: the platform directories keep their two names (`anyka-ak3918` for direct drivers, `anyka-ak3918av130`
  for the vendor-library stack), named after the interface; the step renamed the camera program to `akamd` on both cameras. Model ids are one-way once released.
