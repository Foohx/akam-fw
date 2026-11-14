package tui

import (
	"fmt"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func longText(n int) string {
	var b strings.Builder
	for i := 1; i <= n; i++ {
		fmt.Fprintf(&b, "Line %02d of the explanation, with a few words so that it is long enough to wrap in a narrow window.\n", i)
		if i%5 == 0 {
			b.WriteString("\n")
		}
	}
	return b.String()
}

func TestDetailsScroll(t *testing.T) {
	d := &Details{Title: "Why is the clock off?", Text: longText(30), Theme: testTheme(), W: 80, H: 12}
	v := d.View()
	if !strings.Contains(v, "Line 01") || strings.Contains(v, "Line 30") || !strings.Contains(v, "lines 1-") {
		t.Errorf("start of the text:\n%s", v)
	}
	checkFits(t, "details", strings.TrimRight(v, "\n"), 80, 12+1)
	feed(d, "down", "down")
	if d.top != 2 {
		t.Errorf("down: %d", d.top)
	}
	feed(d, "pgdown")
	if d.top != 2+d.view()-1 {
		t.Errorf("pgdown: %d", d.top)
	}
	feed(d, "end")
	if v := d.View(); !strings.Contains(v, "Line 30") || strings.Contains(v, "Line 01") {
		t.Errorf("end:\n%s", v)
	}
	feed(d, "down", "pgdown")
	if d.Done {
		t.Error("scrolling past the end closes nothing")
	}
	feed(d, "home")
	if d.top != 0 {
		t.Error("home")
	}
	feed(d, "up")
	if d.top != 0 {
		t.Error("up at the top")
	}
	for _, k := range []string{"esc", "q", "?", "enter", "ctrl+c"} {
		d2 := &Details{Text: "x", Theme: testTheme()}
		feed(d2, k)
		if !d2.Done {
			t.Errorf("%s closes", k)
		}
	}
	// a short text has no position line
	if v := (&Details{Title: "t", Text: "short", Theme: testTheme(), H: 10}).View(); strings.Contains(v, "lines ") {
		t.Errorf("no position for a short text:\n%s", v)
	}
	// the transcript pane starts on its last lines
	e := &Details{Text: longText(30), Theme: testTheme(), W: 80, H: 12, AtEnd: true}
	if v := e.View(); !strings.Contains(v, "Line 30") {
		t.Errorf("AtEnd:\n%s", v)
	}
}

func TestDetailsGoldens(t *testing.T) {
	for _, sz := range [][2]int{{80, 24}, {60, 16}, {120, 40}} {
		w, h := sz[0], sz[1]
		d := &Details{Title: "Clock and time zone", Text: longText(20), Theme: testTheme(), W: w, H: h - 6}
		feed(d, "down", "down", "down")
		v := d.View()
		checkFits(t, "details", v, w, h)
		golden(t, fmt.Sprintf("details_%dx%d", w, h), v)
	}
}

func TestChecklistKeys(t *testing.T) {
	steps := []string{"Unplug the camera's USB-C power", "Take the microSD card out", "Plug the power back in"}
	for _, k := range []string{"space", "enter"} {
		c := &Checklist{Title: "Do this by hand", Steps: steps, Theme: testTheme()}
		feed(c, "x", "y", "n") // other keys do nothing
		if c.Done {
			t.Fatal("only Space or Enter acknowledge")
		}
		feed(c, k)
		if !c.Done || c.Cancelled {
			t.Errorf("%s: %+v", k, c)
		}
	}
	for _, k := range []string{"q", "esc", "ctrl+c"} {
		c := &Checklist{Steps: steps, Theme: testTheme()}
		feed(c, k)
		if !c.Done || !c.Cancelled {
			t.Errorf("%s stops: %+v", k, c)
		}
	}
}

func TestChecklistGoldens(t *testing.T) {
	steps := []string{"Unplug the camera's USB-C power", "Take the microSD card out and put it in this computer", "Plug the power back in and wait for the blue light to stay on, which takes about a minute"}
	for _, sz := range [][2]int{{80, 24}, {60, 16}, {120, 40}} {
		w, h := sz[0], sz[1]
		c := &Checklist{Title: "Power the camera again", Steps: steps, Theme: testTheme(), W: w}
		v := c.View()
		checkFits(t, "checklist", v, w, h)
		golden(t, fmt.Sprintf("checklist_%dx%d", w, h), v)
	}
}

// ---------------------------------------------------------------- in the session

// Plain and the inline UI do not implement the rich interfaces: callers keep their own path.
func TestRichInterfacesAreSessionOnly(t *testing.T) {
	for name, u := range map[string]ui.UI{"plain": &ui.Plain{}, "inline": &UI{Plain: &ui.Plain{}}} {
		if _, ok := u.(ui.Former); ok {
			t.Errorf("%s implements Former", name)
		}
		if _, ok := u.(ui.Detailer); ok {
			t.Errorf("%s implements Detailer", name)
		}
		if _, ok := u.(ui.Checklister); ok {
			t.Errorf("%s implements Checklister", name)
		}
	}
	var s ui.UI = &sessionUI{}
	if _, ok := s.(ui.Former); !ok {
		t.Error("the session must implement Former")
	}
}

func TestSessionForm(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	clk := &clock{}
	var calls []applied
	h.run(func(u ui.UI) error {
		return u.(ui.Former).Form("Camera options", sampleRows(), func(id, v string) (ui.FormRow, error) {
			calls = append(calls, applied{id, v})
			return ui.FormRow{}, nil
		})
	})
	q := h.ask()
	f, ok := q.model.(*Form)
	if !ok {
		t.Fatalf("overlay is %T", q.model)
	}
	f.After = clk.after
	if !strings.Contains(h.m.View(), "Brightness") || !strings.Contains(h.m.View(), "left/right change") {
		t.Errorf("the form is on screen:\n%s", h.m.View())
	}
	checkFits(t, "session form", h.m.View(), 80, 24)
	h.m.Update(key("right"))
	// the tick is routed to the form by the session, and its apply command is returned to the program
	_, cmd := h.m.Update(clk.last())
	if cmd == nil {
		t.Fatal("the session must return the form's command")
	}
	h.m.Update(cmd())
	if len(calls) != 1 || calls[0] != (applied{"picture.brightness", "51"}) {
		t.Errorf("%v", calls)
	}
	golden(t, "session_form_80x24", h.m.View())
	h.press("esc")
	h.finish()
	if h.err != nil {
		t.Errorf("Form: %v", h.err)
	}
}

// A camera text in a row cannot move the cursor, and an apply that panics is a refusal.
func TestSessionFormSafety(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	rows := []ui.FormRow{{ID: "a", Label: "Name\x1b[2J", Kind: ui.FormEnum, Value: "x\x07", Choices: []string{"x\x07", "y"}}}
	h.run(func(u ui.UI) error {
		return u.(ui.Former).Form("t\x1b[31m", rows, func(id, v string) (ui.FormRow, error) { panic("boom") })
	})
	f := h.ask().model.(*Form)
	f.After = (&clock{}).after
	if v := h.m.View(); strings.ContainsAny(v, "\x1b\x07") {
		t.Errorf("control characters drawn: %q", v)
	}
	h.m.Update(key("right"))
	_, c := f.Update(formTick{f, f.gen})
	h.m.Update(c())
	if f.rowErr[0] == "" || f.Rows[0].Value != f.real[0] {
		t.Errorf("a panic in apply must be a refusal and snap back: %+v", f.rowErr)
	}
	h.press("esc")
	h.finish()
}

func TestSessionFormEndsWithTheProgram(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		return u.(ui.Former).Form("t", sampleRows(), nil)
	})
	h.ask()
	close(h.st.done)
	<-h.flowDone
	if h.err == nil {
		t.Error("a form left open by the end of the program returns an error")
	}
}

func TestSessionDetailsAndChecklist(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		u.(ui.Detailer).Details("Why?", longText(40))
		return u.(ui.Checklister).Checklist("Power the camera again", []string{"Unplug it", "Plug it back in"})
	})
	h.ask()
	checkFits(t, "details", h.m.View(), 80, 24)
	golden(t, "session_details_80x24", h.m.View())
	h.press("down", "esc")
	h.ask()
	if !strings.Contains(h.m.View(), "[ ] 1. Unplug it") {
		t.Errorf("checklist:\n%s", h.m.View())
	}
	golden(t, "session_checklist_80x24", h.m.View())
	h.press("space")
	h.finish()
	if h.err != nil {
		t.Errorf("acknowledged: %v", h.err)
	}

	h = newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error { return u.(ui.Checklister).Checklist("t", []string{"a"}) })
	h.ask()
	h.press("q")
	h.finish()
	if h.err != ui.ErrNoInput {
		t.Errorf("q stops: %v", h.err)
	}
}

// ---------------------------------------------------------------- scrolling the main panel

func TestSessionScroll(t *testing.T) {
	m := layoutModel(80, 24)
	for i := 1; i <= 60; i++ {
		m.Update(msgLine{entry{kSay, fmt.Sprintf("output line %02d", i)}})
	}
	v := m.View()
	if !strings.Contains(v, "output line 60") || strings.Contains(v, "output line 01") || !strings.Contains(v, "up/down scroll") {
		t.Errorf("follows the output:\n%s", v)
	}
	if strings.Contains(v, "End latest") {
		t.Error("End is offered only when scrolled")
	}
	m.Update(key("pgup"))
	v = m.View()
	if strings.Contains(v, "output line 60") || !strings.Contains(v, "End latest") {
		t.Errorf("scrolled up:\n%s", v)
	}
	// new output does not move what is being read
	top := firstOutput(m.View())
	m.Update(msgLine{entry{kSay, "output line 61"}})
	if got := firstOutput(m.View()); got != top {
		t.Errorf("the view moved: %q -> %q", top, got)
	}
	m.Update(key("home"))
	if !strings.Contains(m.View(), "output line 01") {
		t.Errorf("home:\n%s", m.View())
	}
	checkFits(t, "scroll", m.View(), 80, 24)
	m.Update(key("end"))
	if !strings.Contains(m.View(), "output line 61") || m.scroll != 0 {
		t.Error("end follows again")
	}
	m.Update(key("pgup"))
	golden(t, "session_scrolled_80x24", m.View())
}

func firstOutput(v string) string {
	for _, l := range strings.Split(v, "\n") {
		if strings.Contains(l, "output line") {
			return l
		}
	}
	return ""
}

func TestSessionTranscriptPane(t *testing.T) {
	m := layoutModel(80, 24)
	for i := 1; i <= 40; i++ {
		m.Update(msgLine{entry{kSay, fmt.Sprintf("output line %02d", i)}})
	}
	m.Update(msgLine{entry{kErr, "The camera did not answer"}})
	m.Update(msgLine{entry{kHint, "check the power"}})
	m.Update(key("?"))
	if m.pane == nil {
		t.Fatal("? opens the pane")
	}
	v := m.View()
	if !strings.Contains(v, "[x] The camera did not answer") || !strings.Contains(v, "What to do: check the power") || !strings.Contains(v, "Esc/? close") {
		t.Errorf("transcript pane:\n%s", v)
	}
	checkFits(t, "pane", v, 80, 24)
	m.Update(key("home"))
	if !strings.Contains(m.View(), "output line 01") {
		t.Error("the whole transcript can be read")
	}
	// Ctrl-C closes the pane first; it does not ask the flow to stop
	m.Update(key("ctrl+c"))
	if m.pane != nil || m.stopping {
		t.Errorf("ctrl+c in the pane: %+v", m.stopping)
	}
	m.Update(key("?"))
	m.Update(key("esc"))
	if m.pane != nil {
		t.Error("esc closes")
	}
	// a question closes the pane and takes the panel
	m.Update(key("?"))
	m.Update(msgAsk{newAsk(&YesNo{Prompt: "Restart?", Title: "Question", Frame: true})})
	if m.pane != nil || !strings.Contains(m.View(), "Restart?") {
		t.Error("a question takes over")
	}
}
