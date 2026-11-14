# Security policy

## Reporting a vulnerability

Please report privately, not in a public issue: use GitHub's private vulnerability reporting (the **Security** tab of
the repository, "Report a vulnerability", i.e. a repository security advisory). Include the version, the camera model,
the steps to reproduce, and the impact. Do not attach a backup, a dump or a firmware image.

Akam is a personal, non-commercial project. I answer when I can and have no fixed response time.

## Scope

In scope: the camera firmware (`ak_rtsp` and the camera scripts), the PC assistants (`./akam` bash wizard and the Go
`akam` program), and the signing and verification of the release kit.

Out of scope: the stock firmware and its vendors, Dropbear, SmolRTSP and other upstream projects (report to them),
attacks that need physical access to the camera (the microSD maintenance flag and UART are root access by design),
and exposing the camera to the Internet, which the documentation says never to do.

## Bounty

There is no bug bounty. Credit in the changelog is offered if you want it.
