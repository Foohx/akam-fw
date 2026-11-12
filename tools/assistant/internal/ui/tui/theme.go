package tui

import (
	"io"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"

	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"
	"golang.org/x/term"
)

// This file is the only place that names a colour or a glyph (spec 12, section 3). Widgets ask the
// Theme for a role (OK, Warn, Err, Accent, Info, Muted) and never hold a literal colour, so a palette
// edit is one edit here, and a colour is never the only signal: every state also has a glyph or a word.

// Palette: the semantic roles with their truecolor, ANSI256 and ANSI16 values.
var (
	colOK     = lipgloss.CompleteColor{TrueColor: "#2FA84F", ANSI256: "35", ANSI: "2"}
	colWarn   = lipgloss.CompleteColor{TrueColor: "#D9A400", ANSI256: "178", ANSI: "3"}
	colErr    = lipgloss.CompleteColor{TrueColor: "#D6403A", ANSI256: "160", ANSI: "1"}
	colAccent = lipgloss.CompleteColor{TrueColor: "#2E8BC0", ANSI256: "33", ANSI: "6"}
	colInfo   = lipgloss.CompleteColor{TrueColor: "#2E8BC0", ANSI256: "33", ANSI: "4"}
	// Muted is a mid grey; on the 16 colour palette it is the faint attribute on the default colour
	// (bright black is invisible on some palettes), see NewTheme.
	colMuted = lipgloss.CompleteColor{TrueColor: "#8E939B", ANSI256: "244", ANSI: ""}
)

// Env is everything the theme decides from. Tests build it by hand and never read the process
// environment; CurrentEnv fills it from the real one.
type Env struct {
	Profile     termenv.Profile // colour support of the output stream
	NoColor     bool            // NO_COLOR set or --no-color: no colour, attributes kept
	Dumb        bool            // TERM=dumb
	ASCII       bool            // AKAM_ASCII=1
	Locale      string          // first non-empty of LC_ALL, LC_CTYPE, LANG
	GOOS        string
	WTSession   string // WT_SESSION (Windows Terminal)
	ConEmuANSI  string
	TermProgram string // TERM_PROGRAM
}

// CurrentEnv reads the process environment; out is the stream the interface draws on (stderr).
func CurrentEnv(out *os.File) Env {
	e := Env{
		NoColor:     os.Getenv("NO_COLOR") != "",
		Dumb:        os.Getenv("TERM") == "dumb",
		ASCII:       os.Getenv("AKAM_ASCII") == "1",
		GOOS:        runtime.GOOS,
		WTSession:   os.Getenv("WT_SESSION"),
		ConEmuANSI:  os.Getenv("ConEmuANSI"),
		TermProgram: os.Getenv("TERM_PROGRAM"),
	}
	for _, k := range []string{"LC_ALL", "LC_CTYPE", "LANG"} {
		if v := os.Getenv(k); v != "" {
			e.Locale = v
			break
		}
	}
	e.Profile = termenv.Ascii
	if out != nil {
		e.Profile = termenv.NewOutput(out).ColorProfile()
		if e.NoColor && e.Profile == termenv.Ascii && term.IsTerminal(int(out.Fd())) {
			// termenv turns NO_COLOR into "no escape at all"; the spec keeps bold and faint.
			e.Profile = termenv.ANSI
		}
	}
	return e
}

// Mode is the capability decision of spec 12, section 3.2.
type Mode int

const (
	ModePlain   Mode = iota // line based interface, today's behaviour
	ModeNoColor             // full-screen, no colour, attributes and markers kept
	ModeColor               // full-screen with the colour of the profile (Ascii profile = no colour too)
)

// Decide applies the order, first match wins: --plain or a non-terminal, then TERM=dumb, then
// NO_COLOR (or --no-color), then the colour profile.
func Decide(e Env, plainFlag, isTTY bool) Mode {
	switch {
	case plainFlag || !isTTY:
		return ModePlain
	case e.Dumb:
		return ModePlain
	case e.NoColor:
		return ModeNoColor
	}
	return ModeColor
}

// UseASCII tells whether the ASCII glyph set is used: AKAM_ASCII=1; a locale that is not UTF-8 on
// Unix; on Windows when neither WT_SESSION, ConEmuANSI nor TERM_PROGRAM is set (legacy conhost).
func UseASCII(e Env) bool {
	if e.ASCII || e.Dumb {
		return true
	}
	if e.GOOS == "windows" {
		return e.WTSession == "" && e.ConEmuANSI == "" && e.TermProgram == ""
	}
	l := strings.ToLower(e.Locale)
	return !strings.Contains(l, "utf-8") && !strings.Contains(l, "utf8")
}

// BoxGlyphs are the characters of a box border.
type BoxGlyphs struct{ H, V, TL, TR, BL, BR string }

// Glyphs is one of the two glyph sets.
type Glyphs struct {
	Marker   string // selected row
	OK       string // badge glyphs
	Warn     string
	Err      string
	Info     string
	Mute     string
	Spinner  []string
	Bullet   string
	Rule     string // separator line
	Ellipsis string
	UpDown   string
	RadioOn  string
	RadioOff string
	BarFull  string
	BarEmpty string
	Box      BoxGlyphs
}

var unicodeGlyphs = Glyphs{
	Marker: "›", OK: "✓", Warn: "!", Err: "x", Info: "⠼", Mute: "-",
	Spinner: []string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"},
	Bullet:  "•", Rule: "─", Ellipsis: "…", UpDown: "↑/↓", RadioOn: "(•)", RadioOff: "( )",
	BarFull: "█", BarEmpty: "░",
	Box: BoxGlyphs{H: "─", V: "│", TL: "╭", TR: "╮", BL: "╰", BR: "╯"},
}

var asciiGlyphs = Glyphs{
	Marker: ">", OK: "[ok]", Warn: "[!]", Err: "[x]", Info: "[~]", Mute: "[-]",
	Spinner: []string{"|", "/", "-", "\\"},
	Bullet:  "*", Rule: "-", Ellipsis: "...", UpDown: "up/down", RadioOn: "(*)", RadioOff: "( )",
	BarFull: "#", BarEmpty: "-",
	Box: BoxGlyphs{H: "-", V: "|", TL: "+", TR: "+", BL: "+", BR: "+"},
}

// Theme is the set of styles and glyphs every widget draws with.
type Theme struct {
	OK, Warn, Err, Accent, Info, Muted lipgloss.Style
	Bold                               lipgloss.Style
	Sel                                lipgloss.Style // selected row: accent + bold
	Rev                                lipgloss.Style // the text cursor: reverse video
	Glyphs                             Glyphs
	Mode                               Mode
	Profile                            termenv.Profile
}

// NewTheme builds the theme of an environment. The styles come from a private renderer fixed to the
// profile, so the theme never depends on (or changes) the global lipgloss state.
func NewTheme(e Env) *Theme {
	t := &Theme{Mode: Decide(e, false, true), Profile: e.Profile}
	t.Glyphs = unicodeGlyphs
	if UseASCII(e) {
		t.Glyphs = asciiGlyphs
	}
	r := lipgloss.NewRenderer(io.Discard)
	prof := e.Profile
	noColour := e.NoColor || e.Dumb
	if noColour && prof != termenv.Ascii {
		prof = termenv.ANSI // attributes only: bold and faint stay, no foreground is ever set
	}
	r.SetColorProfile(prof)
	fg := func(c lipgloss.CompleteColor) lipgloss.Style {
		s := r.NewStyle()
		if noColour {
			return s
		}
		return s.Foreground(c)
	}
	t.OK, t.Warn, t.Err = fg(colOK), fg(colWarn), fg(colErr)
	t.Accent, t.Info = fg(colAccent), fg(colInfo)
	t.Muted = r.NewStyle().Faint(true)
	if !noColour && (prof == termenv.TrueColor || prof == termenv.ANSI256) {
		t.Muted = t.Muted.Foreground(colMuted)
	}
	t.Bold = r.NewStyle().Bold(true)
	t.Sel = t.Accent.Bold(true)
	t.Rev = r.NewStyle().Reverse(true)
	return t
}

var (
	themeMu      sync.Mutex
	defaultTheme *Theme
)

// SetTheme replaces the theme the widgets use when they have none of their own (tui.New sets it from
// the real environment; tests set a fixed one).
func SetTheme(t *Theme) {
	themeMu.Lock()
	defaultTheme = t
	themeMu.Unlock()
}

func theme() *Theme {
	themeMu.Lock()
	defer themeMu.Unlock()
	if defaultTheme == nil {
		defaultTheme = NewTheme(CurrentEnv(os.Stderr))
	}
	return defaultTheme
}

// BadgeKind selects the colour and glyph of a badge.
type BadgeKind int

const (
	BadgeOK BadgeKind = iota
	BadgeWarn
	BadgeErr
	BadgeInfo
	BadgeMute
)

// maxBadgeWord is the longest word of a badge (spec 3.4).
const maxBadgeWord = 12

// Badge is a coloured glyph plus one word, the word at most 12 columns.
func (t *Theme) Badge(k BadgeKind, word string) string {
	st, g := t.OK, t.Glyphs.OK
	switch k {
	case BadgeWarn:
		st, g = t.Warn, t.Glyphs.Warn
	case BadgeErr:
		st, g = t.Err, t.Glyphs.Err
	case BadgeInfo:
		st, g = t.Info, t.Glyphs.Info
	case BadgeMute:
		st, g = t.Muted, t.Glyphs.Mute
	}
	word = truncW(word, maxBadgeWord, t.Glyphs.Ellipsis)
	if word == "" {
		return st.Render(g)
	}
	return st.Render(g) + " " + word
}

// BadgeWidth is the widest badge of this glyph set for a word of n columns.
func (t *Theme) BadgeWidth(n int) int {
	w := 0
	for _, g := range []string{t.Glyphs.OK, t.Glyphs.Warn, t.Glyphs.Err, t.Glyphs.Info, t.Glyphs.Mute} {
		w = max(w, lipgloss.Width(g))
	}
	return w + 1 + min(n, maxBadgeWord)
}

// Footer renders key hints: pairs of (key, word), keys in accent and words muted.
func (t *Theme) Footer(pairs ...string) string {
	var parts []string
	for i := 0; i+1 < len(pairs); i += 2 {
		parts = append(parts, t.Accent.Render(pairs[i])+" "+t.Muted.Render(pairs[i+1]))
	}
	return strings.Join(parts, "   ")
}

// CursorAt draws the text rs with the cursor before rune pos: reverse video on the character under
// it (a space at the end). The Ascii profile (no terminal) emits no attribute, so no cursor shows.
func (t *Theme) CursorAt(rs []rune, pos int) string {
	pos = min(max(pos, 0), len(rs))
	under, rest := " ", ""
	if pos < len(rs) {
		under, rest = string(rs[pos]), string(rs[pos+1:])
	}
	return string(rs[:pos]) + t.Rev.Render(under) + rest
}

// Rule is a muted separator line of n columns.
func (t *Theme) Rule(n int) string {
	return t.Muted.Render(strings.Repeat(t.Glyphs.Rule, max(n, 0)))
}

// Spinner is the frame i of the spinner, in the info colour.
func (t *Theme) Spinner(i int) string {
	f := t.Glyphs.Spinner
	return t.Info.Render(f[((i%len(f))+len(f))%len(f)])
}

// ProgressBar draws "[####------] 42 %" with a bar of width columns (at least 4).
func (t *Theme) ProgressBar(pct, width int) string {
	pct = min(max(pct, 0), 100)
	width = max(width, 4)
	full := width * pct / 100
	bar := t.Info.Render(strings.Repeat(t.Glyphs.BarFull, full)) + t.Muted.Render(strings.Repeat(t.Glyphs.BarEmpty, width-full))
	return bar + " " + strconv.Itoa(pct) + " %"
}

// Box draws a bordered box of outerW columns (0 = as wide as the content, at least wide enough for
// the title). The border and the title use the style bs; lines are drawn as they come (they may
// carry their own styling) and padded with lipgloss.Width. The caller wraps lines to outerW-4.
func (t *Theme) Box(bs lipgloss.Style, title string, lines []string, outerW int) string {
	g := t.Glyphs.Box
	tw := lipgloss.Width(title)
	need := tw + 7
	for _, l := range lines {
		need = max(need, lipgloss.Width(l)+4)
	}
	if outerW < need {
		outerW = need
	}
	inner := outerW - 4
	var b strings.Builder
	b.WriteString(bs.Render(g.TL+g.H+" ") + bs.Bold(true).Render(title) + bs.Render(" "+strings.Repeat(g.H, outerW-2-3-tw)+g.TR) + "\n")
	for _, l := range lines {
		b.WriteString(bs.Render(g.V) + " " + pad(l, inner) + " " + bs.Render(g.V) + "\n")
	}
	b.WriteString(bs.Render(g.BL+strings.Repeat(g.H, outerW-2)+g.BR) + "\n")
	return b.String()
}

// pad right-pads s with spaces to n columns, measured with lipgloss.Width (not len: accented, CJK
// and emoji text is not one byte per column).
func pad(s string, n int) string {
	if w := lipgloss.Width(s); w < n {
		return s + strings.Repeat(" ", n-w)
	}
	return s
}

// truncW cuts s to at most n columns, ending with ell when it was cut.
func truncW(s string, n int, ell string) string {
	if lipgloss.Width(s) <= n {
		return s
	}
	n -= lipgloss.Width(ell)
	if n < 0 {
		n = 0
	}
	var b strings.Builder
	w := 0
	for _, r := range s {
		rw := lipgloss.Width(string(r))
		if w+rw > n {
			break
		}
		b.WriteRune(r)
		w += rw
	}
	return b.String() + ell
}

// wrapW breaks s into lines of at most n columns (words, then hard cuts for a longer word).
func wrapW(s string, n int) []string {
	if n < 1 || lipgloss.Width(s) <= n {
		return []string{s}
	}
	var out []string
	cur := ""
	flush := func() {
		if cur != "" {
			out = append(out, cur)
			cur = ""
		}
	}
	for _, w := range strings.Fields(s) {
		for lipgloss.Width(w) > n {
			flush()
			head := truncW(w, n, "")
			if head == "" {
				head = w[:1]
			}
			out = append(out, head)
			w = w[len(head):]
		}
		switch {
		case cur == "":
			cur = w
		case lipgloss.Width(cur)+1+lipgloss.Width(w) <= n:
			cur += " " + w
		default:
			flush()
			cur = w
		}
	}
	flush()
	if len(out) == 0 {
		return []string{""}
	}
	return out
}
