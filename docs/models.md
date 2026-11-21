# Models

One-stop map: model id, where its code lives, how it is delivered, status. A path that contains a model id belongs to
that model only; shared code stays in `src` and `tools/`.

| Model id | Camera | SoC | Firmware kind | Code | Status |
|---|---|---|---|---|---|
| `lsc-indoor-1080p-ak3918` | LSC Smart Indoor 1080p, article `3215672.3` | Anyka AK3918AV130 | `native-app`: Akam replaces the APP partition | [`platforms/anyka-ak3918`](../platforms/anyka-ak3918), [`firmware/lsc-indoor-1080p-ak3918`](../firmware/lsc-indoor-1080p-ak3918), registry in [`tools/release/models/`](../tools/release/models) | validated (two units) |
| `lsc-rotatable-2k-ak3918av130` | LSC Smart Rotatable Camera 2K (Dual Band), article `3225722` | Anyka AK3918AV130 | `cardless-appfs`: Akam runs from flash with no SD card (the first install and the recovery use an SD card) | [`firmware/lsc-rotatable-2k-ak3918av130`](../firmware/lsc-rotatable-2k-ak3918av130) | validated (one unit, AK3918AV130, card-less since 2026-11-15); the assistant does not drive it yet ([design](assistant-rot2k-flash-design.md)) |

Kinds:

- `native-app`: the camera's APP flash partition is replaced by an Akam image (installer, kit, release flow).
- `cardless-appfs`: the appfs partition (`mtd3`, squashfs xz 128 KiB, 0x490000) is replaced by the stock appfs plus the
  Akam payload, flashed with `appfs/build_appfs.sh` + `tools/flash_appfs.sh` ([card-less appfs](porting/lsc-rotatable-2k/cardless-appfs.md)).
  Inserting the Akam SD card overrides it (recovery). The assistant refuses its flows until step 2 of the
  [assistant design](assistant-rot2k-flash-design.md).
- `decloud-overlay`: nothing is flashed; an SD card hooks the stock boot (`mode=debug`), cuts the cloud, enables the
  stock local services and adds SSH. Removing the card restores stock behaviour.

See [compatible cameras](compatible-cameras.md) for what is supported today, and the
[rotatable plan](porting/lsc-rotatable-2k/PLAN.md).
