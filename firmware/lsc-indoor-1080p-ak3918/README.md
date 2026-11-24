# LSC Smart Indoor 1080p -- `lsc-indoor-1080p-ak3918`

Firmware kind: **`native-app`**: Akam replaces the APP flash partition (`mtd7`) with an image built on your computer from your
own stock backup plus the signed kit. The one application, `akamd`, runs in place of the stock `anyka_ipc`.
Entry page (hardware, install, update, recovery, limits): [docs/porting/lsc-1080p](../../docs/porting/lsc-1080p/README.md).
Model index: [docs/models.md](../../docs/models.md).

Camera: Action article `3215672.3`, label `SI D26228`; SoC Anyka AK3918AV130, 8 MiB SPI NOR, 8 partitions.
Everything model-specific lives in this directory; the platform code is in
[`platforms/anyka-ak3918`](../../platforms/anyka-ak3918/README.md) and the shared code in `src/`.

## Layout

| Path | What |
|---|---|
| `model.mk` | the features of this model and the ordered object list of `akamd` |
| `*.c`, `*.h` | the model's binding: settings rows (`settings.c`), signed-API binding (`api_bind.c`), ONVIF model data |
| `overlay/` | on-camera scripts and the patch templates applied to your own stock APP ([overlay README](overlay/README.md)) |
| `image/` | `build_image.sh` and `assemble_from_kit.sh` (the image builders), `flash_tool.c` |

Users do not build these by hand: [install guide](../../docs/install.md). Manual route: [build guide](../../docs/manual/build.md).
