package cli

import (
	"bufio"
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// errSession is the session UI reduced to what the error screen needs: it remembers Fail, answers the
// screen from a script and counts the clears.
type errSession struct {
	ui.UI
	fail    *ui.FailInfo
	keys    []string
	specs   []ui.ErrorSpec
	clears  int
	reports []string
}

func (s *errSession) Fail(msg, hint string) error { return s.FailTech(msg, hint, "") }
func (s *errSession) FailTech(msg, hint, tech string) error {
	s.fail = &ui.FailInfo{Msg: msg, Hint: hint, Tech: tech}
	return ui.ErrFailed
}
func (s *errSession) LastFail() (ui.FailInfo, bool) {
	if s.fail == nil {
		return ui.FailInfo{}, false
	}
	return *s.fail, true
}
func (s *errSession) ClearFail()   { s.fail = nil }
func (s *errSession) ClearOutput() { s.clears++; s.fail = nil }
func (s *errSession) Err(t string) { s.reports = append(s.reports, t) }
func (s *errSession) ErrorScreen(sp ui.ErrorSpec) (string, error) {
	s.specs = append(s.specs, sp)
	k := s.keys[0]
	s.keys = s.keys[1:]
	return k, nil
}

func errApp(u ui.UI) *App {
	return &App{UI: u, Opt: &Options{IP: "192.0.2.41"}, Cams: &state.Cameras{Name: "salon"}}
}

// A failed action shows the screen with the words of Fail; r runs the action again on a clean panel,
// Esc returns, and the screen is not shown for an action that succeeded.
func TestRunScreenRetryAndBack(t *testing.T) {
	s := &errSession{keys: []string{ui.ErrKeyRetry, ui.ErrKeyBack}}
	a := errApp(s)
	runs := 0
	a.runScreen("Update this camera", func() error {
		runs++
		return a.failTech("the camera did not answer on SSH within 120 s. It may be starting.", "check the power; run Find cameras. Nothing was written.", "ssh: i/o timeout")
	})
	if runs != 2 {
		t.Fatalf("the action ran %d times, want 2 (one retry)", runs)
	}
	if s.clears != 1 {
		t.Errorf("the output was cleared %d times on retry, want 1", s.clears)
	}
	if len(s.specs) != 2 {
		t.Fatalf("%d screens", len(s.specs))
	}
	sp := s.specs[0]
	if sp.Title != "The camera did not answer on SSH within 120 s." || len(sp.Body) != 1 || sp.Body[0] != "It may be starting." {
		t.Errorf("title/body: %q %q", sp.Title, sp.Body)
	}
	if strings.Join(sp.Todo, "|") != "check the power|run Find cameras|Nothing was written" {
		t.Errorf("todo: %q", sp.Todo)
	}
	if sp.Tech != "ssh: i/o timeout" || !sp.Retry || sp.Context != "Update this camera - salon  192.0.2.41" || sp.Log == "" || sp.Help == "" {
		t.Errorf("spec: %+v", sp)
	}
	if _, ok := s.LastFail(); ok {
		t.Error("the Fail was not cleared after it was shown")
	}

	s = &errSession{}
	a = errApp(s)
	a.runScreen("Settings", func() error { return nil })
	if len(s.specs) != 0 {
		t.Error("a screen for an action that succeeded")
	}
}

// An error that is not a Fail still gets the screen; a user who stopped (ErrNoInput), a flow that
// printed its own message without Fail, and a UI with no screen keep the strip and the old report.
func TestRunScreenOtherErrors(t *testing.T) {
	s := &errSession{keys: []string{ui.ErrKeyBack}}
	a := errApp(s)
	a.runScreen("Backup", func() error { return errors.New("disk full") })
	if len(s.specs) != 1 || s.specs[0].Title != "Disk full" {
		t.Errorf("specs: %+v", s.specs)
	}

	s = &errSession{}
	a = errApp(s)
	a.runScreen("Backup", func() error { return ui.ErrNoInput })
	a.runScreen("Backup", func() error { return ui.ErrFailed }) // no Fail was called
	if len(s.specs) != 0 {
		t.Errorf("a screen was shown: %+v", s.specs)
	}
	if len(s.reports) != 1 { // ErrNoInput is reported as before; ErrFailed was already shown
		t.Errorf("reports: %q", s.reports)
	}

	var out, errOut bytes.Buffer
	p := &ui.Plain{In: bufio.NewReader(strings.NewReader("")), Out: &out, ErrOut: &errOut}
	a = errApp(p)
	a.runScreen("Backup", func() error { return a.failTech("it broke.", "try again.", "tech") })
	if out.String() != "  What to do: try again.\n" || errOut.String() != "ERROR: it broke.\n" {
		t.Errorf("plain Fail changed: out=%q err=%q", out.String(), errOut.String())
	}
}

// The words of the screen: no CLI wording leaks into a missing hint, "read the lines above" points at
// the log key, a full stop inside a sentence does not split it.
func TestErrorSpecWords(t *testing.T) {
	a := errApp(&errSession{})
	sp := a.errorSpec("", ui.FailInfo{Msg: "no kit at /a/b.c/kit. Run get-kit.", Hint: ""}, false)
	if sp.Title != "No kit at /a/b.c/kit." || len(sp.Body) != 1 || sp.Retry {
		t.Errorf("%+v", sp)
	}
	if len(sp.Todo) != 1 || !strings.Contains(sp.Todo[0], "press l") {
		t.Errorf("todo: %q", sp.Todo)
	}
	sp = a.errorSpec("", ui.FailInfo{Msg: "the camera did not run x.", Hint: "read the lines above."}, true)
	if len(sp.Todo) != 1 || !strings.Contains(sp.Todo[0], "press l") {
		t.Errorf("todo: %q", sp.Todo)
	}
}

// The text diet of the full-screen menus: every step has one short clause that keeps its risk, and the
// home rows stay under 60 columns; the plain texts are the long ones, unchanged.
func TestTextDietShortClauses(t *testing.T) {
	for _, s := range state.Steps {
		short := stepWhyShort(s)
		if short == "" || len(short) > 40 || len(short) >= len(stepWhy(s)) {
			t.Errorf("step %s: short %q, long %q", s, short, stepWhy(s))
		}
		if strings.Contains(stepWhy(s), "ERASES") != strings.Contains(short, "ERASES") || strings.Contains(stepWhy(s), "WRITES") != strings.Contains(short, "WRITES") {
			t.Errorf("step %s: the short text lost the risk: %q", s, short)
		}
	}
	b, _ := errApp(&errSession{}).homeBoard()
	for _, r := range b.Rows {
		if n := len(r.Label) + 2 + len(r.Detail); n > 60 {
			t.Errorf("home row %q is %d columns", r.Label, n)
		}
	}
}
