# Contributing to Akam

Thank you for helping. Akam is a small personal project: please keep changes focused and tested.

## Licence and sign-off

- Contributions are under the [MIT licence](LICENSE) (inbound = outbound).
- Every commit of a contribution submitted by pull request needs a `Signed-off-by:` line (`git commit -s`), by a
  **human**, following the [Developer Certificate of Origin 1.1](https://developercertificate.org/). This also applies to changes written with
  AI assistance: the person who signs is responsible for them.

## What must never be committed

- vendor binaries, firmware images, dumps or backups of a camera;
- scripts or configuration files extracted from the stock firmware, decompiler output;
- captures of the stock firmware (`isp_blocks.bin`, `stock_audio.raw` and similar);
- third-party code without a compatible licence and an entry in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md);
- personal data in fixtures: use the documentation addresses of RFC 5737 (`192.0.2.x`, `198.51.100.x`,
  `203.0.113.x`), no real names, Wi-Fi names, serial numbers or MAC addresses.

## Where code goes

`src/` is shared code only (core, net, api, features, hal: no vendor header, no device path, no model name), `platforms/<soc-family>/` holds the drivers and
process glue of one SoC family, `firmware/<model-id>/` only what is specific to one camera (model.mk, bindings, boot scripts, image builders; the model
sheet is `tools/release/models/<model-id>.txt`). That is the **target**. **Current state, honestly:** the 1080p model directory is close to it, the 2K one
is not: `firmware/lsc-rotatable-2k-ak3918av130/` still holds about 9 000 lines of feature code (`ptz.c`, `settings.c`, `track.c`, `scene.c`, `patrol.c`, `button_rt.c`,
`night_ctl.c`, `zones.c`, `vlive.c`, ...) next to its real camera facts (`hal.c`, `ptz_desc.c`, `pin.c`), and the 2K platform and that directory include each
other's headers, so a second camera on the 2K platform cannot yet be added without copying them. The planned split (pin and sensor facts into a model
descriptor, then the pure files to `src/features/` or `src/core/`, then `ptz`/`track`/`scene`/`settings` behind ops tables, each step gated by the 2K golden)
is described in [docs/hacking.md](docs/hacking.md#repository-layout). Do not add new feature logic to a model directory. A new camera or SoC adds a directory under `firmware/` or `platforms/`; see [docs/porting/TEMPLATE-new-camera.md](docs/porting/TEMPLATE-new-camera.md)
and [docs/hacking.md](docs/hacking.md#repository-layout). `tools/dev/path-map.txt` maps the old paths to the new ones for a branch cut before the move.

## Checks

```bash
bash tools/build/bootstrap.sh       # once: pinned toolchain
tools/ci/local_check.sh             # shellcheck, all tests, hygiene (run only one at a time)
bash tests/readme_links_test.sh     # when you change documentation
make help                           # root Makefile: 1080p, 2k, test, check, repro, clean (wraps the scripts above)
```

Details: [hacking guide](docs/hacking.md#tests).

### CI

GitHub CI (`.github/workflows/ci.yml`) runs on pull requests and on pushes to `dev`, and runs the same code as
`local_check.sh`, one group at a time: `tools/ci/local_check.sh --only lint|shell|native|go|race`. `lint` always
runs; `shell`, `native` and `go` run only when the diff touches them (`tools/ci/changed_groups.sh`; a docs-only
change runs `lint` only). It builds and uploads no release: releases are built and signed locally.

## Style

- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:`...).
- Code, comments, documentation and commit messages are in **English**.
- Docs: plain, short sentences. Do not claim "validated" for something that was only compiled or host-tested
  ([status labels](specs/README.md#status-labels)).
- Behaviour changes need the matching update in [`specs/`](specs/README.md).
