# Other neural networks on the Rotatable 2K NPU: study (offline, 2026-11-15)

Question: can Akam run neural networks other than the stock person + head detector (pets, vehicles, faces, a better person model)
on the AK3918AV130 NPU, and how? Method: structural reading of the firmware files already extracted from the camera image
(`model_0x0A00000D_V1.00.01.bin`, `libmpi_svp2`, `libak_nne`, `libplat_npu`, `libakv_cnn_dt`, `ak_npu.ko`) plus a public-source
search. Offsets and numbers only; no vendor byte, table or string dump is reproduced here (proprietary, never committed).
Confidence: HIGH = read directly, MEDIUM = inferred from several consistent readings, LOW = guess.

## Verdict

| Path | Verdict |
|---|---|
| (a) Reuse other official Anyka model files from other products | Only realistic path to a new class today. Blocked by availability and licence, not by technology. |
| (b) Generic NNE API + model from a public converter | No public Anyka converter found. Dead end for now. |
| (c) Reverse-engineer the format and write our own models | Feasible only for networks that fit a compiled-in topology; weeks of work, hardware experiments needed. Not before (a). |
| (d) CPU-only tiny models | Works for a narrow job (second-stage classifier on a crop, 0.1 to 2 s), not for full-frame detection. |

## 1. What the camera software tells us

### 1.1 Model container (HIGH on layout, LOW on semantics)

Little-endian, 0x30-byte header, then two sections. Numbers below are from the stock file.

- Words: `0` format version (2), `1` header size (0x30), `2` size of section A (0x22f4), `3` size of section B (0xee490), `4` total
  size (equals the file size; header + A + B adds up exactly), `5` model label (`0x0A00000D`), `6` version (1.00.01 as bytes),
  `7..11` zero (reserved). The driver and libs check size and label (`header.model_size != model_size`, "model_id and
  header.model_label conflict"), so a label that does not match the id requested through `model_type` is rejected.
- Section A (0x22f4 bytes) is a **layer descriptor table**. It starts with a small block (counts 7 / 114 / 110, input channels 3,
  width 320, height 192, buffer sizes such as 320x192x3), then 110 records of 16 bytes (kind 5 or 4 per layer, layer index, feature
  width/height, channel count, a flag). Spatial sizes fall 160x96, 80x48, 40x24, 20x12, 10x6 and channels grow 16 to 256: a
  MobileNet-like depthwise/pointwise backbone with an SSD-style head, matching the `slim320` description in `libakv_cnn_dt` (MEDIUM).
  A second part holds per-layer convolution descriptors (kernel, strides, input/output layer ids with 0xffff for unused,
  scale-looking fields), consistent with the "Check User Input" field names of `libakv_cnn_dt` (kernel_ch, ds_conv, norm_conv, padded
  width). Field-by-field meaning is not decoded.
- Section B (0xee490 bytes, about 970 KB) is the weight blob. Byte entropy is 5.9 to 6.6 bits per byte over the whole file, uniform
  across blocks: not obviously compressed or encrypted, consistent with 8-bit quantised weights (MEDIUM). Not proven.
- Integrity: no stored CRC/hash found over the file or either section (tried CRC-32/MPEG-2 and plain variants over the usual ranges
  against the header words; reserved words are zero). No signature or encryption routine in `libmpi_svp2`, `libak_nne` or
  `libplat_npu` (no AES/SHA/MD5 symbols or strings). `libakv_cnn_dt` checks its own parameter file with `crc32_mpeg_2` and a header
  pattern `param_DT_<n>_idx<m>_V<a>.<b>.<cc>`: that is the older AV100 user-space format, not this file's. So validation in the new stack
  is size + label (+ version) only (MEDIUM; driver checks were read as strings, not fully traced).

### 1.2 Library layers (HIGH)

- `libplat_npu` V2.0.05: opens `/dev/npu`, `ak_npu_init/deinit/reset_hw/command` (generic ioctl pass-through), calls `NNE_Init`.
- `libak_nne` (`libplat_nne` V1.1.05, 9 KB): a **thin generic layer**: `NNE_Create_CH` (model pointer + size, or path), `NNE_Run` /
  `NNE_Run_Interrupt` (input blobs {virt, phys, size, w, h, c, scale, memory_type} in, output blobs out), `NNE_Get_Model_Header`,
  `NNE_Destroy_CH`, `NNE_Get_Lib_Version`. The driver traces show the NNE ioctl takes blob arrays, so this is the path that runs a layer
  table without post-processing: raw output feature maps. It carries no model-id logic.
- `libmpi_svp2` V2.0.26 (21 KB): the fixed-function pipeline `ak_svp2_create_chn/process/release/get_param/set_param`. It resizes
  the input (`yuv420sp_roi_resize_rgbline`, ROI crop supported), then calls the driver's "NNA" path (`IOC_NNA_CREATE_CHN`,
  `IOC_NNA_PROCESS`), which does inference **and post-processing in the kernel driver**. Input types: YUV420SP or RGB line. The model
  can come from a path (`conf_mode 0`) or a memory buffer (`conf_mode 1`), and the lib mentions an optional custom reader
  (`pReadFunc`), otherwise the default parser. Parameter families seen in the range checks: `hd_param` (classify 0..1000, IoU 0..10,
  target_type), `fas_param` (face anti-spoof), `fr_param` (face recognition, cosine distance), `gesture_param`. `model_type` must be <= 47.
- `ak_npu.ko` 2.0.17 (licence string GPL, author Anyka; DT compatible `anyka,ak3918av130-npu`, also `km01a-npu` and
  `ak3918av100-uio-cnn`): holds the per-model post-processing. Its strings name the model ids it knows.

### 1.3 Model ids the driver knows (MEDIUM)

From the process/result error strings of `ak_npu.ko`: `0x0A000007` to `0x0A00002D` (about 35 ids), plus `0x0A010002`, `0x0A010004..06`,
and two numeric families named "FAS 10003" and "20002". Named families: `HD_180P`, `HD_360P` (human detection at 180p/360p),
`FACE_DT_180P`, a face-recognition model, a FAS (anti-spoof) model, a gesture parameter family in svp2, and `td07..td2D` parameter
blocks (a detector parameter set per id). What each id is (person, head, face, pet, vehicle, gesture) is **not stated in text**: driver
and lib give ids, not names. Only `0x0A00000D` (class 0 person, class 1 head, from the walk test) is confirmed. `libakv_cnn_dt` names its
networks `slim320`, `slim320_face`, `slim480`, `slim640`, `slim640_pets`: Anyka has a pet detector at least in the AV100 generation (a
name, not proof that the AV130 ids include one). LOW on any id-to-class mapping beyond `0x0A00000D`.

### 1.4 What `libakv_cnn_dt` is (HIGH)

V1.19.02, 94 KB, not loaded by `yy_cam`. It is the user-space detector for the older AV100 CNN block: it programs the CNN registers itself
(`AKCNN_Init`, `cnn_driver_mode_2`), holds compiled-in network descriptions (frame size, layer table, score/location layer indices,
anchor sizes, 5,875 candidate boxes for slim320) and its own CRC-32/MPEG-2 checked parameter file. On the AV130 this logic moved into the
kernel driver. Its `DT_*` entry points are a reference for layer constraints (supported convolution modes, "Table1"), not something to
call on this camera.

### 1.5 Consequence

The topology and anchor logic for each id is **compiled into the driver**. The model file carries only layer descriptors and weights for
that topology. With the stock stack a new model works only if (1) its id is one the driver knows and (2) its layer table and weights
match what that id's post-processor expects. A different network shape has no post-processor, except through the raw `NNE_Run` path,
where decoding boxes and NMS would run on the CPU (cheap).

## 2. Public knowledge (provenance cautions)

Searches (English and Chinese, 2026-11) found **no public Anyka NPU toolchain, converter, SDK documentation, or third-party model file**
for AV100/AV130/AK37E. Terms tried: "AK NNE", "anyka npu tool", `libakv_cnn_dt`, `libmpi_svp2`, `ak_svp2_create_chn`, `libplat_npu`,
`model_0x0A`. Every hit was another vendor (Rockchip RKNN, Axera Pulsar2, Amlogic, Allwinner V831). Only tangential material:

- Anyka SDK/firmware hacking repos for older, NPU-less AK3918 (not read in depth, none confirmed to mention an NPU; licences vary,
  firmware images in them are vendor property): https://github.com/Nemobi/Anyka,
  https://github.com/MuhammedKalkan/Anyka-Camera-Firmware,
  https://github.com/Lawliet95/ANYKA-Tuya-Hacking-Journey-AK3918v200EN080-v200
- OpenIPC Anyka vendor page, no AV100/AV130 and no NPU work: https://openipc.org/cameras/vendors/anyka
- Effort-scale precedent for reverse-engineering a small NPU (not Anyka):
  https://www.cnx-software.com/2021/04/29/allwinner-v831-npu-neural-processor-unit-reverse-engineered/

Cautions: Anyka model files and libs are vendor-copyrighted and usually ship inside OEM firmware (Tuya cameras, other brands) with no
redistribution licence. Never commit them; Akam could at most support "bring a model file from a device you own". The module
declares a GPL licence, so the source of `ak_npu.ko` can in principle be requested from whoever shipped it: that would settle the
post-processors and ioctl layout legally (UNVERIFIED, no answer expected quickly).

## 3. Paths, ranked

### (a) More official model files from other products (best; MEDIUM feasibility; legal risk)

Other AV130 products (Tuya cameras and others on an Anyka AI SoC) may ship further `model_0x0A0000NN_*.bin` (face, pet, vehicle,
gesture). The driver already contains post-processing for about 35 ids, so such a file would likely load through `ak_svp2_create_chn`
with the right `model_type`, with no new code beyond mapping the id and reading classes. Cost: find firmware from devices the
maintainers own, extract the file, test on our camera (size/label checks reject mismatches harmlessly). Caveats: the `model_type` (0..47)
to id mapping is unknown (try each; wrong ones fail at load), classes are unlabelled until observed, redistribution is not allowed, and
input size may differ (svp2 resizes and supports ROI).

### (b) Generic NNE API plus a public converter (not available)

`NNE_Create_CH` / `NNE_Run` would run any layer table and return raw feature maps, which is what a custom detector needs. But no
public converter produces the Anyka container. Revisit if a toolchain surfaces. Any converter must respect hardware limits visible in
`libakv_cnn_dt` (convolution modes, kernel/channel rules, int8-like scales).

### (c) Reverse-engineer the container and write our own models (hard; LOW-MEDIUM feasibility)

Known: file framing, a layer-descriptor table, a quantised weight blob. Missing: meaning of the 16-byte records and per-layer
descriptors, weight layout (channel order, hardware tile padding), quantisation scale rules, register-level constraints. Method:
differential experiments on the owned stock file (change one weight or scale, observe the output on our own camera), the
`libakv_cnn_dt` layer checker as a spec, and the driver GPL source if obtainable. Then an exporter (PyTorch int8 to this container)
for the same slim topology, retrained on pets/vehicles. Estimate: several weeks of RE plus a training pipeline; real risk that
driver-side per-id limits (anchors, score-layer indices) force reusing a stock id's topology exactly. Payoff: arbitrary classes at
stock-like cost (27 ms, about 5 % CPU). Only after (a) fails and a concrete use case exists.

### (d) CPU-only tiny models (fallback; narrow use)

ARM926EJ-S (ARMv5TE, about 300 MHz), no FPU, no NEON; only the 16x16 DSP multiply-accumulates help. Estimate (LOW, must be
measured): int8 convolution about 10 to 20 MMAC/s in plain C, 30 to 60 MMAC/s hand-tuned. A 5 MMAC classifier (64x64 crop) then
takes 0.1 to 0.5 s, a 20 MMAC model 0.5 to 2 s; a full-frame detector at 160x96 (100+ MMAC) is out of reach and would starve the
encoders and RTSP. Float is out (soft-float is far slower). Good use: two-stage, an NPU person box or motion triggers a crop and a tiny
CPU classifier labels it (cat/dog/bird, person vs not) at about 1 Hz under event. Keep weights under 1 MB (RAM is tight).

## 4. Next steps (offline first)

1. On an owned camera, call `ak_svp2_create_chn` with the stock file for each `model_type` 0..47 to learn the type-to-id rule (expected
   accepted: 10 only).
2. List `model_0x0A...` file names in other AV130 firmware images the maintainers legally own (names alone reveal ids).
3. Ask the vendor for the GPL source of `ak_npu.ko` 2.0.17.
4. Benchmark (d) on the camera CPU before any claim (needs the cross toolchain, separate task).

Open questions: id-to-class mapping for every id except `0x0A00000D`; whether `NNE_Run` accepts a layer table the driver does not
know (likely yes at blob level, MEDIUM); the quantisation format; whether section B is encrypted (entropy says unlikely).
