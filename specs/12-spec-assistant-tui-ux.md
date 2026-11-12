# Akam - Specification: a colourful, fluid assistant TUI (phase 12)

Status: proposal for review, written for the coordinator. Language of the tool: English (spec 02, DA2).
Nothing here was run against a camera. File references are against `dev` (`tools/assistant` is the Go
module; paths starting with `internal/` or `cmd/` are relative to it). Builds on specs/09 and spec 11
(section 3.1, the home screen). Addresses in mockups are RFC 5737 (192.0.2.x).

User request: "make the TUI prettier: colour, never ask to press Enter to continue, a super simple and
fluid experience".

---

## 0. Summary

1. **Root cause of every "Press Enter"** is architectural, not textual. Each widget (`tui.UI.Pick`,
   `AskYN`, `Prompt`) and the full-screen `Menu` is a *separate* `tea.Program`; the menu runs in the
   alternate screen, the flows print with `fmt.Fprintln` on the normal screen between programs, and when
   the home menu returns it hides that output. `cmdHome` (`flows_home.go:142`) and `cmdStepMenu`
   (`misccmds.go:341`) therefore stop with "Press Enter" so the user can read what just happened.
   Fixing the wording would not fix the experience; the output needs a place to live on the screen the
   user returns to.
2. **Design**: one long-lived full-screen session (one `tea.Program`, alt screen) with a fixed layout
   (header with camera and health badges, main panel, result/toast strip, footer keys). Flows keep their
   code written once against `ui.UI`; they run in a goroutine and the session UI turns `Say/Good/Warn/Err`
   into entries of the result strip and `Pick/AskYN/Prompt/Danger/Progress` into in-session overlays.
   No program switch, nothing to hide, so nothing to acknowledge.
3. **Colour**: one theme file, semantic roles (ok, warn, error, accent, muted), `CompleteColor`
   (truecolor / 256 / 16) values, no terminal background query, text/glyph badges so colour is never the
   only signal, ASCII glyph fallback for the legacy Windows console.
4. **`--plain` is untouched**: same strings, same prompts, same exit codes. Plain tests and
   `tests/assistant_test.sh` are the regression net; the TUI is tested with model unit tests and golden
   screens (no terminal scraping).
5. **Delivery**: 10 small tasks (T1..T10), two developers in parallel on disjoint files. The first two
   (theme + restyle, result strip instead of "Press Enter") already give the user colour and no Enter
   before the session architecture lands.

Safety rules (unchanged, restated as acceptance criteria in 6.4): destructive actions ask in a red box with
default No (Enter, Esc, q = No); the tool never reboots a camera; the command is shown before it runs;
every yes/no defaults to No; nothing is sent on the network before the user agrees to a scan.

---

## 1. Audit of the current experience

### 1.1 Code map

| Piece | File | Notes |
|---|---|---|
| UI interface | `internal/ui/ui.go` | `Say Bold Good Warn Err Hint Blank Fail Prompt Secret AskYN Danger Pick Progress Banner` |
| Plain (line mode) | `internal/ui/plain.go` | raw ANSI via `paint` only when `Color`; text cleaned by `textsafe`; progress prints every 10 % |
| TUI wrapper | `internal/ui/tui/tui.go` | `tui.UI` embeds `*ui.Plain`: messages are the plain ones, widgets are tea programs; each widget = `tea.NewProgram(...).Run()` |
| Widgets/models | `internal/ui/tui/models.go` | `YesNo`, `Input`, `Picker`, `Menu`; styles: bold, faint, ANSI 1/2/3 and `Reverse` |
| Selection of UI | `cmd/akam/main.go` (`makeUI`) | plain when `--plain`, `AKAM_PLAIN=1`, or stdin/stderr not a terminal |
| Home | `internal/cli/flows_home.go` | `cmdHome`: `Menuer.Menu` (alt screen) or `Pick` |
| Step menu | `internal/cli/misccmds.go` | `cmdStepMenu`, same pattern |
| Chooser | `internal/cli/flows_camera.go` | `chooseCamera`: table in `Say`, then a `Pick` of rows + 5 actions |
| Options | `internal/cli/flows_options.go` | `flowOptions` -> `groupScreen` (`Pick` of option lines, `Pick`/`Prompt` of a value, "Apply N pending") |
| Health | `internal/cli/camhealth.go` | `showHealth`, `clockOffer`, `ntpOffer`: several long sentences each visit |
| Backup/flash/update | `flows_backup.go`, `flows_update.go`, `flashcmds.go` | banners, `Progress`, many `Say` lines |

### 1.2 Places that block without a safety reason

| # | Where | Today | Verdict |
|---|---|---|---|
| B1 | `flows_home.go:142` | "Press Enter to go back to the home screen." after every home action, TUI only | remove (result strip on home, T2) |
| B2 | `misccmds.go:341` | "Press Enter to go back to the menu." after each step/tool | remove (same mechanism) |
| B3 | `flashcmds.go:602` | "Press Enter when the camera is powered again (or q to stop)" (verify without the card) | physical action; keep one key press but render as a checklist step; optional auto-detect (Q2) |
| B4 | `telnetcmds.go:79` (`manualUI.Wait`) | "Press Enter when done (or type q to stop)" for hand steps (reset, card) | physical action; same treatment as B3 |
| B5 | `flows_options.go` `groupScreen` | edit a value = open `Pick`, choose, back to the list, choose "Apply N pending", then the video-restart question | in-place edit, apply at once (4.4) |
| B6 | `flows_options.go` `flowOptions` | clock/Wi-Fi paragraphs and a clock yes/no printed *every* time the group list is shown or Reload is chosen | badges + on-demand action (4.5) |
| B7 | `flows_camera.go` `chooseCamera` | after each scan/check the screen is reprinted from the top with a table, a note and an unconditional "no camera known" paragraph | one persistent list, live updates (4.3) |
| B8 | every `Pick` with a trailing "Back" / "Cancel" item | an extra row to select for what Esc already does | drop the row in the TUI, keep it in plain |
| B9 | `networkScreen`, `securityScreen` | a one-shot `Pick`, one action, return to the options list with no feedback except scrolled text | inline form, toast |
| B10 | every `Pick` footer | "up/down or j/k, Enter, or the number; Esc cancels" repeated under each menu | one footer per screen, shown by the layout |

Not counted (kept on purpose): every `Danger` and `AskYN` that gates a write, the SSH host-key
fingerprint confirmation, the video restart question (it interrupts the stream), typed values.

### 1.3 Walls of text and colour today

- Menu detail lines are cut at the window width with "..." (`Menu.View`) and the explanation of Enter is a
  second faint sentence. The home header is two sentences ("seen 3 min ago", "(c: change camera)").
- Options entry (`optionLine`) prints label, value, `[a to b]` range and a description on one line
  (about 110 columns), wrapping on 80.
- `showHealth` and `clockOffer`: 3 to 6 sentences before the menu, repeated on each visit.
- Chooser: header row of fixed-width columns, then a two-line paragraph when empty.
- Colour: the TUI uses `lipgloss` colours "1", "2", "3" (red, green, yellow), faint, bold and reverse
  only. Selection is reverse video. `New()` forces `SetHasDarkBackground(true)`. The Plain colours are
  raw SGR 1/31/32/33. Status words in menus ("done", "todo") are the only other colour. No accent colour,
  no badges, no distinction between info and muted text other than faint.

### 1.4 What is already good (keep)

Safety gates and their wording, `textsafe.Clean` on everything a camera says, golden tests for the models
(`internal/ui/tui/testdata`, `flows_tui_test.go`), `lipgloss.SetColorProfile(termenv.Ascii)` in tests, the
`Menuer` seam, and the rule that commands only see `ui.UI`.

---

## 2. Design principles

1. **One screen, one place to look.** The user never wonders where the answer went.
2. **Nothing to dismiss.** Results are shown on the screen the user lands on and fade by themselves (toast)
   or stay as a small status (badge). A key press is only ever asked for a *physical* action.
3. **Short first, details on demand.** Every list line is one short clause (at most about 60 characters);
   `?` opens the long text.
4. **Show, then act.** The command about to run is shown in the activity line before it is sent
   (existing rule), and it is also written to the log.
5. **Colour has a meaning**, never decoration, and never alone: each coloured state also has a glyph or a
   word (also on no-colour terminals).
6. **Degrade, do not break**: 16 colours, no colour, 80x24, ASCII-only, resize.

---

## 3. Visual language

### 3.1 Palette and roles

Defined once in `internal/ui/tui/theme.go`; no other file may use a literal colour.

| Role | Use | Truecolor | ANSI256 | ANSI16 |
|---|---|---|---|---|
| `ok` | done, healthy, applied | `#2FA84F` | 35 | green (2) |
| `warn` | stale, clock off, not saved, restart needed | `#D9A400` | 178 | yellow (3) |
| `error` | failed, refused, destructive box | `#D6403A` | 160 | red (1) |
| `accent` | selection, titles, focus, key letters | `#2E8BC0` | 33 | cyan (6) |
| `info` | spinner, progress, in flight | same as accent | 33 | blue (4) |
| `muted` | secondary text, key hints, ranges | `#7A7F87` | 244 | faint attribute on default (not bright black: invisible on some palettes) |
| `text` | body | terminal default | default | default |

Implementation: `lipgloss.CompleteColor{TrueColor, ANSI256, ANSI}` per role; lipgloss and termenv pick the
profile. These mid-tone values keep contrast on both light and dark backgrounds, so the terminal
background is **not** queried (the current `SetHasDarkBackground(true)` workaround exists because that
query hangs on some terminals). `AdaptiveColor` is only used if a user sets `AKAM_THEME=light|dark` (Q3:
not shipped in V1).

Selected row: accent foreground + bold + a `>` marker (not reverse video, which looks heavy and is the
only signal on no-colour terminals; the marker covers that case). Danger box: red border and red bold
title, body in default colour.

### 3.2 Colour capability and `NO_COLOR`

Order of decision (first match wins): `--plain` or non-terminal -> plain, existing behaviour; `TERM=dumb`
-> plain; `NO_COLOR` set (or `--no-color`) -> no colour, attributes kept (bold, faint, markers); otherwise
the termenv profile of the output stream (stderr, see `tui.run`): TrueColor, ANSI256, ANSI or Ascii.
`COLORTERM=truecolor|24bit` selects truecolor; macOS Terminal.app is treated as 256 colours (termenv does
this). On Windows 10 bubbletea enables the VT mode; a console that refuses it falls back to plain.

### 3.3 Glyphs

Two sets chosen once by `theme.go`: `unicode` (rounded box drawing, check mark, bullet, braille spinner)
and `ascii` (`+ - |`, `[ok]`, `*`, `|/-\`). ASCII is used when: `AKAM_ASCII=1`; the locale is not UTF-8 on
Unix; on Windows when neither `WT_SESSION`, `ConEmuANSI` nor `TERM_PROGRAM` is set (legacy conhost raster
fonts lack the glyphs). Widths use `lipgloss.Width` (never `len`; the current `pad` uses `len`, a latent
bug for non-ASCII camera names, fixed in T1).

### 3.4 Badges

A badge is a coloured glyph plus one word, 12 characters at most:

```
 ok   (green)   ok ready     ok synced     ok -62 dBm        ascii: [ok]
 warn (yellow)  !  clock     !  weak wifi  !  not saved      ascii: [!]
 err  (red)     x  offline   x  refused                      ascii: [x]
 info (blue)    ~  checking  ~  42 %                         ascii: [~]
 mute (faint)   -  unknown   -  not seen                     ascii: [-]
```

(Mockups below use the ASCII forms.) Badge sources already exist: `camHealth` (`readCamHealth`,
`wifiLevel`, clock offset, ntp state), `discover.Row` (kind, last seen), `camopt.OptionState` (not saved,
unavailable). The spec adds no new camera query: the home screen still sends nothing (spec 11, 3.1).

---

## 4. Screens

All mockups are 80 columns. `>` is the selection. Real output uses rounded unicode borders where the glyph
set allows.

### 4.1 Layout (every screen)

```
 Akam 0.9  windows/amd64                                              (muted)
 salon  192.0.2.41  Akam 1.4.2          [ok] ready   [!] clock   [ok] -58 dBm
 --------------------------------------------------------------------------------
                                                         main panel (scrolls)
 --------------------------------------------------------------------------------
 [ok] Front LED set to off                                      (toast, fades 4 s)
 up/down move   Enter open   ? details   c camera   q quit       (footer, this screen)
```

- Header: line 1 = product, version, platform (muted); line 2 = camera name (bold), IP, firmware, badges.
  No camera: `no camera  - press c`. Width under 80: badges drop right to left (clock and Wi-Fi first); name
  and IP never. Width under 60 or height under 14: single-column fallback, 1-line header, no borders.
- Footer: drawn by the layout, never by the flow; key letters `accent`, words `muted`. `?` toggles the
  details pane (4.7). There is no hint sentence inside lists.
- Strip: the last message of `Good` (ok), `Warn` (warn), `Err` (error), `Say` (muted). It replaces "Press
  Enter": the result stays visible on the screen the user is on; an error stays until the next key.

### 4.2 Home screen

```
 Akam 0.9  windows/amd64
 salon  192.0.2.41  Akam 1.4.2          [ok] ready   [!] clock off   [ok] -58 dBm
 --------------------------------------------------------------------------------
 > Set up a new camera        first install from the stock firmware
   Update this camera         Akam to Akam, over SSH
   Camera options             LED, picture, video, audio, network
   Back up / restore          dump the flash, restore the original
   Find cameras               remembered, scan, typed address
   Settings                   backup folder, state folder, licences
   Check this computer        what works here, what is missing
 --------------------------------------------------------------------------------
 [ok] Backup saved: 3 files, 16.0 MB                                   (just now)
 up/down move   Enter open   c camera   ? details   q quit
```

- Digits 1 to 8 stay as hidden shortcuts (muscle memory; the plain `Pick` still prints numbers); `c`
  changes camera. Less noise on screen.
- The right column is the existing `Detail`, cut to one short clause (T9 shortens it for the TUI; plain
  keeps the full `Label  - Detail`).
- After any action the home shows its outcome in the strip (`[ok]`, `[!]` or `[x]` with a one-line
  summary); any key clears it. If the flow failed the strip is red and `?` opens the error details (4.7).

### 4.3 Camera chooser and scan

```
 Choose the camera
 --------------------------------------------------------------------------------
 > salon     192.0.2.41   [ok] Akam 1.4.2      seen 2 min ago
   garage    192.0.2.58   [~]  checking...
   porch     192.0.2.77   [-]  not seen        remembered
   (new)     192.0.2.90   [ok] stock           found by scan
 --------------------------------------------------------------------------------
 [~] Scanning 192.0.2.0/24 on 4 ports  ####------  38 %                 Esc stops
 up/down move   Enter use   s scan   o ONVIF search   t type   r recheck   f forget
```

- One persistent list; rows update in place while probes answer (probes already run concurrently:
  `probeMany`). No reprint, no "Check the remembered cameras now" row: it is `r`.
- `s` opens the existing yes/no first ("Scan 192.0.2.0/24, 4 ports on each address? [y/N]", default No)
  with a short line on what is sent; nothing is sent before `y`. Esc during a scan stops it.
- The 5 action rows become footer keys; "Back" is Esc (B8).
- Empty state: one muted line "No camera known yet. Press s to scan, or t to type an address."; the
  stock-firmware explanation moves to `?`.

### 4.4 Camera options, one group being edited

Group list on the left, the options of the focused group on the right, both always visible: no "go into a
group and come back".

```
 Camera options - salon  192.0.2.41  Akam 1.4.2          [ok] ready  [!] clock
 --------------------------------------------------------------------------------
   Video      1080p 15 fps    |  Brightness            [#####-----]  50
 > Picture    normal          |  Contrast              [######----]  60
   LED        on / auto       |> Saturation            [####------]  40   [!] not saved
   Audio      speaker 60      |  Sharpness             [#####-----]  50
   Network    ONVIF on        |  Mirror                  < off >
   Security   password on     |  Flip                    < off >
 --------------------------------------------------------------------------------
 > SET picture.saturation 40                                       sent, waiting
 left/right change   up/down move   Tab group   Enter type a value   ? details   Esc back
```

Interaction:

- Enum and on/off: left/right (or Space) cycles; applied once 400 ms pass without another key (debounce,
  so cycling does not send every step); the result is a toast.
- Number: left/right by 1, Shift+arrows or PgUp/PgDn by 10, or type digits then Enter. Same debounce. The
  bar is drawn from `camopt.Option{Min,Max}`.
- Text (host name, NTP servers, MQTT broker): Enter opens an inline field on the row; Enter applies, Esc
  cancels.
- `[!] not saved` is the warn badge for `r.NotSaved` ("lost at the next reboot"); `?` explains.
- Unavailable options (`st.Unavailable`) are muted with `needs update`; focusing one shows "update this
  camera to get it" in the strip. No dead-end sentence.
- Apply: each change is one `camopt.Apply` of one `Change`. The line `SET key value` is shown in the strip
  *before* it is sent (and logged as today). The "N pending, Apply, Discard" model is plain-only and stays
  there; the TUI applies immediately because these options are reversible and non-destructive. On failure
  the row snaps back to the real value and the strip is red with the refusal text.
- Video-restart options (`camopt.VideoRestart`): the value is sent at once; the strip says `[!] Saved.
  Applies when the video service restarts: r restarts it (about 20 s)`. `r` then asks the existing audited
  `restartVideo` yes/no (default No). One on-demand question, not one after every edit. Options that apply
  at the next power cycle say so; the tool never reboots or power-cycles.
- Anything destructive in this area (removing the video password) keeps its confirmation (4.8).

### 4.5 Clock and Wi-Fi: badges instead of paragraphs

| Badge | Meaning | Detail (`?`) |
|---|---|---|
| `[ok] ready` | SSH answers | - |
| `[ok] -58 dBm`, `[!] weak wifi`, `[x] no wifi` | `wifiLevel` | current `wifiReport` lines |
| `[ok] synced`, `[!] clock off 3 h`, `[!] clock unset` | `clockReport` | current `clockReport` lines |

When `clockReport` says `offer` (PC clock plausible, camera not NTP-synced) the footer gains `t set clock`.
`t` runs the existing question "Set the camera clock from this computer now?" (default No) with the
`clockLostNote` text as its single body line. It is never asked unprompted: the paragraph and the yes/no on
every visit (B6) disappear. The guards of `clockOffer` (never offer when the PC clock is implausible or the
camera is NTP-synced) are unchanged.

### 4.6 A long operation (backup, flash)

```
 Back up the camera - salon  192.0.2.41                          step 2 of 4
 --------------------------------------------------------------------------------
  [ok] Connect over SSH
  [ok] Check free space           12.4 GB free
  [~]  Dump the flash
        mtd2 rootfs    [##########----------]  52 %   8.3 / 16.0 MB   1.1 MB/s
  [ ]  Verify checksums
 --------------------------------------------------------------------------------
 > dd if=/dev/mtd2 bs=64k | gzip        (running on the camera, read only)
 Ctrl-C stops (nothing is written to the camera)
```

- Steps come from the existing `Banner` ("Step N/T - title") and `Progress(label, pct)` calls: the session
  UI keeps a step list; a spinner (braille or `|/-\`) animates in-flight steps; bars are drawn by hand (no
  new dependency).
- The command about to run is always visible while it runs (safety rule); it is also in the log.
- Speed and size are shown only when the caller provides them (optional detail argument); otherwise the
  bar shows percent only.
- At the end the panel collapses into one result line on the screen the user returns to (`[ok] Backup
  saved: 3 files, 16.0 MB`), `o` shows the folder path. No "Press Enter".
- Physical-action steps (B3, B4) are a checklist in the same panel:

```
  [ ] 1. Unplug the camera's USB-C power
  [ ] 2. Take the microSD card out
  [ ] 3. Plug the power back in
 > Space: done, then the assistant waits for SSH (180 s)             q stop
```

  One key (Space or Enter) acknowledges the whole checklist; the text prompt and "or type q" disappear.
  Optional (Q2): the assistant polls the SSH port, notices it go down then up and ticks the steps itself;
  the key stays as a fallback.

### 4.7 Details on demand and errors

`?` opens a bottom pane with the long text of the focused item (existing `MenuItem.Detail`, the old
paragraphs, the `Hint`), scrollable; `?` or Esc closes it. Long sentences move there; lists show the short
form.

Error screen (a flow returned `ErrFailed` or an error; every `Fail(msg, hint)`):

```
 Update this camera - salon  192.0.2.41
 --------------------------------------------------------------------------------
  [x] The camera did not answer on SSH within 120 s.

      What to do
        - check the camera's power and its Wi-Fi light
        - run "Find cameras" to see whether its address changed
        - nothing was written to the camera

 --------------------------------------------------------------------------------
 r retry   f find cameras   l log file   Esc back
```

- `Fail(msg, hint)` renders msg in `error` and hint under "What to do". `r` retries the flow when the flow
  registered a retry; `l` shows the log path; Esc returns home. No "Press Enter".
- Plain and the log keep the `ERROR: ` wording; in the TUI the prefix becomes the `[x]` badge. Camera text
  still passes `textsafe.Clean` first.

### 4.8 The red-box destructive confirmation

```
 +- ERASE THE SD CARD ---------------------------------------------------------+   (red border)
 |                                                                              |
 |  This erases everything on  /dev/sdb  (Sample 32 GB, 31.9 GB).               |
 |  The camera is not touched. This cannot be undone.                           |
 |                                                                              |
 |  Command:  mkfs.fat -F32 /dev/sdb1                                           |
 |                                                                              |
 |  Erase the card?      ( ) Yes     (*) No                                     |
 +------------------------------------------------------------------------------+
   y yes   n / Enter / Esc / q no
```

Invariants (tested, 6.4): the highlight starts on **No**; Enter, Esc, `q`, Ctrl-C and end of input mean No;
only `y` (or moving to Yes then Enter) means yes; the box is red in every colour profile above Ascii and
keeps an uppercase title in Ascii; the command is shown inside the box before the answer; the box shows the
same lines the plain `Danger` prints; no timeout, no auto-dismiss, no toast ever answers it; it takes over
the whole panel so it cannot be missed; `Danger` still refuses without a terminal and in `--plain` prints
today's text. The same box serves restore and flash writes. A normal (non-destructive) `AskYN` uses the same
overlay with an accent border and the same default No.

---

## 5. Architecture

### 5.1 One session program

New `tui.Session` (`internal/ui/tui/session.go`): a single `tea.Program` (alt screen, no mouse capture)
started by `cmdHome` / `cmdStepMenu` through a new optional interface in `cli`, next to `Menuer`:

```go
// Sessioner is implemented by the TUI: Session shows the screen and runs fn in a goroutine whose
// ui.UI renders inside it. It returns fn's error.
type Sessioner interface{ Session(fn func(ui.UI) error) error }
```

- `fn` is the home loop. The `ui.UI` handed to flows is a `sessionUI` whose methods `p.Send` messages and
  block on a reply channel for questions. Flows need **no change** to run inside the session; they gain
  features only through optional interfaces.
- `Say/Good/Warn/Err/Hint/Blank/Bold/Banner` append to a bounded transcript (2000 entries) that the strip
  and the details pane render; the text still goes to the log as today (`logf`).
- `Pick/Prompt/Secret/AskYN/Danger` become overlay models. `Secret` never echoes and never enters the
  transcript.
- `Progress/ProgressClose` update the step list.
- When the session ends, the tail of the transcript is printed once on the normal screen, so the last
  result survives in the scrollback.
- One-shot commands (`akam led`, `akam flash` ...) keep the **inline** widgets of today, restyled with the
  theme (T1/T3): no alt screen for a single command.

### 5.2 Optional rich interfaces (cli side)

Added in `internal/cli`; plain does not implement them and the code keeps its current branch (the pattern
`Menuer` already uses):

```go
type Former interface {      // in-place editor of a group of options (4.4)
    Form(title string, rows []ui.FormRow, apply func(id, value string) (ui.FormRow, error)) error
}
type Detailer interface{ Details(title, text string) }                       // the ? pane
type Checklister interface{ Checklist(title string, steps []string) error }  // 4.6
```

`flowOptions` uses `Former` when `a.UI` implements it, else the existing `groupScreen` (unchanged; plain
and its tests).

### 5.3 Theme and models

`theme.go` exposes `type Theme struct{ OK, Warn, Err, Accent, Muted lipgloss.Style; Glyphs Glyphs }` and
`NewTheme(env Env)` with an explicit `Env` (profile, NO_COLOR, WT_SESSION, locale) so tests never read the
process environment. Models stay plain structs with `Update`/`View`; `View` is pure and sized from
`tea.WindowSizeMsg`.

---

## 6. `--plain` and tests

### 6.1 Plain contract (frozen)

- `internal/ui/plain.go` behaviour is not modified. Unchanged: the strings `WARNING: `, `ERROR: `,
  `  What to do: `, `[y/N]`, `Number [1]: `, `WOULD ...`, `Step N/T - title`, the progress line
  `    | label: N%` every 10 %, exit codes 0/1/2, and the stdout/stderr split.
- `--plain`, `AKAM_PLAIN=1`, no terminal on stdin or stderr, `TERM=dumb`: `makeUI` returns plain.
  `NO_COLOR` and `--no-color` strip SGR in plain as today and keep attributes only in the TUI.
- Plain keeps `Pick` for the home and options menus with their "Back" / "Reload" / "Apply N" rows and the
  "N pending" semantics (scripts and `tests/assistant_test.sh` depend on them).
- Physical-action prompts B3/B4 stay line prompts in plain ("Press Enter when ... or type q"); scripts feed
  them. Only the TUI shows a checklist.
- Plain has no "Press Enter" at the home today (`Menuer` is not implemented): unchanged.

### 6.2 Test strategy

1. **Plain**: no change; existing tests are the guard. Every task states "`go test ./...` and
   `tests/assistant_test.sh` unchanged and green".
2. **Model unit tests** (as `internal/ui/tui/tui_test.go`): drive `Update` with `tea.KeyMsg` /
   `tea.WindowSizeMsg`, assert state and `View()` golden text. No pty, no terminal scraping.
3. **Golden screens** per screen and size (80x24, 60x16, 120x40). Goldens use the `Ascii` profile (as
   `flows_tui_test.go` forces with `termenv.Ascii`); a small set of `ANSI`/`TrueColor` tests asserts the
   raw SGR per **role** only (ok/warn/error/accent), so a palette edit is a conscious update
   (`-update-flows` flag exists).
4. **Flow tests** run a flow against `internal/testfake` through `sessionUI` driven by `Update` calls with
   scripted keys, asserting the final strip text and that no `Prompt` containing "Press Enter" was issued
   in the home, options, chooser, backup and update flows (a recording `ui.UI` counts prompts): the
   regression test for B1..B10.
5. **Theme tests**: table over `Env` (NO_COLOR, TERM=dumb, no WT_SESSION on Windows, COLORTERM=truecolor,
   non-UTF-8 locale) -> profile and glyph set.
6. **Width tests**: accented, CJK and emoji camera names keep columns aligned (`lipgloss.Width`).

### 6.3 Platforms

Manual matrix run before merge of T1, T4, T8 and T10: Windows 10 conhost (VT) and Windows Terminal; macOS
Terminal.app (256 colours) and iTerm2; Linux gnome-terminal, tmux, and the 16-colour linux console (ASCII
glyphs); `NO_COLOR=1`. `GOOS=windows|darwin|linux go vet ./...` passes (no build-tag specific code planned).

### 6.4 Safety invariants as tests

`TestDangerDefaultsNo` (Enter, Esc, q, Ctrl-C, end of input -> false, also as a session overlay);
`TestDangerRefusesWithoutTTY`; `TestNoNetworkBeforeScanYes` (chooser: zero dials before `y`);
`TestNeverReboots` (the existing allowlist test passes; no new command string is introduced);
`TestCommandShownBeforeRun` (the strip/transcript holds the `SET ...` or command line before the exchange,
asserted by a fake exchanger recording order); `TestAskYNDefaultNo` for every overlay.

---

## 7. Tasks

Two developers in parallel: **Dev A** owns `internal/ui/**`; **Dev B** owns `internal/cli/**`. The shared
contract is the interfaces of 5.2, frozen in T4 (A) and consumed by B. Ordered by user value.

### T1 - Theme, palette, restyle of existing widgets (Dev A) - colour first

Files: `internal/ui/tui/theme.go`, `theme_test.go` (new), `models.go`, `tui.go`, goldens.
- `NewTheme(Env)`, roles of 3.1, glyph sets of 3.3, rules of 3.2. Remove `SetHasDarkBackground(true)`;
  `CompleteColor` only.
- Restyle `YesNo`, `Input`, `Picker`, `Menu`: accent marker and bold selection, badges (`[ok]`, `[!]`,
  `[-]` plus the word) replacing the "done/todo/blocked" tags, red danger box, muted footers. Fix `pad` to
  use `lipgloss.Width`.
- Acceptance: colour per role under a colour profile; with `NO_COLOR` or Ascii the text equals the coloured
  one minus SGR; correct widths for non-ASCII; plain byte-identical; no new dependency.
- Tests: updated Ascii goldens, role-colour tests per profile, theme table test; `go test ./...` and
  `tests/assistant_test.sh` unchanged.

### T2 - Remove "Press Enter" on the home and step menus (Dev B) - no Enter, first

Files: `internal/cli/flows_home.go`, `misccmds.go` (the two loops only), `flows_home_test.go`,
`flows_tui_test.go`; additive `ui.MenuView{Title, Items, Result}` type in `internal/ui/ui.go` (one
line shared with A; `Menuer.Menu` keeps its signature, the result is passed through an optional
`ResultMenuer` interface).
- A decorator around `a.UI`, installed only when `Menuer` is present, records the last `Good/Warn/Err/Fail`
  and the flow error of the action.
- Delete both "Press Enter" prompts. The next home screen shows the strip (`[ok]/[!]/[x] text`) until the
  first key. The full text stays in the scrollback and the log.
- Acceptance: any home action in the TUI returns to the home screen with a one-line result and no prompt;
  failures show `[x]` and the message; plain path unchanged.
- Tests: recording-UI flow test asserting zero "Press Enter" prompts; goldens of the home with each level.
- Quick win; T4 later replaces the capture with the session transcript.

### T3 - Inline widgets polish (Dev A)

Files: `models.go`, `tui.go`, goldens.
- Picker: numbers shown only up to 9 items (1-9 still select), footer once and muted, type-to-filter over
  9 entries, trailing "Back"/"Cancel" rows hidden in the TUI (Esc does it; plain keeps them).
- Input: default as ghost text, visible cursor, Left/Right/Home/End editing (only backspace today).
- Acceptance: digit select, Esc cancel and default-No behaviour kept; plain strings untouched.
- Tests: model goldens and key sequences.

### T4 - Session architecture (Dev A) - the structural fix

Files: `internal/ui/tui/session.go`, `overlay.go`, `session_test.go` (new); `tui.go` (adds `Session`);
`internal/cli/misccmds.go` (the `Sessioner` interface, about 10 lines). Depends on T1.
- `Session(fn)` per 5.1: layout of 4.1, transcript, overlays for `Pick/Prompt/Secret/AskYN/Danger`,
  `Progress` step list, resize, guaranteed terminal restore.
- Acceptance: a flow using only `ui.UI` runs unchanged inside the session; a question blocks the flow
  goroutine until answered; Ctrl-C during a question answers No/cancel and the flow ends cleanly; quitting
  prints the last result once; no goroutine leaked; `Secret` input never appears in the transcript.
- Tests: scripted-key model tests with a fake flow doing `Say`, `Pick`, `AskYN`, `Progress`; goldens of the
  layout at three sizes.

### T5 - Home and chooser in the session (Dev B) - depends on T2, T4

Files: `flows_home.go`, `flows_camera.go` (+tests, goldens).
- `cmdHome` runs inside `Session`; header badges from `homeInfo`; the chooser becomes the persistent list
  of 4.3 with live row updates and `s/o/t/r/f` keys; the scan question stays an explicit yes/no (default
  No); the empty-state paragraph moves to details; trailing rows removed in the TUI path.
- Acceptance: nothing is dialled before `y`; rows update during probes; Esc stops a scan; plain chooser
  output byte-identical (`flows_camera_test.go` unchanged).
- Tests: `TestNoNetworkBeforeScanYes`; goldens (empty, 4 rows, scanning).

### T6 - Options as in-place forms (Dev B) - depends on T4 and the T7 widget

Files: `flows_options.go`, `camhealth.go` (badge accessors only), tests.
- `flowOptions` uses `Former` when present (4.4): group list + option pane, enum/number/text editing,
  debounced immediate apply with the `SET` line shown first, snap-back on failure, `[!] not saved`, restart
  on demand (`r`), clock/Wi-Fi as badges and `t` (4.5). `groupScreen`/`applyChanges` stay for plain.
- Acceptance: right-arrow x3 on Brightness sends exactly one `SET` after the debounce and shows one toast;
  a refused `SET` restores the shown value; entering options prints no paragraph; the restart question
  appears only on `r`; `flows_options_test.go` (plain) unchanged.
- Tests: fake exchanger recording order (command shown before exchange), goldens at three sizes, `NotSaved`
  and `Unavailable` rows.

### T7 - Form, Details, Checklist widgets (Dev A) - parallel with T6 against 5.2

Files: `internal/ui/tui/form.go`, `details.go`, `checklist.go` + tests; `internal/ui/ui.go` (additive
`FormRow`).
- Form: rows of kind enum/int/text/readonly, focus, left/right, Shift/page, typing, debounce as a
  `tea.Tick` message (deterministic in tests), per-row badge and error. Details: scrollable pane.
  Checklist: steps + one key.
- Acceptance: goldens at three sizes; the package holds no camera or network knowledge.

### T8 - Long operations, spinners, results (A for the panel, B for the flows) - depends on T4, T7

Files (A): `internal/ui/tui/steps.go`; (B): `flows_backup.go`, `flows_update.go`, `flashcmds.go` and
`telnetcmds.go` (call sites only: optional progress detail; the two physical `Prompt`s use `Checklister`
when present, else the same line prompt as today).
- Step list with spinner, bar, optional speed/size, running command line (4.6), collapse into the result
  line; B3/B4 become the checklist.
- Acceptance: no "Press Enter" text prompt remains in the TUI; plain prompts and the progress lines
  (every 10 %) unchanged; Ctrl-C leaves the camera untouched and says so.
- Tests: fake-camera backup through the session harness, goldens mid-run and after; plain goldens unchanged.

### T9 - Error screen and text diet (Dev B) - depends on T4, T7

Files: `internal/cli/*` (string sources only for the TUI path).
- `Fail` renders as 4.7 with `r`/`l` keys (flows register a retry through an optional `Retrier`); long
  paragraphs listed in 1.3 move to `Detailer`; shorten `homeScreen` details and `optionLine` for the TUI
  only (selected through `a.UI.(Menuer)`); plain keeps the current strings.
- Acceptance: no list line over 60 columns in the TUI goldens; plain goldens (`golden_test.go`, testdata)
  byte-identical.

### T10 - Safety regression net and platform matrix (A + B) - last

Files: `internal/ui/tui/*_test.go`, `internal/cli/safety_test.go` (additions).
- The tests of 6.4, plus "no Press Enter prompt reaches `ui.UI` in any TUI flow" over home, chooser,
  options, backup and update with the fake camera.
- The manual matrix of 6.3 recorded in the task report.
- Acceptance: all pass; `go vet` for the three OSes; binary size delta reported (budget: under 100 KB,
  no new dependency).

Parallelism: A: T1 -> T3 -> T4 -> T7 -> panel of T8 -> T10 part. B: T2 -> (after T4) T5 -> T6 -> flows of T8
-> T9 -> T10 part. T1 and T2 land first and give both visible wins (colour, no Enter on the home) as two
small merges.

---

## 8. Risks and open questions

| # | Risk / question | Default answer |
|---|---|---|
| R1 | A long-lived alt-screen session hides output the user wants to copy (an address, a path) | the transcript tail is printed on exit; `l` shows the log path; no mouse capture so terminal selection works |
| R2 | A flow goroutine blocked on a reply while the program quits (deadlock) | every question also selects on a `done` channel; quit answers pending questions No; goroutine-count test in T4 |
| R3 | Immediate apply sends more `SET`s than before | 400 ms debounce, one `Apply` per settled value, reversible options only; plain keeps pending/apply |
| R4 | Colour differs per terminal; contrast on light backgrounds | mid-tone palette, no background query, roles always paired with a glyph or word |
| R5 | Legacy Windows console lacks glyphs or VT | ASCII glyph set auto-selected; VT failure falls back to plain |
| R6 | Golden churn | goldens in Ascii; colour asserted per role only |
| Q1 | Keep number shortcuts 1-8 on the home? | yes, hidden, plus `c` |
| Q2 | Auto-detect the power cycle in "verify without the card" to drop the last key press? | not in V1: one key press (Space/Enter) for physical actions; revisit after the checklist exists |
| Q3 | Ship a light/dark theme switch? | no; one mid-tone palette; `NO_COLOR` honoured |
| Q4 | Add `bubbles`? | no; spinner, bar and list are about 150 lines by hand; offline module cache and size budget |
| Q5 | Keep the clock question as an unprompted yes/no once per session? | no: badge plus `t`; the guards (default No, never when NTP-synced or PC clock implausible) are unchanged |
