# Contributing to Akam

Thank you for helping. Akam is a small personal project: please keep changes focused and tested.

## Licence and sign-off

- Contributions are under the [MIT licence](LICENSE) (inbound = outbound).
- Every commit needs a `Signed-off-by:` line (`git commit -s`), by a **human**, following the
  [Developer Certificate of Origin 1.1](https://developercertificate.org/). This also applies to changes written with
  AI assistance: the person who signs is responsible for them.

## What must never be committed

- vendor binaries, firmware images, dumps or backups of a camera;
- scripts or configuration files extracted from the stock firmware, decompiler output;
- captures of the stock firmware (`isp_blocks.bin`, `stock_audio.raw` and similar);
- third-party code without a compatible licence and an entry in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md);
- personal data in fixtures: use the documentation addresses of RFC 5737 (`192.0.2.x`, `198.51.100.x`,
  `203.0.113.x`), no real names, Wi-Fi names, serial numbers or MAC addresses.

## Checks

```bash
bash tools/build/bootstrap.sh       # once: pinned toolchain
tools/ci/local_check.sh             # shellcheck, all tests, hygiene (run only one at a time)
bash tests/readme_links_test.sh     # when you change documentation
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
