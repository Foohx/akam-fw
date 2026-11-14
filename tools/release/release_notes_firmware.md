# Akam firmware <version>

Draft. Not published. `tools/release/make_release.sh firmware` writes the filled copy (`<version>`, `<date>`, `<key>`, the kit list, the CHANGELOG section, absolute links) next to the release directory.

Released on <date>. Open firmware add-ons for LSC Smart Indoor cameras: one **kit** per camera model. It is
**experimental**. Read the [kit documentation](manual/kit.md) and keep a verified backup of your stock camera before you
flash anything: a flash can brick a camera if the power is cut during the write. Pick the kit of YOUR model
(see [compatible cameras](compatible-cameras.md)); the `akam` assistant
([assistant releases](https://github.com/Foohx/akam-fw/releases?q=assistant-v)) does that for you.

## Kits in this release

<kits>

A kit holds Akam's own files only; it never contains a camera image. This release also holds
`akam-firmware-<version>-src.tar.gz` (the source, with the bash wizard `./akam`), `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `SHA256SUMS` and `SHA256SUMS.sig`.

## What is new

<changelog>

## Verify your download

`SHA256SUMS` lists every file of the release; `SHA256SUMS.sig` signs it (SSH signature, namespace and principal
`akam-release`). The signature only proves the files come from the Akam maintainer; it is not a warranty.
Release public key: `<key>` (also in `tools/release/allowed_signers` and the README).

```sh
echo 'akam-release namespaces="akam-release" <key>' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I akam-release -n akam-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS       # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
```

GitHub's automatic "Source code" archives are not covered by the signature: use `akam-firmware-<version>-src.tar.gz`.
`akam get-kit` verifies the kit itself; `tools/release/verify_kit.sh` needs GNU tools (Linux). Steps per OS:
[assistant.md](assistant.md).
