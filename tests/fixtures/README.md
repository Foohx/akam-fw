# Test fixtures

`app_fake/` is a SYNTHETIC stand-in for an extracted APP partition tree:
hand-written, no vendor content (no Tuya/Anyka files). It only mimics the file
layout (executables, a non-executable file, a symlink, boot-script shapes) so
build and patch tooling can be tested without a camera dump.

Note: `bin/` and `*.so*` match patterns in the repository `.gitignore`; the
fixture files are added with `git add -f` until the `!/tests/fixtures/**`
negation is in place.

`scene/wall_80x48.gray` is a 3840-byte raw 80x48 luma thumbnail (the one
the app computes for the scene check: 1152x648 snapshot of a plastered white
wall, area-scaled to 320x192, box-filtered 4x4). It is a close wall with a
window edge and a blue cloth, no person, nothing personal. The scene unit
test derives its shifted / noisy / exposure-changed / vignetted / flattened
variants from it.
