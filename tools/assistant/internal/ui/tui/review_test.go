package tui

import (
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// H1: a stop of one action must not disable the questions of the next one.
func TestStopIsRearmedByTheNextAction(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	stop := make(chan struct{})
	var e1, e2 error
	var yes bool
	h.run(func(u ui.UI) error {
		u.Banner("Action one")
		op := u.(ui.Operator)
		op.OpPlan([]string{"read"})
		op.OpStart(0)
		<-stop
		_, e1 = u.AskYN("never shown")
		u.(ui.OutputClearer).ClearOutput() // the next action starts
		yes, e2 = u.AskYN("Second action?")
		_, e3 := u.Danger("Erase", "Erase it?", "a fact")
		if e3 != nil {
			return e3
		}
		return nil
	})
	h.until(func() bool { return h.m.op != nil })
	h.press("esc")
	if !h.m.stopping || !h.st.stopped() {
		t.Fatal("Esc did not stop the action")
	}
	close(stop)
	q := h.ask()
	yn, ok := q.model.(*YesNo)
	if !ok || yn.Prompt != "Second action?" {
		t.Fatalf("the next action's question is %T %+v", q.model, q.model)
	}
	if h.m.stopping {
		t.Error("stopping survived the start of the next action")
	}
	h.press("y")
	q = h.ask()
	if yn, ok := q.model.(*YesNo); !ok || !yn.Danger {
		t.Fatalf("the danger box was not shown after a stop: %T", q.model)
	}
	h.press("n")
	h.finish()
	if !errors.Is(e1, ui.ErrNoInput) || e2 != nil || !yes {
		t.Errorf("e1=%v e2=%v yes=%v", e1, e2, yes)
	}
}

// B1/M1: the write lock is an atomic of the session. A new banner drops the panel, the lock stays:
// Ctrl-C and Esc quit nothing and stop nothing, and the footer offers no stop.
func TestWriteLockIndependentOfThePanel(t *testing.T) {
	h := newHarness(t, 100, 24, nil)
	release := make(chan struct{})
	h.run(func(u ui.UI) error {
		op := u.(ui.Operator)
		op.OpPlan([]string{"write"})
		op.OpStart(0)
		op.OpWriting(true)
		u.Banner("Reading back") // drops the operation panel
		<-release
		op.OpWriting(false)
		return nil
	})
	h.until(func() bool { return h.m.title == "Reading back" })
	if h.m.op != nil {
		t.Fatal("the banner kept the operation panel")
	}
	for _, k := range []string{"ctrl+c", "ctrl+c", "esc", "ctrl+c"} {
		_, cmd := h.m.Update(key(k))
		if cmd != nil {
			if _, quit := cmd().(tea.QuitMsg); quit {
				t.Fatalf("%s quit the program during a write", k)
			}
		}
		if h.m.stopping || h.st.stopped() {
			t.Fatalf("%s asked the flow to stop during a write", k)
		}
	}
	v := h.m.View()
	if !strings.Contains(v, "cannot be interrupted safely") {
		t.Errorf("no write notice:\n%s", v)
	}
	if strings.Contains(v, "Ctrl-C stop") || strings.Contains(v, "Esc stop") {
		t.Errorf("the footer offers a stop during a write:\n%s", v)
	}
	close(release)
	h.finish()
}

// B1: when the program ends while a write is under way, runSession waits for the flow without limit.
func TestRunSessionWaitsForAWrite(t *testing.T) {
	inR, inW := io.Pipe()
	defer inW.Close()
	out := &syncBuf{}
	var prog *tea.Program
	started, release := make(chan struct{}), make(chan struct{})
	done := make(chan error, 1)
	go func() {
		_, _, err := runSession(testTheme(), newBase(), nil, sessionOpts{in: inR, out: out, grace: 30 * time.Millisecond,
			hook: func(p *tea.Program) { prog = p }},
			func(u ui.UI) error {
				u.(ui.Operator).OpWriting(true)
				close(started)
				<-release
				u.(ui.Operator).OpWriting(false)
				return nil
			})
		done <- err
	}()
	<-started
	prog.Quit() // the program ends (a signal, a closed terminal) in the middle of the write
	select {
	case err := <-done:
		t.Fatalf("runSession returned during a write: %v", err)
	case <-time.After(300 * time.Millisecond): // ten times the grace
	}
	close(release)
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("err = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("runSession did not return once the write was over")
	}
}

// M3: after a write the stop only concerns the read-back and the strip says so.
func TestStopWordingFollowsThePhase(t *testing.T) {
	m, adv := opModel(100, 24)
	backupOp(m, adv)
	m.Update(key("esc"))
	if m.strip == nil || !strings.Contains(m.strip.text, "Nothing on the camera changes") {
		t.Errorf("before a write: %+v", m.strip)
	}
	m, adv = opModel(100, 24)
	backupOp(m, adv)
	op(m, msgOp{kind: "writing", on: true})
	op(m, msgOp{kind: "writing", on: false})
	if f := m.footer(); !strings.Contains(f, "read-back") || strings.Contains(f, "nothing on the camera changes") {
		t.Errorf("footer after a write: %q", f)
	}
	m.Update(key("esc"))
	if m.strip == nil || strings.Contains(m.strip.text, "Nothing on the camera changes") || !strings.Contains(m.strip.text, "read-back") {
		t.Errorf("after a write: %+v", m.strip)
	}
}

func dangerAsk(m *sessModel) {
	m.Update(msgAsk{newAsk(&YesNo{Prompt: "Erase the card?", Title: "Erase the SD card", Danger: true, Box: []string{
		"Camera: 192.0.2.41 (route: SSH)", "It erases and rewrites the APP partition (/dev/mtd7).",
		"Device: /dev/sdb1 (SD, 31.9 GB)", "This cannot be undone.", "Command:  mkfs.fat -F32 /dev/sdb1"}})})
}

// M2: a danger box that does not fit says how many lines are hidden and refuses y until it fits.
func TestDangerBoxThatDoesNotFitRefusesYes(t *testing.T) {
	m := layoutModel(50, 12)
	dangerAsk(m)
	v := m.View()
	if !strings.Contains(v, "lines hidden") || !strings.Contains(v, "enlarge the window") {
		t.Errorf("no notice about the hidden lines:\n%s", v)
	}
	if strings.Contains(m.footer(), "y yes") {
		t.Errorf("the footer offers y: %q", m.footer())
	}
	for _, k := range []string{"y", "Y"} {
		m.Update(key(k))
		if q := m.current(); q == nil || q.model.(*YesNo).Done {
			t.Fatalf("%s answered a box that is not fully visible", k)
		}
	}
	m.Update(key("left")) // highlight Yes, then Enter: still refused
	m.Update(key("enter"))
	if q := m.current(); q == nil || q.model.(*YesNo).Done {
		t.Fatal("Enter on Yes answered a box that is not fully visible")
	}
	if m.strip == nil || !strings.Contains(m.strip.text, "does not fit") {
		t.Errorf("strip: %+v", m.strip)
	}
	// enlarged: every fact is on screen and y works
	m.Update(tea.WindowSizeMsg{Width: 100, Height: 30})
	v = m.View()
	for _, f := range []string{"192.0.2.41", "APP partition", "/dev/sdb1"} {
		if !strings.Contains(v, f) {
			t.Errorf("fact %q missing in the enlarged window:\n%s", f, v)
		}
	}
	m.Update(key("y"))
	if m.current() != nil {
		t.Error("y did not answer once the box fits")
	}
	// and No is always possible, even when it does not fit
	m = layoutModel(50, 12)
	dangerAsk(m)
	m.Update(key("n"))
	if m.current() != nil {
		t.Error("n must always answer")
	}
}

// L6: a confirmation typed before the danger box could be read is ignored for a moment.
func TestDangerBoxIgnoresTypeAhead(t *testing.T) {
	old := dangerTypeAhead
	dangerTypeAhead = 300 * time.Millisecond
	t.Cleanup(func() { dangerTypeAhead = old })
	m, adv := opModel(100, 30)
	dangerAsk(m)
	m.Update(key("y"))
	if m.current() == nil {
		t.Fatal("y answered at once")
	}
	adv(100 * time.Millisecond)
	m.Update(key("y"))
	if m.current() == nil {
		t.Fatal("y answered after 100 ms")
	}
	m.Update(key("n")) // No is never delayed
	if m.current() != nil {
		t.Fatal("n was delayed")
	}
	dangerAsk(m)
	adv(400 * time.Millisecond)
	m.Update(key("y"))
	if m.current() != nil {
		t.Error("y after the delay was ignored")
	}
}

// M5: the footer of the options form fits 60, 80 and 120 columns and keeps Esc and the action keys.
func TestFormFooterFitsTheWindow(t *testing.T) {
	rows := []ui.FormRow{{ID: "a", Label: "Alpha", Kind: ui.FormInt, Value: "5", Min: 0, Max: 10},
		{ID: "b", Label: "Beta", Kind: ui.FormText, Value: "x", Group: "Two"}, {ID: "c", Label: "Gamma", Kind: ui.FormInt, Value: "1", Max: 4, Group: "One"}}
	keys := []ui.FormKey{{Key: "r", Label: "restart video"}, {Key: "n", Label: "network"}, {Key: "p", Label: "password"}, {Key: "t", Label: "set clock"}}
	for _, w := range []int{60, 80, 120} {
		m := layoutModel(w, 24)
		f := &Form{Title: "Camera options - salon (192.0.2.41)", Rows: rows, Keys: keys}
		m.Update(msgAsk{newAsk(f)})
		foot := m.footer()
		if lw := lipgloss.Width(foot); lw > w {
			t.Errorf("%d columns: the footer is %d wide: %q", w, lw, foot)
		}
		for _, want := range []string{"Esc back", "r restart"} {
			if !strings.Contains(foot, want) {
				t.Errorf("%d columns: %q missing in %q", w, want, foot)
			}
		}
		if w >= 80 && !strings.Contains(foot, "t set clock") {
			t.Errorf("%d columns: all action keys expected in %q", w, foot)
		}
	}
}

// L8: a board key carries the row it was pressed on, whatever the rows are when the flow reads it.
func TestBoardKeyCarriesTheRowID(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	b := chooserBoard()
	h.m.Update(msgBoardSet{b})
	h.m.Update(key("down")) // on garage
	h.m.Update(key("enter"))
	b2 := b
	b2.Rows = b.Rows[1:] // the first row went away while the key was waiting
	h.m.Update(msgBoardSet{b2})
	k, row, err := h.sui.BoardKey()
	if err != nil || k != "enter" || row != 0 || b2.Rows[row].ID != "192.0.2.58" {
		t.Errorf("key=%q row=%d err=%v", k, row, err)
	}
	h.m.Update(key("enter"))
	b3 := b
	b3.Rows = b.Rows[2:] // the row it was on is gone
	h.m.Update(msgBoardSet{b3})
	if _, row, _ := h.sui.BoardKey(); row != -1 {
		t.Errorf("a vanished row resolved to %d", row)
	}
}

// L10: End and G on an empty menu do not panic and Enter runs nothing.
func TestEmptyMenuKeys(t *testing.T) {
	m := NewMenu("Nothing", nil)
	m.Update(tea.WindowSizeMsg{Width: 60, Height: 16})
	for _, k := range []string{"G", "end", "g", "up", "down", "enter"} {
		m.Update(key(k))
	}
	_ = m.View()
	if m.Done || m.Cursor < 0 {
		t.Errorf("done=%v cursor=%d", m.Done, m.Cursor)
	}
}

// The session UI keeps the outcome of the running action itself, so the caller never has to wrap it.
func TestSessionUIRecordsTheActionResult(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var r ui.ActionResulter = h.sui
	h.sui.ClearOutput()
	h.sui.Good("one")
	h.sui.Err("broke here")
	h.sui.Good("later success")
	if got := r.ActionResult(); got.Level != "error" || got.Text != "broke here" {
		t.Errorf("result %+v", got)
	}
	h.sui.ClearOutput()
	h.sui.Warn(fmt.Sprint("careful  ", "now"))
	if got := r.ActionResult(); got.Level != "warn" || got.Text != "careful now" {
		t.Errorf("result %+v", got)
	}
}
