package tui

import (
	"fmt"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The session (specs/installer.md) is one long-lived full-screen program: a header with the camera
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
	msgClear         struct{}
)

const tickEvery = 100 * time.Millisecond

// sessState is what the program and the flow goroutine share.
type sessState struct {
	done      chan struct{} // closed when the program has ended
	send      func(tea.Msg)
	boardKeys chan boardEv // keys of the board for the flow (board.go)

	// writing is set by OpWriting for as long as a write to the camera is under way. It does not
	// depend on the operation panel (a banner or a new board may drop the panel): while it is set the
	// session offers no stop, ignores Ctrl-C and Esc, and runSession waits for the flow without limit.
	writing atomic.Bool
	// wrote is set once a write was started in the current action: what is left to stop is only the
	// read-back, and the strip must not say that nothing on the camera changes.
	wrote atomic.Bool

	mu       sync.Mutex
	cancel   chan struct{} // closed by a stop; replaced by rearm when the next action starts
	boardIDs []string      // the row IDs of the board on screen, for the keys the flow reads
}

func newSessState() *sessState {
	return &sessState{done: make(chan struct{}), cancel: make(chan struct{}), send: func(tea.Msg) {}, boardKeys: make(chan boardEv, 16)}
}

func (s *sessState) stop() {
	s.mu.Lock()
	defer s.mu.Unlock()
	select {
	case <-s.cancel:
	default:
		close(s.cancel)
	}
}

// rearm makes a stop of an earlier action forgotten: the next action may ask questions again.
func (s *sessState) rearm() {
	s.mu.Lock()
	defer s.mu.Unlock()
	select {
	case <-s.cancel:
		s.cancel = make(chan struct{})
	default:
	}
}

func (s *sessState) setBoardIDs(ids []string) {
	s.mu.Lock()
	s.boardIDs = ids
	s.mu.Unlock()
}

// rowOf is the index, in the board now on screen, of the row a key was pressed on: found by its ID
// (the rows may have changed since), -1 when that row is gone. A row without an ID keeps its index.
func (s *sessState) rowOf(ev boardEv) int {
	if ev.row < 0 {
		return -1
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if ev.id == "" {
		if ev.row < len(s.boardIDs) {
			return ev.row
		}
		return -1
	}
	for i, id := range s.boardIDs {
		if id == ev.id {
			return i
		}
	}
	return -1
}

// stopChan is the channel closed when the current action is asked to stop.
func (s *sessState) stopChan() <-chan struct{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cancel
}

func (s *sessState) stopped() bool {
	select {
	case <-s.stopChan():
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
	start time.Time // when this bar began (elapsed and time left)
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
	op    *opState // the long operation of ui.Operator, nil when none
	now   func() time.Time

	board *boardView // the persistent list of the home and the chooser, nil when none

	queue    []*ask   // the first one is on screen
	scroll   int      // lines the main panel is scrolled up from the latest output
	overflow bool     // the transcript is longer than the panel (set when drawn)
	pane     *Details // the "?" pane over the transcript, when open
	frame    int
	ticking  bool
	finished bool // the flow returned
	stopping bool // the user pressed Ctrl-C once
}

func newSessModel(th *Theme, st *sessState) *sessModel {
	return &sessModel{th: th, st: st, w: 80, h: 24, now: time.Now}
}

func (m *sessModel) Init() tea.Cmd { return nil }

func (m *sessModel) panelRows() int {
	if m.compact() {
		return max(m.h-3, 1)
	}
	return max(m.h-6, 1)
}

// compact is the single column fallback of specs/installer.md, section 10: under 60 columns or 14 lines.
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
	if m.prog != nil || m.op != nil {
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
	m.title, m.steps, m.total, m.prog, m.op = t, nil, 0, nil, nil
}

func (m *sessModel) resizeAll() {
	if m.pane != nil {
		m.pane.Update(tea.WindowSizeMsg{Width: m.w, Height: m.panelRows()})
	}
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
		before := 0
		if m.scroll > 0 {
			before = len(m.transcriptLines(m.w))
		}
		m.addLine(v.e)
		if m.scroll > 0 { // keep what the user is reading where it is
			m.scroll += len(m.transcriptLines(m.w)) - before
		}
	case msgBanner:
		m.banner(v.title)
		return m, m.startTick()
	case msgProgress:
		p := &progress{label: v.label, pct: min(max(v.pct, 0), 100), start: m.now()}
		if m.prog != nil && m.prog.label == p.label && p.pct >= m.prog.pct {
			p.start = m.prog.start
		}
		m.prog = p
		return m, m.startTick()
	case msgOp:
		m.opApply(v)
		return m, m.startTick()
	case msgProgressClose:
		m.prog = nil
	case msgHeader:
		m.hdr = v.h
	case msgBoardSet:
		m.op, m.stopping = nil, false // a board is a new screen: an earlier stop is forgotten
		m.setBoard(v.b)
	case msgBoardClose:
		m.board = nil
		m.st.setBoardIDs(nil)
	case msgStripIfEmpty:
		if m.strip == nil {
			e := v.e
			e.text = oneLine(e.text)
			m.strip = &e
		}
	case msgClear:
		m.clearOutput()
	case msgAsk:
		if ev, ok := v.q.model.(*ErrorView); ok && ev.Theme == nil {
			ev.Theme = m.th
		}
		v.q.resize(m.w, m.panelRows())
		m.queue = append(m.queue, v.q)
		m.armHead()
		m.pane = nil
	case msgFlowDone:
		m.finished = true
		m.st.writing.Store(false)
		return m, tea.Quit
	case msgTick:
		if !m.busy() {
			m.ticking = false
			return m, nil
		}
		m.frame++
		return m, tea.Tick(tickEvery, func(time.Time) tea.Msg { return msgTick{} })
	case formMsg:
		var cmd tea.Cmd
		for _, q := range m.queue {
			if q.model == tea.Model(v.target()) {
				cmd = q.update(msg)
			}
		}
		m.settle()
		return m, cmd
	case tea.KeyMsg:
		return m.key(v)
	}
	return m, nil
}

// clearOutput starts a new action on a clean panel: the transcript, the steps, the operation and the
// strip of the previous one are dropped (the home keeps the last output only until the next action).
func (m *sessModel) clearOutput() {
	m.stopping = false // the stop of the previous action does not carry over
	m.transcript, m.strip, m.scroll, m.pane = nil, nil, 0, nil
	m.title, m.steps, m.total, m.prog, m.op = "", nil, 0, nil, nil
}

// settle answers the overlay on screen when its model is finished.
func (m *sessModel) settle() {
	if q := m.current(); q != nil {
		if a, ok := q.finished(); ok {
			q.ch <- a
			m.queue = m.queue[1:]
			if len(m.queue) > 0 {
				m.queue[0].resize(m.w, m.panelRows())
			}
			m.armHead()
		}
	}
}

// transcriptText is the whole transcript as plain lines, for the "?" pane.
func (m *sessModel) transcriptText() string {
	g := m.th.Glyphs
	var b strings.Builder
	for _, e := range m.transcript {
		switch e.kind {
		case kGood:
			b.WriteString(g.OK + " ")
		case kWarn:
			b.WriteString(g.Warn + " ")
		case kErr:
			b.WriteString(g.Err + " ")
		case kHint:
			b.WriteString("What to do: ")
		}
		b.WriteString(e.text + "\n")
	}
	if b.Len() == 0 {
		return "Nothing to show yet."
	}
	return b.String()
}

// keyIdle handles the keys of a screen with no question: scrolling the output, the "?" pane and the
// two-step Ctrl-C. It returns false for a key it does not use.
func (m *sessModel) keyIdle(k tea.KeyMsg) {
	if m.pane != nil {
		if k.String() == "ctrl+c" {
			m.pane = nil
			return
		}
		m.pane.Update(k)
		if m.pane.Done {
			m.pane = nil
		}
		return
	}
	page := max(m.panelRows()-2, 1)
	switch k.String() {
	case "up", "k":
		m.scroll++
	case "down", "j":
		m.scroll--
	case "pgup":
		m.scroll += page
	case "pgdown":
		m.scroll -= page
	case "home":
		m.scroll = 1 << 20
	case "end":
		m.scroll = 0
	case "?":
		m.pane = &Details{Title: "Details", Text: m.transcriptText(), Theme: m.th, AtEnd: true}
		m.pane.Update(tea.WindowSizeMsg{Width: m.w, Height: m.panelRows()})
	}
	m.scroll = max(m.scroll, 0)
}

func (m *sessModel) key(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	m.strip = nil // any key clears the result strip
	q := m.current()
	if q == nil && m.board != nil && m.pane == nil {
		return m.boardKey(k)
	}
	if q == nil {
		if m.pane == nil && m.opKey(k.String()) {
			return m, nil
		}
		if k.String() != "ctrl+c" || m.pane != nil {
			m.keyIdle(k)
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
	if msg := m.dangerRefuses(q, k.String()); msg != "" {
		m.strip = &entry{kWarn, msg}
		return m, nil
	}
	cmd := q.update(k)
	m.settle()
	return m, cmd
}

// dangerTypeAhead is how long a danger box ignores a confirmation after it appeared: a key typed
// before the user could read it (type-ahead, a held Enter) must never answer it.
var dangerTypeAhead = 300 * time.Millisecond

// armHead starts the type-ahead guard of the question that is now on screen.
func (m *sessModel) armHead() {
	if q := m.current(); q != nil && q.since.IsZero() {
		q.since = m.now()
	}
}

func dangerBox(q *ask) (*YesNo, bool) {
	yn, ok := q.model.(*YesNo)
	return yn, ok && yn.Danger
}

// dangerCut keeps the title, a notice and the last lines (the question and the answer) of a danger
// box that does not fit in rows lines.
func (m *sessModel) dangerCut(lines []string, rows int) []string {
	q := m.current()
	if q == nil {
		return lines
	}
	if _, ok := dangerBox(q); !ok || rows < 3 {
		return append([]string{lines[0]}, lines[len(lines)-(rows-1):]...)
	}
	hidden := len(lines) - rows + 1
	note := " " + m.th.Warn.Render(fmt.Sprintf("%d lines hidden - enlarge the window to confirm", hidden))
	if hidden == 1 {
		note = " " + m.th.Warn.Render("1 line hidden - enlarge the window to confirm")
	}
	out := []string{lines[0], note}
	return append(out, lines[len(lines)-(rows-2):]...)
}

// dangerRefuses says why a key that would answer Yes to the danger box on screen is ignored ("" when
// it is not refused): the box does not fit the window, or it appeared a moment ago.
func (m *sessModel) dangerRefuses(q *ask, k string) string {
	yn, ok := dangerBox(q)
	if !ok {
		return ""
	}
	confirms := k == "y" || k == "Y" || (k == "enter" && yn.yes)
	if !confirms {
		return ""
	}
	if len(q.lines()) > m.panelRows() {
		return "The warning does not fit the window: enlarge it to read every line before answering yes."
	}
	if m.now().Sub(q.since) < dangerTypeAhead {
		return "Ignored: read the warning first, then answer."
	}
	return ""
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
		if _, ok := dangerBox(q); ok && len(q.lines()) > m.panelRows() {
			return " " + th.Footer("n/Enter/Esc/q", "no") // yes is refused until the box fits
		}
		if f, ok := q.model.(*Form); ok {
			return " " + th.Footer(f.keysFit(func(p []string) bool { return lipgloss.Width(th.Footer(p...))+2 <= m.w })...)
		}
		return " " + th.Footer(q.keys()...)
	}
	if m.pane != nil {
		return " " + th.Footer(m.pane.keys()...)
	}
	if m.board != nil {
		return m.boardFooter()
	}
	if m.op != nil || m.writing() {
		return m.opFooter()
	}
	var pairs []string
	if m.stopping {
		pairs = []string{"Ctrl-C", "quit now"}
	} else {
		pairs = []string{"Ctrl-C", "stop"}
	}
	if m.overflow {
		pairs = append(pairs, "up/down", "scroll")
		if m.scroll > 0 {
			pairs = append(pairs, "End", "latest")
		}
	}
	if len(m.transcript) > 0 {
		pairs = append(pairs, "?", "details")
	}
	return " " + th.Footer(pairs...)
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
		if info := m.stepInfo(); info != "" {
			if gap := w - 1 - lipgloss.Width(l) - lipgloss.Width(info); gap > 1 {
				l += strings.Repeat(" ", gap) + th.Muted.Render(info)
			}
		}
		out = append(out, l)
	} else if info := m.stepInfo(); info != "" {
		out = append(out, " "+th.Muted.Render(info))
	}
	progLine := func(indent string) string { return m.progBar(w, indent) }
	if m.op != nil {
		return append(out, m.opLines(w)...)
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
	m.overflow = false
	if q := m.current(); q == nil && m.pane != nil {
		lines = strings.Split(strings.TrimRight(m.pane.body(), "\n"), "\n")
	} else if q != nil {
		lines = q.lines() // an overlay takes over the whole panel
		if len(lines) > rows {
			// A window too small for the box never cuts facts silently: the box says how many lines
			// are hidden (the title, the question and the answer stay) and y is refused until it fits.
			lines = m.dangerCut(lines, rows)
		}
	} else if m.board != nil {
		lines = m.boardLines(rows, w)
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
				m.overflow = true
				m.scroll = min(m.scroll, len(t)-rest)
				t = t[len(t)-rest-m.scroll : len(t)-m.scroll]
			} else {
				m.scroll = 0
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

// stepInfo is "step 2 of 4": from the operation's items, else from the Banner step list.
func (m *sessModel) stepInfo() string {
	if m.op != nil {
		for i, it := range m.op.items {
			if it.mark == opRunning {
				return fmt.Sprintf("step %d of %d", i+1, len(m.op.items))
			}
		}
		return ""
	}
	if m.total > 0 && len(m.steps) > 0 {
		return fmt.Sprintf("step %d of %d", len(m.steps), m.total)
	}
	return ""
}
