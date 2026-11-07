# Test fixtures

`app_fake/` is a SYNTHETIC stand-in for an extracted APP partition tree:
hand-written, no vendor content (no Tuya/Anyka files). It only mimics the file
layout (executables, a non-executable file, a symlink, boot-script shapes) so
build and patch tooling can be tested without a camera dump.

Note: `bin/` and `*.so*` match patterns in the repository `.gitignore`; the
fixture files are added with `git add -f` until the `!/tests/fixtures/**`
negation is in place.
