package tui

import (
	"fmt"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func errSpec() ui.ErrorSpec {
	return ui.ErrorSpec{
		Context: "Update this camera - salon  192.0.2.41",
		Title:   "The camera did not answer on SSH within 120 s.",
		Body:    []string{"It was reachable a minute ago.", "Nothing was written to the camera."},
		Todo:    []string{"check the camera's power and its Wi-Fi light", "run \"Find cameras\" to see whether its address changed"},
		Tech:    "camera answered: DESCRIBE 503, Retry-After 10",
		Help:    "More help: l shows the log; akam doctor checks this computer.",
		Retry:   true,
		Log:     "2026-11-12T10:00:00Z START akam\n2026-11-12T10:00:01Z SSH error: i/o timeout",
	}
}

func errModel(w, h int, sp ui.ErrorSpec) (*sessModel, *ask) {
	m := layoutModel(w, h)
	q := newAsk(&ErrorView{Spec: sp})
	m.Update(msgAsk{q})
	return m, q
}

// The error screen at three sizes: title, what happened, what to do, the technical line and the keys.
func TestErrorViewGoldens(t *testing.T) {
	for _, sz := range [][2]int{{80, 24}, {60, 16}, {120, 40}} {
		w, h := sz[0], sz[1]
		tag := fmt.Sprintf("%dx%d", w, h)
		m, _ := errModel(w, h, errSpec())
		v := m.View()
		checkFits(t, "error "+tag, v, w, h)
		for _, want := range []string{"The camera did not answer", "What to do", "DESCRIBE 503", "r retry", "l show the log", "c other camera", "Esc back"} {
			if !strings.Contains(v, want) {
				t.Errorf("error %s lacks %q:\n%s", tag, want, v)
			}
		}
		golden(t, "error_"+tag, v)
	}
}

// With no retry the footer has no r; a tiny window still keeps the title, the first advice and the keys.
func TestErrorViewNoRetryAndSmall(t *testing.T) {
	sp := errSpec()
	sp.Retry = false
	m, q := errModel(80, 24, sp)
	if strings.Contains(m.View(), "retry") {
		t.Error("retry offered without a retry")
	}
	m.Update(key("r"))
	if _, done := q.finished(); done {
		t.Error("r must do nothing when no retry is offered")
	}
	for _, sz := range [][2]int{{50, 12}, {40, 8}} {
		m, _ = errModel(sz[0], sz[1], errSpec())
		v := m.View()
		checkFits(t, fmt.Sprintf("small %v", sz), v, sz[0], sz[1])
		if !strings.Contains(v, "did not answer") || !strings.Contains(v, "Esc back") {
			t.Errorf("small %v lost the title or the keys:\n%s", sz, v)
		}
	}
}

// r, c and Esc answer the flow; l opens the log pane inside the screen and Esc closes only the pane.
func TestErrorViewKeys(t *testing.T) {
	for _, tc := range []struct{ k, want string }{{"r", ui.ErrKeyRetry}, {"c", ui.ErrKeyCamera}, {"esc", ui.ErrKeyBack}, {"ctrl+c", ui.ErrKeyBack}} {
		m, q := errModel(80, 24, errSpec())
		m.Update(key(tc.k))
		a, done := q.finished()
		if !done || a.text != tc.want {
			t.Errorf("key %s: done=%v answer=%q, want %q", tc.k, done, a.text, tc.want)
		}
		if m.current() != nil {
			t.Errorf("key %s: the screen is still open", tc.k)
		}
	}
	// "Enter" is not a way out: nothing happens on a stray key
	m, q := errModel(80, 24, errSpec())
	m.Update(key("enter"))
	m.Update(key("x"))
	if _, done := q.finished(); done {
		t.Error("a stray key closed the error screen")
	}

	m, q = errModel(80, 24, errSpec())
	m.Update(key("l"))
	v := m.View()
	if !strings.Contains(v, "SSH error: i/o timeout") || strings.Contains(v, "What to do") {
		t.Errorf("the log pane is not shown:\n%s", v)
	}
	golden(t, "error_log_80x24", v)
	m.Update(key("esc"))
	if _, done := q.finished(); done {
		t.Error("Esc in the log pane must only close the pane")
	}
	if !strings.Contains(m.View(), "What to do") {
		t.Errorf("the screen did not come back:\n%s", m.View())
	}
	m.Update(key("esc"))
	if _, done := q.finished(); !done {
		t.Error("Esc on the screen must go back")
	}
}

// Fail in the session is remembered and shown by the caller as a screen; the answer comes back, the
// title stays on the strip, and a Fail is forgotten once read.
func TestSessionFailThenScreen(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var key1 string
	h.run(func(u ui.UI) error {
		err := u.Fail("the camera did not answer.", "check the power; try again.")
		if err != ui.ErrFailed {
			return err
		}
		fr := u.(ui.FailReader)
		f, ok := fr.LastFail()
		if !ok || f.Msg != "the camera did not answer." || f.Hint != "check the power; try again." {
			return fmt.Errorf("LastFail: %+v %v", f, ok)
		}
		fr.ClearFail()
		if _, ok := fr.LastFail(); ok {
			return fmt.Errorf("ClearFail kept it")
		}
		var e error
		key1, e = u.(ui.ErrorScreener).ErrorScreen(ui.ErrorSpec{Title: "The camera did not answer.", Retry: true})
		return e
	})
	q := h.ask()
	if _, ok := q.model.(*ErrorView); !ok {
		t.Fatalf("the question is %T", q.model)
	}
	h.press("r")
	h.finish()
	if h.err != nil || key1 != ui.ErrKeyRetry {
		t.Errorf("err=%v key=%q", h.err, key1)
	}
	// plain behaviour of Fail is unchanged in the transcript: the error and the hint
	if got := h.m.transcriptText(); !strings.Contains(got, "the camera did not answer.") || !strings.Contains(got, "What to do: check the power") {
		t.Errorf("transcript:\n%s", got)
	}
}

// ClearOutput gives the next action a clean panel: no older lines, steps or strip above its own.
func TestSessionClearOutput(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		u.Banner("Check this computer")
		u.Say("old line one")
		u.Warn("old warning")
		u.(ui.OutputClearer).ClearOutput()
		u.Banner("Back up")
		u.Say("new line")
		return nil
	})
	h.finish()
	v := h.m.View()
	if strings.Contains(v, "old line") || strings.Contains(v, "old warning") || strings.Contains(v, "Check this computer") {
		t.Errorf("the previous action is still on screen:\n%s", v)
	}
	if !strings.Contains(v, "new line") {
		t.Errorf("the new line is missing:\n%s", v)
	}
	if got := h.m.transcriptText(); strings.Contains(got, "old") {
		t.Errorf("transcript kept old lines:\n%s", got)
	}
}

// The output of the last action stays reachable with ? from the home board.
func TestBoardQuestionMarkShowsLastOutput(t *testing.T) {
	m := layoutModel(80, 24)
	m.Update(msgBoardSet{ui.Board{Title: "Home", Rows: []ui.BoardRow{{ID: "a", Label: "Check this computer"}}, Help: []string{"Enter opens the highlighted line."}}})
	m.Update(msgLine{entry{kSay, "ffmpeg: found"}})
	m.Update(msgLine{entry{kWarn, "speaker: missing"}})
	m.Update(key("?"))
	v := m.View()
	if !strings.Contains(v, "ffmpeg: found") || !strings.Contains(v, "speaker: missing") || !strings.Contains(v, "Last action") {
		t.Errorf("the last output is not shown:\n%s", v)
	}
	if !strings.Contains(v, "Enter opens the highlighted line.") {
		t.Errorf("the help is missing under the output:\n%s", v)
	}
	golden(t, "board_last_output_80x24", v)
	m.Update(key("esc"))
	if m.pane != nil || m.board == nil || strings.Contains(m.View(), "Last action") {
		t.Errorf("Esc must close the pane and show the board:\n%s", m.View())
	}
	// with nothing printed, ? keeps toggling the inline help
	m.Update(msgClear{})
	m.Update(key("?"))
	if m.pane != nil || !m.board.help {
		t.Error("with no output ? must toggle the help of the board")
	}
}
