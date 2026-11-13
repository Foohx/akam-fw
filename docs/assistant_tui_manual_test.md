# Assistant screen: manual test checklist

A short check of the `akam` home screen and its main screens on the terminals people really use. The automated tests cover
the logic and the layout; only a person can say whether colours, glyphs and keys feel right in a real terminal. Nothing
here touches a camera: use `--dry-run` where a step would write (the red box then does not open) or press `n`/Esc at every
question. For hardware tests see the [hardware test plan](assistant_hardware_test_plan.md).

## Terminals to run

| # | Terminal | Notes |
|---|----------|-------|
| 1 | Windows 10, conhost (the classic console window, `akam.exe` from cmd or PowerShell) | uses the console's VT mode; try a raster font and Consolas |
| 2 | Windows 10, Windows Terminal | PowerShell and cmd tabs |
| 3 | macOS Terminal.app | also iTerm2 if you have it |
| 4 | A Linux terminal (gnome-terminal, konsole, xterm...) | |
| 5 | tmux (inside any of the above, `TERM=screen-256color` or `tmux-256color`) | resize the pane while the program runs |
| 6 | 16 colours: `TERM=linux` on the Linux console, or `TERM=xterm` with a 16-colour palette | glyphs fall back to ASCII on the Linux console |
| 7 | No colour: `NO_COLOR=1 akam` (PowerShell: `$env:NO_COLOR=1`) | the screen must stay readable and keep its structure |
| 8 | Small window: 60 columns by 16 lines, then 40 by 8 | |

Also run once with `AKAM_ASCII=1` (forces ASCII glyphs) and once with `--plain` (no screen, line-by-line text).

## What to look at, per screen

Write PASS, FAIL or SKIP (why) and what you saw. "Fits" means nothing wraps badly, nothing is cut mid-word at the right edge
and no stray characters (`?`, boxes, `[`...) appear.

| Screen | How to get there | Keys to press | Expected |
|--------|------------------|---------------|----------|
| Home | run `akam` | Up/Down, `j`/`k`, a digit 1 to 8 (shortcut, then answer `n` or Esc to the first question), `?`, `q` | Header (product, camera name, address, badges) over a list; one line of keys at the bottom; the selected row is clearly marked; the status colours (ok, warning, error) differ and each has a word or glyph, not colour alone; `?` opens details and Esc closes them; `q` quits; nothing says "Press Enter" |
| Camera chooser and scan | `c` on the home | `s` then `n` (no packet is sent), `s` then `y` (scan of your own network; stop with Esc), `o` then `n`, `t` (type an address, Esc), Esc | Empty state tells you to press `s` or `t`; the question defaults to No (Enter, Esc, `q` all mean No); the scan shows a status line with a percentage and rows appear as cameras answer; Esc stops it at once and goes back |
| Options form | choose "Camera options" (needs a camera, or use `--dry-run` with a remembered camera) | Up/Down, Left/Right or Space on a choice, type a number and Enter, `p` (password row is hidden), Esc | Six groups (Video, Picture, LED, Audio, Network, Security) on one screen that scrolls; a change shows `SET ...` then a one-line result; typed passwords never show, not even as `*`; Esc leaves without asking anything |
| Backup progress | "Back up / restore", then back up (a camera, or `--dry-run`) | Esc once while reading, `?`, resize the window | Steps listed with a spinner on the current one; a bar with size, elapsed time and time left; Esc says it stops after the current step and nothing on the camera changes; `?` shows the log and Esc closes it; the bar redraws cleanly after a resize |
| Red box | "Prepare the backup microSD card" or "Update this camera" until the box (use a spare card; or `--dry-run`, where it must **not** open and only a `WOULD ask` line shows) | Enter, Esc, `q`, Ctrl-C, then run again and press `y` only on a card you can erase | The box is red (bold uppercase title in no-colour and ASCII), shows the command, and No is highlighted; Enter, Esc, `q` and Ctrl-C all mean No and nothing happens; waiting a minute changes nothing; only `y` (or Left then Enter) means yes |
| Which firmware? | "Set up a new camera" (a card-less check: stop at the list) or "Update this camera" on an Akam camera, with and without a network | Up/Down, a digit, Enter, `/` (filter), Esc; pick an older release, then answer Enter (No) | Title with what the camera runs; `Akam X.Y.Z (latest)` first and selected, older versions marked `older`, then downloaded kits, "A kit file or folder...", "An image file you built (.squashfs)...", and in Update with a verified stock backup "Stock firmware (from your backup)  - never tested on a real camera"; no stock row in the first install; offline or rate-limited: a warning line, only the local rows and a "Try again to read the releases" row; with an image already built from your backup and no builder, that image is the first row; a release whose image cannot be built here is refused and nothing changes; an older release asks a downgrade yes/no with No as the default and nothing is downloaded on No; an image path shows its md5 and a "you take responsibility" question (No by default); Esc goes back and nothing changes |
| SSH key | in "Camera options", Security group, look at the row `SSH key` (`shared (~/.ssh/akam)`, `this camera only (...)` or `custom (...)`), press `k` | `k`, Up/Down and Enter on the question, a passphrase (not shown), `y`/`n` | A read-only row; `k` asks "Which SSH key should open this camera from now on?", then a yes/no (default No) that says what changes, then the write (no stop offered while it runs), then "The new key works ..."; a last yes/no (default No) offers to remove the old key; Esc and No change nothing; no key or passphrase appears in the transcript |
| Configs | "Configs" on the home (digit `9`), then "Edit the global config" (no camera needed) | Up/Down, Left/Right on a choice, type a number and Enter, `s` (passwords, Wi-Fi), `c` (copy from a camera), Esc | The Options-style form on the template: every row starts as "not set" or its saved value; a change is saved at once (no write to any camera); a bad number is refused and the row snaps back; `s` asks "Keep ... in the global config? They are stored on this computer in a private file; anyone with access to your account can read it." with default No, and a typed password is never shown; "Apply the global config to this camera" lists only the differences (passwords as `password: set` or `asked`), asks a yes/no (default No), the video restart is a separate question (default No), and no write is offered a stop while it runs; the Options title then reads `settings: local (from global)` |
| Error screen | stop a camera (or type a wrong address in the chooser) and run "Update this camera" | `l`, Esc, `r`, `c` | Red title in words, what happened, what to do, a muted technical line; `l` opens the log tail, Esc closes only the log; `r` retries; `c` offers another camera; Esc goes back home; no password or key appears anywhere |
| Write in progress (hardware plan only) | during a flash write | Esc, Ctrl-C, `q` | Nothing stops or quits; the footer says the write cannot be interrupted safely; the keys work again when it ends |

## Checks that apply to every screen

- **Colour:** in 256 colours the palette is readable on both a light and a dark background; in 16 colours the red box and
  errors are still red (not invisible); with `NO_COLOR=1` there is no escape-sequence colour at all and warnings still read
  as warnings (`[!]`, `[x]`, `[ok]` or words).
- **Glyphs:** boxes and spinner look right (UTF-8), or fall back to plain ASCII on the Linux console and with `AKAM_ASCII=1`.
- **Width:** a camera name with accents or CJK characters keeps the columns aligned.
- **Resize:** shrink and grow the window while on the home, the chooser and the form. The screen redraws without leftovers.
  At 40 by 8 it keeps its title and its key line.
- **Keys:** arrow keys, Enter, Esc, `q`, `?`, Ctrl-C and the mouse wheel do what the key line says. On Windows conhost also try
  Esc while typing in a field. In tmux check that Esc is not delayed for a long time (set `escape-time 10` if it is).
- **Quit:** `q` or Ctrl-C leaves the terminal as it found it: cursor visible, no colour left on, no alternate screen stuck, typed
  characters echoed again.
- **No camera needed for safety:** on the red box never press `y` unless you meant to; the tester must be able to read the
  command in the box before answering.

## Report

Record the terminal name and version, `akam version`, the OS, and for each FAIL a screenshot or the exact text on screen.
