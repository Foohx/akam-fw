package tui

import (
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The long-operation panel (specs/installer.md): a list of items with a spinner on the running one, a short
// note on the finished ones, muted pending ones, a progress bar with the speed and the time left when
// the flow reports sizes, and the command that runs. It is fed by ui.Operator (flows that know their
// items) and, without it, by the Banner "Step N/T" list and Progress calls of the session.

type opMark int

const (
	opPending opMark = iota
	opRunning
	opOK
)

type opItem struct {
	title string
	note  string
	mark  opMark
}

// opState is the operation on screen. Its fields belong to the program goroutine.
type opState struct {
	items  []opItem
	status string
	done   int64
	total  int64
	start  time.Time // the running item started (speed and time left)
}

// msgOp is one Operator call on its way to the program.
type msgOp struct {
	kind  string // plan, start, done, status, bytes, writing, end
	i     int
	text  string
	items []string
	a, b  int64
	on    bool
}

var _ ui.Operator = (*sessionUI)(nil)

func (s *sessionUI) OpPlan(items []string) {
	c := make([]string, len(items))
	for i, it := range items {
		c[i] = textsafe.Clean(it)
	}
	s.st.send(msgOp{kind: "plan", items: c})
}
func (s *sessionUI) OpStart(i int) { s.st.send(msgOp{kind: "start", i: i}) }
func (s *sessionUI) OpDone(i int, note string) {
	s.st.send(msgOp{kind: "done", i: i, text: textsafe.Clean(note)})
}
func (s *sessionUI) OpStatus(line string) {
	line = textsafe.Clean(line)
	s.log("SAY %s", line)
	s.st.send(msgOp{kind: "status", text: line})
}
func (s *sessionUI) OpBytes(done, total int64) { s.st.send(msgOp{kind: "bytes", a: done, b: total}) }

// OpWriting sets the write lock at once (an atomic of the session, not a message): the screen must
// refuse to stop from the very moment the flow says the write starts, whatever panel is shown.
func (s *sessionUI) OpWriting(on bool) {
	if on {
		s.st.wrote.Store(true)
	}
	s.st.writing.Store(on)
	s.st.send(msgOp{kind: "writing", on: on})
}
func (s *sessionUI) OpEnd() { s.st.send(msgOp{kind: "end"}) }

// opApply applies one Operator call.
func (m *sessModel) opApply(v msgOp) {
	if v.kind == "plan" {
		m.op = &opState{items: make([]opItem, len(v.items))}
		for i, t := range v.items {
			m.op.items[i].title = t
		}
		m.prog = nil
		return
	}
	o := m.op
	if o == nil {
		return
	}
	in := func(i int) bool { return i >= 0 && i < len(o.items) }
	switch v.kind {
	case "start":
		for i := range o.items {
			if o.items[i].mark == opRunning {
				o.items[i].mark = opOK
			}
		}
		if in(v.i) {
			o.items[v.i].mark = opRunning
		}
		o.done, o.total, o.start = 0, 0, m.now()
	case "done":
		if in(v.i) {
			o.items[v.i].mark, o.items[v.i].note = opOK, oneLine(v.text)
		}
	case "status":
		o.status = oneLine(v.text)
	case "bytes":
		o.done, o.total = max(v.a, 0), max(v.b, 0)
	case "writing":
	case "end":
		m.op = nil
	}
}

// stoppable is whether the screen offers to stop: never in the middle of a write to the camera.
func (m *sessModel) writing() bool { return m.st.writing.Load() }

const writeBlocked = "A write to the camera is under way: it cannot be interrupted safely. Wait until it says it is finished."

// sizeUnit is the divisor and the unit used to show sizes around ref bytes.
func sizeUnit(ref int64) (float64, string) {
	switch {
	case ref >= 1<<30:
		return 1 << 30, "GB"
	case ref >= 1<<20:
		return 1 << 20, "MB"
	}
	return 1 << 10, "KB"
}

func humanBytes(n, ref int64) string {
	div, unit := sizeUnit(ref)
	return fmt.Sprintf("%.1f %s", float64(n)/div, unit)
}

func humanDur(d time.Duration) string {
	s := int(d.Round(time.Second) / time.Second)
	if s < 60 {
		return fmt.Sprintf("%d s", s)
	}
	if s < 3600 {
		return fmt.Sprintf("%d min %d s", s/60, s%60)
	}
	return fmt.Sprintf("%d h %d min", s/3600, s%3600/60)
}

// timing is "12 s, about 11 s left" from the elapsed time and the fraction done (empty while too early
// to say anything useful).
func timing(el time.Duration, pct int) string {
	if el < time.Second {
		return ""
	}
	t := humanDur(el)
	if el >= 3*time.Second && pct >= 2 && pct < 100 {
		left := time.Duration(float64(el) * float64(100-pct) / float64(pct))
		t += ", about " + humanDur(left) + " left"
	}
	return t
}

// opBar is the line under the running item: bar and percentage, then sizes, speed and time as they fit.
func (m *sessModel) opBar(w int, indent string) string {
	th, o := m.th, m.op
	pct := 0
	if o.total > 0 {
		pct = int(min(o.done*100/o.total, 100))
	}
	barW := max(min(w-30, 24), 4)
	line := indent + th.ProgressBar(pct, barW)
	if o.total <= 0 {
		return line
	}
	el := m.now().Sub(o.start)
	div, unit := sizeUnit(o.total)
	extra := []string{fmt.Sprintf("%.1f / %.1f %s", float64(o.done)/div, float64(o.total)/div, unit)}
	if el >= time.Second && o.done > 0 {
		extra = append(extra, humanBytes(int64(float64(o.done)/el.Seconds()), o.total)+"/s")
	}
	if t := timing(el, pct); t != "" {
		extra = append(extra, t)
	}
	for _, e := range extra {
		if lipgloss.Width(line)+2+lipgloss.Width(e) > w-1 {
			break
		}
		line += "  " + th.Muted.Render(e)
	}
	return line
}

// opLines draws the items of the operation.
func (m *sessModel) opLines(w int) []string {
	th, o := m.th, m.op
	var out []string
	titleW := 0
	for _, it := range o.items {
		titleW = max(titleW, lipgloss.Width(it.title))
	}
	titleW = min(titleW, max(w/2, 12))
	for _, it := range o.items {
		var mark string
		switch it.mark {
		case opOK:
			mark = th.Badge(BadgeOK, "")
		case opRunning:
			mark = th.Spinner(m.frame)
			if m.finished {
				mark = th.Badge(BadgeMute, "")
			}
		default:
			mark = th.Badge(BadgeMute, "")
		}
		t := truncW(it.title, max(w-8, 8), th.Glyphs.Ellipsis)
		if it.mark == opPending {
			t = th.Muted.Render(t)
		}
		l := "  " + mark + " " + t
		if it.note != "" {
			gap := max(titleW-lipgloss.Width(it.title), 0) + 3
			l += strings.Repeat(" ", gap) + th.Muted.Render(truncW(it.note, max(w-12-titleW, 6), th.Glyphs.Ellipsis))
		}
		out = append(out, l)
		if it.mark == opRunning && !m.finished {
			if o.total > 0 {
				out = append(out, m.opBar(w, "      "))
			} else if m.prog != nil {
				out = append(out, m.progBar(w, "      "))
			}
		}
	}
	if o.status != "" {
		out = append(out, "", " "+th.Muted.Render(truncW("> "+o.status, max(w-2, 8), th.Glyphs.Ellipsis)))
	}
	return out
}

// progBar is the progress line of a Progress(label, pct) call, with the elapsed time and the time left.
func (m *sessModel) progBar(w int, indent string) string {
	th, p := m.th, m.prog
	barW := max(min(w-30, 24), 4)
	lbl := truncW(p.label, max(w-len(indent)-barW-10, 6), th.Glyphs.Ellipsis)
	line := indent + lbl + "  " + th.ProgressBar(p.pct, barW)
	if t := timing(m.now().Sub(p.start), p.pct); t != "" && lipgloss.Width(line)+2+lipgloss.Width(t) <= w-1 {
		line += "  " + th.Muted.Render(t)
	}
	return line
}

// opFooter is the footer of an operation: stopping is offered only where nothing can go wrong.
func (m *sessModel) opFooter() string {
	th := m.th
	pairs := []string{"?", "details"}
	if m.overflow {
		pairs = append(pairs, "up/down", "scroll")
		if m.scroll > 0 {
			pairs = append(pairs, "End", "latest")
		}
	}
	switch {
	case m.writing():
		return " " + th.Footer(pairs...) + "   " + th.Warn.Render("the write cannot be interrupted safely")
	case m.stopping:
		pairs = append(pairs, "Ctrl-C", "quit now")
	case m.st.wrote.Load():
		pairs = append(pairs, "Esc", "stop the read-back")
	default:
		pairs = append(pairs, "Esc", "stop (nothing on the camera changes)")
	}
	return " " + th.Footer(pairs...)
}

// opKey handles Esc and Ctrl-C while an operation runs. It returns true when it used the key.
func (m *sessModel) opKey(k string) bool {
	if k != "esc" && k != "ctrl+c" {
		return false
	}
	if m.writing() { // before any quit or stop path, with or without a panel
		m.strip = &entry{kWarn, writeBlocked}
		return true
	}
	if m.op == nil {
		return false
	}
	if k == "esc" {
		if !m.stopping {
			m.stopping = true
			m.st.stop()
			if m.st.wrote.Load() {
				m.strip = &entry{kWarn, "Stopping the read-back after the current step. The write to the camera is already done."}
			} else {
				m.strip = &entry{kWarn, "Stopping after the current step. Nothing on the camera changes."}
			}
		}
		return true
	}
	return false
}
