# Akam assistant <version>

Draft. Not published. `tools/release/make_release.sh assistant` writes the filled copy (`<version>`, `<date>`, `<key>`, the CHANGELOG section, absolute links) next to the release directory.

Released on <date>. `akam` is a single static program that guides you through putting Akam on a supported camera and
managing it afterwards (backup, install, update, options, `talk`). It downloads the camera firmware kit itself from the
[firmware releases](https://github.com/Foohx/akam-fw/releases?q=firmware-v), verifying its checksum and signature.
It is **experimental**: keep a verified backup of your stock camera before you flash anything.

Files: `akam-<os>-<arch>[.exe]` and archives for six targets (Linux, macOS, Windows; amd64 and arm64), `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `SHA256SUMS` and `SHA256SUMS.sig`. The binaries carry no Windows or Apple certificate:
SmartScreen, Gatekeeper and the UAC prompt ("Unknown publisher") warn, so check the SHA256 first (below).

## What is new

<changelog>

| Target | File | Status |
|---|---|---|
| Linux amd64 | `akam-linux-amd64` | partly tested on real cameras |
| Windows amd64 | `akam-windows-amd64.exe` | an earlier build was tested on a real camera |
| Linux arm64, macOS arm64 and amd64, Windows arm64 | `akam-<os>-<arch>` | built, not hardware-tested |

Details: [testing status](testing-status.md). Testers are welcome:
[hardware test checklists](hardware-test-checklists.md#assistant-checklists).

## Verify your download

`SHA256SUMS` lists every file of the release; `SHA256SUMS.sig` signs it (SSH signature, namespace and principal
`akam-release`). The signature only proves the files come from the Akam maintainer; it is not a warranty.
Release public key: `<key>` (also in `tools/release/allowed_signers` and the README).

```sh
echo 'akam-release namespaces="akam-release" <key>' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I akam-release -n akam-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS       # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
```

Steps per OS: [assistant.md](assistant.md).
