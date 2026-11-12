package tui

import (
	"fmt"
	"regexp"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The session (spec 12, section 5.1) is one long-lived full-screen program: a header with the camera
// and its health badges, a main panel, a result strip and a footer with the keys of the current screen.
// The flows keep their code written against ui.UI and run in a goroutine; the sessionUI of
// sessionui.go turns Say/Good/Warn/Err into entries of the transcript and the strip, and
// Pick/Prompt/Secret/AskYN/Danger into overlays the goroutine blocks on. Nothing switches program, so
// there is no output to hide and nothing to acknowledge.

// maxTranscript bounds the transcript (the oldest entries are dropped).
const maxTranscript = 2000

type lineKind int

const (
	kSay lineKind = iota
	kBold
	kGood
	kWarn
	kErr
	kHint
	kBlank
)

// entry is one line of the transcript.
type entry struct {
	kind lineKind
	text string
}

// Messages sent to the program by the flow goroutine.
type (
	msgLine     struct{ e entry }
	msgBanner   struct{ title string }
	msgProgress struct {
		label string
		pct   int
	}
	msgProgressClose struct{}
	msgAsk           struct{ q *ask }
	msgHeader        struct{ h ui.Header }
	msgStripIfEmpty  struct{ e entry }
	msgFlowDone      struct{}
	msgTick          struct{}
)

const tickEvery = 100 * time.Millisecond

// sessState is what the program and the flow goroutine share.
type sessState struct {
	done       chan struct{} // closed when the program has ended
	cancel     chan struct{} // closed by the first Ctrl-C on a running flow: stop asking, finish
	cancelOnce sync.Once
	send       func(tea.Msg)
}

func newSessState() *sessState {
	return &sessState{done: make(chan struct{}), cancel: make(chan struct{}), send: func(tea.Msg) {}}
}

func (s *sessState) stop() { s.cancelOnce.Do(func() { close(s.cancel) }) }

func (s *sessState) stopped() bool {
	select {
	case <-s.cancel:
		return true
	default:
		return false
	}
}

type step struct {
	title string
	done  bool
}

type progress struct {
	label string
	pct   int
}

var stepRe = regexp.MustCompile(`^Step (\d+)/(\d+) - (.+)$`)

// sessModel is the tea.Model of the session. All its fields belong to the program goroutine.
type sessModel struct {
	th *Theme
	st *sessState

	w, h int
	hdr  ui.Header

	transcript []entry
	strip      *entry

	title string // the last banner that is not a step
	steps []step
	total int
	prog  *progress

	queue    []*ask // the first one is on screen
	frame    int
	ticking  bool
	finished bool // the flow returned
	stopping bool // the user pressed Ctrl-C once
}

func newSessModel(th *Theme, st *sessState) *sessModel {
	return &sessModel{th: th, st: st, w: 80, h: 24}
}

func (m *sessModel) Init() tea.Cmd { return nil }

func (m *sessModel) panelRows() int {
	if m.compact() {
		return max(m.h-3, 1)
	}
	return max(m.h-6, 1)
}

// compact is the single column fallback of spec 4.1: under 60 columns or 14 lines.
func (m *sessModel) compact() bool { return m.w < 60 || m.h < 14 }

func (m *sessModel) current() *ask {
	if len(m.queue) == 0 {
		return nil
	}
	return m.queue[0]
}

func (m *sessModel) busy() bool {
	if m.finished || m.current() != nil {
		return false
	}
	if m.prog != nil {
		return true
	}
	for _, s := range m.steps {
		if !s.done {
			return true
		}
	}
	return false
}

// startTick arms the spinner timer once; it re-arms itself while something is in flight.
func (m *sessModel) startTick() tea.Cmd {
	if m.ticking {
		return nil
	}
	m.ticking = true
	return tea.Tick(tickEvery, func(time.Time) tea.Msg { return msgTick{} })
}

func (m *sessModel) addLine(e entry) {
	e.text = strings.ReplaceAll(e.text, "\t", "    ")
	m.transcript = append(m.transcript, e)
	if n := len(m.transcript); n > maxTranscript {
		m.transcript = append([]entry(nil), m.transcript[n-maxTranscript:]...)
	}
	switch e.kind {
	case kGood, kWarn:
		if m.strip == nil || m.strip.kind != kErr { // an error stays over a later success
			m.strip = &entry{e.kind, oneLine(e.text)}
		}
	case kErr:
		m.strip = &entry{e.kind, oneLine(e.text)}
	case kSay:
		if m.strip == nil || m.strip.kind == kSay {
			m.strip = &entry{kSay, oneLine(e.text)}
		}
	}
}

func oneLine(s string) string { return strings.Join(strings.Fields(s), " ") }

func (m *sessModel) banner(t string) {
	if mm := stepRe.FindStringSubmatch(t); mm != nil {
		var n, total int
		fmt.Sscanf(mm[1], "%d", &n)
		fmt.Sscanf(mm[2], "%d", &total)
		if n == 1 || total != m.total {
			m.steps = nil
		}
		for i := range m.steps {
			m.steps[i].done = true
		}
		m.steps = append(m.steps, step{title: mm[3]})
		m.total = total
		m.prog = nil
		return
	}
	m.title, m.steps, m.total, m.prog = t, nil, 0, nil
}

func (m *sessModel) resizeAll() {
	for _, q := range m.queue {
		q.resize(m.w, m.panelRows())
	}
}

func (m *sessModel) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.w, m.h = v.Width, v.Height
		m.resizeAll()
	case msgLine:
		m.addLine(v.e)
	case msgBanner:
		m.banner(v.title)
		return m, m.startTick()
	case msgProgress:
		m.prog = &progress{v.label, min(max(v.pct, 0), 100)}
		return m, m.startTick()
	case msgProgressClose:
		m.prog = nil
	case msgHeader:
		m.hdr = v.h
	case msgStripIfEmpty:
		if m.strip == nil {
			e := v.e
			e.text = oneLine(e.text)
			m.strip = &e
		}
	case msgAsk:
		v.q.resize(m.w, m.panelRows())
		m.queue = append(m.queue, v.q)
	case msgFlowDone:
		m.finished = true
		return m, tea.Quit
	case msgTick:
		if !m.busy() {
			m.ticking = false
			return m, nil
		}
		m.frame++
		return m, tea.Tick(tickEvery, func(time.Time) tea.Msg { return msgTick{} })
	case tea.KeyMsg:
		return m.key(v)
	}
	return m, nil
}

func (m *sessModel) key(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	m.strip = nil // any key clears the result strip
	q := m.current()
	if q == nil {
		if k.String() != "ctrl+c" {
			return m, nil
		}
		if m.stopping { // the second Ctrl-C leaves now; pending questions are answered No
			return m, tea.Quit
		}
		// The first one asks the flow to finish: no new question is asked, the current step ends.
		m.stopping = true
		m.st.stop()
		m.strip = &entry{kWarn, "Stopping after the current step. Ctrl-C again quits now."}
		return m, nil
	}
	q.model, _ = q.model.Update(k) // the models' own Quit commands are not used here
	if a, ok := q.finished(); ok {
		q.ch <- a
		m.queue = m.queue[1:]
		if len(m.queue) > 0 {
			m.queue[0].resize(m.w, m.panelRows())
		}
	}
	return m, nil
}

// lastResult is the last Good, Warn or Err of the transcript (what the user should still see after
// the session ends); ok is false when there was none.
func (m *sessModel) lastResult() (entry, bool) {
	for i := len(m.transcript) - 1; i >= 0; i-- {
		if k := m.transcript[i].kind; k == kGood || k == kWarn || k == kErr {
			return m.transcript[i], true
		}
	}
	return entry{}, false
}

// ---------------------------------------------------------------- drawing

func clip(s string, w int) string {
	if w < 1 {
		return ""
	}
	return lipgloss.NewStyle().MaxWidth(w).Render(s)
}

func badgeKind(level string) BadgeKind {
	switch level {
	case "ok":
		return BadgeOK
	case "warn":
		return BadgeWarn
	case "error":
		return BadgeErr
	case "info":
		return BadgeInfo
	}
	return BadgeMute
}

// header draws the product line and the camera line; the compact form is one line.
func (m *sessModel) header(w int) []string {
	th, h := m.th, m.hdr
	prod := h.Product
	if prod == "" {
		prod = "Akam"
	}
	var leftPlain, left string
	if h.Name == "" && h.IP == "" {
		leftPlain = "no camera"
		left = th.Muted.Render(leftPlain)
	} else {
		name := truncW(h.Name, max(w-4, 4), th.Glyphs.Ellipsis)
		rest := ""
		for _, p := range []string{h.IP, h.Firmware} {
			if p != "" {
				rest += "  " + p
			}
		}
		rest = truncW(rest, max(w-3-lipgloss.Width(name), 0), th.Glyphs.Ellipsis)
		leftPlain = name + rest
		left = th.Bold.Render(name) + rest
	}
	if m.compact() {
		return []string{" " + left}
	}
	l1 := " " + th.Muted.Render(truncW(prod, max(w-2, 4), th.Glyphs.Ellipsis))
	lw := lipgloss.Width(leftPlain)
	avail := w - 2 - lw - 2 // margins and the gap
	right, rw := "", 0
	for _, b := range h.Badges {
		bd := th.Badge(badgeKind(b.Level), b.Text)
		need := lipgloss.Width(bd)
		if rw > 0 {
			need += 3
		}
		if rw+need > avail {
			break // badges drop from the right: the first ones are the most important
		}
		if rw > 0 {
			right += "   "
		}
		right += bd
		rw += need
	}
	l2 := " " + left
	if rw > 0 {
		l2 += strings.Repeat(" ", max(w-2-lw-rw, 2)) + right
	}
	return []string{l1, l2}
}

func (m *sessModel) stripLine(w int) string {
	e := m.strip
	if e == nil {
		return ""
	}
	th := m.th
	switch e.kind {
	case kGood:
		return " " + th.Badge(BadgeOK, "") + " " + th.OK.Render(truncW(e.text, max(w-6, 8), th.Glyphs.Ellipsis))
	case kWarn:
		return " " + th.Badge(BadgeWarn, "") + " " + th.Warn.Render(truncW(e.text, max(w-6, 8), th.Glyphs.Ellipsis))
	case kErr:
		return " " + th.Badge(BadgeErr, "") + " " + th.Err.Render(truncW(e.text, max(w-6, 8), th.Glyphs.Ellipsis))
	}
	return "  " + th.Muted.Render(truncW(e.text, max(w-4, 8), th.Glyphs.Ellipsis))
}

func (m *sessModel) footer() string {
	th := m.th
	if q := m.current(); q != nil {
		return " " + th.Footer(q.keys()...)
	}
	if m.stopping {
		return " " + th.Footer("Ctrl-C", "quit now")
	}
	return " " + th.Footer("Ctrl-C", "stop")
}

// transcriptLines renders the transcript wrapped to w columns.
func (m *sessModel) transcriptLines(w int) []string {
	th := m.th
	var out []string
	for _, e := range m.transcript {
		var badge BadgeKind
		var st lipgloss.Style
		withBadge := true
		switch e.kind {
		case kBlank:
			out = append(out, "")
			continue
		case kGood:
			badge, st = BadgeOK, th.OK
		case kWarn:
			badge, st = BadgeWarn, th.Warn
		case kErr:
			badge, st = BadgeErr, th.Err
		default:
			withBadge = false
		}
		for _, raw := range strings.Split(e.text, "\n") {
			if withBadge {
				for i, l := range wrapW(raw, max(w-6, 8)) {
					if i == 0 {
						out = append(out, "  "+th.Badge(badge, "")+" "+st.Render(l))
					} else {
						out = append(out, "      "+st.Render(l))
					}
				}
				continue
			}
			for _, l := range wrapW(raw, max(w-4, 8)) {
				switch e.kind {
				case kBold:
					out = append(out, "  "+th.Bold.Render(l))
				case kHint:
					out = append(out, "  "+th.Muted.Render("What to do: "+l))
				default:
					out = append(out, "  "+l)
				}
			}
		}
	}
	return out
}

// head draws the title, the steps and the progress bar of the running operation.
func (m *sessModel) head(w int) []string {
	th := m.th
	var out []string
	if m.title != "" {
		l := " " + th.Bold.Render(truncW(m.title, max(w-2, 8), th.Glyphs.Ellipsis))
		if m.total > 0 && len(m.steps) > 0 {
			info := fmt.Sprintf("step %d of %d", len(m.steps), m.total)
			if gap := w - 1 - lipgloss.Width(l) - lipgloss.Width(info); gap > 1 {
				l += strings.Repeat(" ", gap) + th.Muted.Render(info)
			}
		}
		out = append(out, l)
	} else if m.total > 0 && len(m.steps) > 0 {
		out = append(out, " "+th.Muted.Render(fmt.Sprintf("step %d of %d", len(m.steps), m.total)))
	}
	barW := max(min(w-30, 24), 4)
	progLine := func(indent string) string {
		lbl := truncW(m.prog.label, max(w-len(indent)-barW-10, 6), th.Glyphs.Ellipsis)
		return indent + lbl + "  " + th.ProgressBar(m.prog.pct, barW)
	}
	for i, s := range m.steps {
		mark := th.Badge(BadgeOK, "")
		if !s.done {
			mark = th.Spinner(m.frame)
			if m.finished {
				mark = th.Badge(BadgeMute, "")
			}
		}
		out = append(out, "  "+mark+" "+truncW(s.title, max(w-8, 8), th.Glyphs.Ellipsis))
		if i == len(m.steps)-1 && m.prog != nil && !s.done {
			out = append(out, progLine("      "))
		}
	}
	if len(m.steps) == 0 && m.prog != nil {
		out = append(out, "  "+th.Spinner(m.frame)+" "+progLine(""))
	}
	return out
}

// panel draws the main panel in exactly rows lines.
func (m *sessModel) panel(rows, w int) []string {
	var lines []string
	if q := m.current(); q != nil {
		lines = q.lines() // an overlay takes over the whole panel
		if len(lines) > rows {
			// A window too small for a box keeps its title and its last lines: the command, the
			// question and the highlighted answer must never be the part that is cut off.
			lines = append([]string{lines[0]}, lines[len(lines)-(rows-1):]...)
		}
	} else {
		lines = m.head(w)
		if len(lines) > rows {
			lines = lines[:rows]
		}
		if rest := rows - len(lines); rest > 0 {
			t := m.transcriptLines(w)
			if len(lines) > 0 && len(t) > 0 && rest > 1 {
				lines = append(lines, "")
				rest--
			}
			if len(t) > rest {
				t = t[len(t)-rest:]
			}
			lines = append(lines, t...)
		}
	}
	for len(lines) < rows {
		lines = append(lines, "")
	}
	return lines
}

func (m *sessModel) View() string {
	w, h := m.w, m.h
	rows := m.panelRows()
	var out []string
	out = append(out, m.header(w)...)
	if !m.compact() {
		out = append(out, " "+m.th.Rule(max(w-2, 0)))
	}
	out = append(out, m.panel(rows, w)...)
	if !m.compact() {
		out = append(out, " "+m.th.Rule(max(w-2, 0)))
	}
	out = append(out, m.stripLine(w), m.footer())
	if len(out) > h && h > 0 {
		out = out[len(out)-h:]
	}
	for i, l := range out {
		out[i] = clip(l, w)
	}
	return strings.Join(out, "\n")
}
