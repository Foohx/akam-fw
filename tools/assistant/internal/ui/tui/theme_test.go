package tui

import (
	"regexp"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"
)

var sgr = regexp.MustCompile("\x1b\\[[0-9;]*m")

func stripSGR(s string) string { return sgr.ReplaceAllString(s, "") }

func envOf(p termenv.Profile) Env {
	return Env{Profile: p, GOOS: "linux", Locale: "en_US.UTF-8"}
}

// Capability decision order of 3.2: --plain or no terminal, TERM=dumb, NO_COLOR, then the profile.
func TestDecide(t *testing.T) {
	cases := []struct {
		name  string
		e     Env
		plain bool
		tty   bool
		want  Mode
	}{
		{"colour terminal", envOf(termenv.TrueColor), false, true, ModeColor},
		{"plain flag", envOf(termenv.TrueColor), true, true, ModePlain},
		{"pipe", envOf(termenv.TrueColor), false, false, ModePlain},
		{"dumb", Env{Dumb: true, Profile: termenv.ANSI}, false, true, ModePlain},
		{"dumb beats NO_COLOR", Env{Dumb: true, NoColor: true}, false, true, ModePlain},
		{"NO_COLOR", Env{NoColor: true, Profile: termenv.TrueColor}, false, true, ModeNoColor},
		{"plain beats NO_COLOR", Env{NoColor: true}, true, true, ModePlain},
		{"ascii profile", envOf(termenv.Ascii), false, true, ModeColor},
	}
	for _, c := range cases {
		if got := Decide(c.e, c.plain, c.tty); got != c.want {
			t.Errorf("%s: mode %d, want %d", c.name, got, c.want)
		}
	}
}

// Glyph set: AKAM_ASCII, non-UTF-8 locale on Unix, legacy conhost on Windows.
func TestUseASCII(t *testing.T) {
	cases := []struct {
		name string
		e    Env
		want bool
	}{
		{"unix utf-8", Env{GOOS: "linux", Locale: "en_US.UTF-8"}, false},
		{"unix utf8 lower", Env{GOOS: "darwin", Locale: "fr_FR.utf8"}, false},
		{"unix C locale", Env{GOOS: "linux", Locale: "C"}, true},
		{"unix no locale", Env{GOOS: "linux"}, true},
		{"AKAM_ASCII", Env{GOOS: "linux", Locale: "en_US.UTF-8", ASCII: true}, true},
		{"windows conhost", Env{GOOS: "windows"}, true},
		{"windows terminal", Env{GOOS: "windows", WTSession: "x"}, false},
		{"windows conemu", Env{GOOS: "windows", ConEmuANSI: "ON"}, false},
		{"windows term program", Env{GOOS: "windows", TermProgram: "vscode"}, false},
		{"dumb", Env{GOOS: "linux", Locale: "en_US.UTF-8", Dumb: true}, true},
	}
	for _, c := range cases {
		if got := UseASCII(c.e); got != c.want {
			t.Errorf("%s: ascii=%v, want %v", c.name, got, c.want)
		}
	}
	if g := NewTheme(Env{GOOS: "linux", Locale: "C"}).Glyphs; g.Marker != ">" || g.OK != "[ok]" {
		t.Errorf("ascii set: %+v", g)
	}
	if g := NewTheme(envOf(termenv.ANSI)).Glyphs; g.Marker != "›" || g.OK != "✓" {
		t.Errorf("unicode set: %+v", g)
	}
}

// Each role carries its own colour code under each colour profile (a palette edit updates this table
// on purpose).
func TestRoleColours(t *testing.T) {
	type rc struct{ ok, warn, err, accent, info string }
	cases := []struct {
		p    termenv.Profile
		want rc
	}{
		{termenv.TrueColor, rc{"38;2;47;168;79", "38;2;217;163;0", "38;2;214;64;58", "38;2;46;139;192", "38;2;46;139;192"}},
		{termenv.ANSI256, rc{"38;5;35", "38;5;178", "38;5;160", "38;5;33", "38;5;33"}},
		{termenv.ANSI, rc{"[32m", "[33m", "[31m", "[36m", "[34m"}},
	}
	for _, c := range cases {
		th := NewTheme(envOf(c.p))
		for name, pair := range map[string][2]string{
			"ok": {th.OK.Render("x"), c.want.ok}, "warn": {th.Warn.Render("x"), c.want.warn},
			"err": {th.Err.Render("x"), c.want.err}, "accent": {th.Accent.Render("x"), c.want.accent},
			"info": {th.Info.Render("x"), c.want.info},
		} {
			if !strings.Contains(pair[0], pair[1]) {
				t.Errorf("profile %v role %s: %q lacks %q", c.p, name, pair[0], pair[1])
			}
		}
	}
	// muted: a grey on 256 and truecolor, the faint attribute alone on 16 colours
	if s := NewTheme(envOf(termenv.TrueColor)).Muted.Render("x"); !strings.Contains(s, "38;2;142;147;155") {
		t.Errorf("muted truecolor: %q", s)
	}
	if s := NewTheme(envOf(termenv.ANSI256)).Muted.Render("x"); !strings.Contains(s, "38;5;244") {
		t.Errorf("muted 256: %q", s)
	}
	s := NewTheme(envOf(termenv.ANSI)).Muted.Render("x")
	if !strings.Contains(s, "\x1b[2m") || strings.Contains(s, "\x1b[3") || strings.Contains(s, "\x1b[9") {
		t.Errorf("muted 16 colours must be faint on the default colour: %q", s)
	}
	// the selected row is accent and bold
	if s := NewTheme(envOf(termenv.ANSI)).Sel.Render("x"); !strings.Contains(s, "36") || !strings.Contains(s, "1") {
		t.Errorf("selection: %q", s)
	}
}

var colourCode = regexp.MustCompile("\x1b\\[(3|4|9)[0-9;]*m")

// The Ascii profile emits no escape at all; NO_COLOR keeps bold and faint but no colour.
func TestNoColourProfiles(t *testing.T) {
	a := NewTheme(envOf(termenv.Ascii))
	for _, s := range []string{a.OK.Render("x"), a.Err.Render("x"), a.Muted.Render("x"), a.Sel.Render("x"), a.Badge(BadgeWarn, "stale")} {
		if strings.ContainsRune(s, '\x1b') {
			t.Errorf("escape in the Ascii profile: %q", s)
		}
	}
	n := NewTheme(Env{Profile: termenv.ANSI, NoColor: true, GOOS: "linux", Locale: "C"})
	if s := n.Sel.Render("x"); !strings.Contains(s, "\x1b[1m") || colourCode.MatchString(s) {
		t.Errorf("NO_COLOR selection keeps bold, no colour: %q", s)
	}
	if s := n.Muted.Render("x"); !strings.Contains(s, "\x1b[2m") {
		t.Errorf("NO_COLOR muted keeps faint: %q", s)
	}
	for _, s := range []string{n.OK.Render("x"), n.Warn.Render("x"), n.Err.Render("x"), n.Accent.Render("x")} {
		if strings.Contains(s, "\x1b[") {
			t.Errorf("NO_COLOR role has an attribute: %q", s)
		}
	}
	// every view under NO_COLOR carries attributes only, never a colour code
	for i, v := range views(n) {
		if m := colourCode.FindString(v); m != "" {
			t.Errorf("view %d: colour code %q under NO_COLOR", i, m)
		}
	}
}

func views(th *Theme) []string {
	menu := NewMenu("Akam\nCamera: salon", menuItems())
	menu.Theme = th
	menu.Update(tea.WindowSizeMsg{Width: 80, Height: 24})
	d := &YesNo{Prompt: "Write?", Title: "Danger", Danger: true, Box: []string{"x"}, Theme: th}
	return []string{
		menu.View(), d.View(),
		(&YesNo{Prompt: "Go?", Theme: th}).View(),
		(&Picker{Title: "t", Items: []string{"a", "b"}, Theme: th}).View(),
		(&Input{Prompt: "n:", Theme: th}).View(),
	}
}

// With NO_COLOR or the Ascii profile, the text equals the coloured text minus its SGR.
func TestTextEqualsColouredMinusSGR(t *testing.T) {
	ref := views(NewTheme(Env{Profile: termenv.TrueColor, ASCII: true}))
	for _, env := range []Env{
		{Profile: termenv.Ascii, ASCII: true},
		{Profile: termenv.ANSI, NoColor: true, ASCII: true},
		{Profile: termenv.ANSI, ASCII: true},
		{Profile: termenv.ANSI256, ASCII: true},
	} {
		got := views(NewTheme(env))
		for i := range ref {
			if stripSGR(ref[i]) != stripSGR(got[i]) {
				t.Errorf("view %d differs for %+v", i, env)
			}
		}
	}
	for i, v := range views(NewTheme(Env{Profile: termenv.Ascii, ASCII: true})) {
		if strings.ContainsRune(v, '\x1b') {
			t.Errorf("view %d: escape in the Ascii profile", i)
		}
	}
}

// The danger box is red in every profile above Ascii, and keeps an uppercase title in Ascii.
func TestDangerBoxIsRed(t *testing.T) {
	for p, want := range map[termenv.Profile]string{termenv.TrueColor: "38;2;214;64;58", termenv.ANSI256: "38;5;160", termenv.ANSI: "[31m"} {
		d := &YesNo{Prompt: "Go?", Title: "Erase it", Danger: true, Box: []string{"x"}, Theme: NewTheme(envOf(p))}
		if v := d.View(); !strings.Contains(v, want) {
			t.Errorf("profile %v: no red border in %q", p, v)
		}
	}
	d := &YesNo{Prompt: "Go?", Title: "Erase it", Danger: true, Theme: NewTheme(envOf(termenv.Ascii))}
	if v := d.View(); !strings.Contains(v, "ERASE IT") || strings.ContainsRune(v, '\x1b') {
		t.Errorf("ascii danger: %q", v)
	}
}

func TestBadges(t *testing.T) {
	a := NewTheme(Env{Profile: termenv.Ascii, ASCII: true})
	for k, want := range map[BadgeKind]string{BadgeOK: "[ok] ready", BadgeWarn: "[!] ready", BadgeErr: "[x] ready", BadgeInfo: "[~] ready", BadgeMute: "[-] ready"} {
		if got := a.Badge(k, "ready"); got != want {
			t.Errorf("badge %d: %q want %q", k, got, want)
		}
	}
	u := NewTheme(envOf(termenv.Ascii))
	if got := u.Badge(BadgeOK, "ready") + "|" + u.Badge(BadgeInfo, "x"); got != "✓ ready|⠼ x" {
		t.Errorf("unicode badges: %q", got)
	}
	if w := lipgloss.Width(a.Badge(BadgeOK, "a very long word indeed")); w != 4+1+12 {
		t.Errorf("badge word is not capped at 12: width %d", w)
	}
	// badge colours: ok green, warn yellow, err red, info blue
	c := NewTheme(envOf(termenv.ANSI))
	for k, code := range map[BadgeKind]string{BadgeOK: "32", BadgeWarn: "33", BadgeErr: "31", BadgeInfo: "34"} {
		if !strings.Contains(c.Badge(k, "w"), "\x1b["+code+"m") {
			t.Errorf("badge %d lacks colour %s", k, code)
		}
	}
}

// Non-ASCII names keep their columns: padding and boxes are measured in cells, not bytes.
func TestWidthsOfNonASCII(t *testing.T) {
	for _, s := range []string{"salon", "café", "séjour", "カメラ", "cam 🎥", "été"} {
		if got := lipgloss.Width(pad(s, 20)); got != 20 {
			t.Errorf("pad(%q, 20) is %d columns", s, got)
		}
		if got := lipgloss.Width(truncW(s+"xxxxxxxxxxxxxxxxxxxxxx", 10, "...")); got > 10 {
			t.Errorf("truncW(%q) is %d columns", s, got)
		}
		for _, l := range wrapW(strings.Repeat(s+" ", 8), 12) {
			if lipgloss.Width(l) > 12 {
				t.Errorf("wrapW(%q): %q is %d columns", s, l, lipgloss.Width(l))
			}
		}
	}
	th := NewTheme(Env{Profile: termenv.Ascii, ASCII: true})
	box := th.Box(th.Err, "TITLE", []string{"café salon", "カメラ  192.0.2.41", "cam 🎥"}, 0)
	var w int
	for i, l := range strings.Split(strings.TrimRight(box, "\n"), "\n") {
		if i == 0 {
			w = lipgloss.Width(l)
		} else if lipgloss.Width(l) != w {
			t.Errorf("box line %d is %d columns, top is %d: %q", i, lipgloss.Width(l), w, l)
		}
	}
	// a menu with such labels never exceeds the window
	items := menuItems()
	items[1].Label = "2. カメラ salon café 🎥 " + strings.Repeat("é", 80)
	m := NewMenu("t", items)
	m.Update(tea.WindowSizeMsg{Width: 60, Height: 16})
	for _, l := range strings.Split(m.View(), "\n") {
		if lipgloss.Width(l) > 60 {
			t.Errorf("menu line of %d columns: %q", lipgloss.Width(l), l)
		}
	}
}

func TestProgressAndSpinner(t *testing.T) {
	th := NewTheme(Env{Profile: termenv.Ascii, ASCII: true})
	if got := th.ProgressBar(50, 10); got != "#####----- 50 %" {
		t.Errorf("bar: %q", got)
	}
	if got := th.ProgressBar(150, 4); got != "#### 100 %" {
		t.Errorf("bar over 100: %q", got)
	}
	if th.Spinner(0) != "|" || th.Spinner(5) != "/" || th.Spinner(-1) != "\\" {
		t.Error("spinner frames")
	}
	c := NewTheme(envOf(termenv.ANSI))
	if s := c.ProgressBar(30, 10); !strings.Contains(s, "\x1b[34m") {
		t.Errorf("the bar is in the info colour: %q", s)
	}
}

// Building a theme never touches the global lipgloss profile.
func TestThemeIsPrivate(t *testing.T) {
	before := lipgloss.ColorProfile()
	_ = NewTheme(envOf(termenv.TrueColor))
	if lipgloss.ColorProfile() != before {
		t.Error("NewTheme changed the global colour profile")
	}
}
