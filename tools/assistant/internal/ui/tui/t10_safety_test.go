package tui

import (
	"bufio"
	"io"
	"os"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// TestDangerNotAnsweredByBoardStripsOrFormTraffic (specs/installer.md, section 12): everything the flow or the program can
// send while a red box is open (a new board, a cleared panel, strips, operation steps, a header, other
// keys than the answer keys) leaves the box open and unanswered.
func TestDangerNotAnsweredByBoardStripsOrFormTraffic(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var got bool
	h.run(func(u ui.UI) error {
		got, _ = u.Danger("WRITE THE FLASH", "Write it?", "cannot be undone")
		return nil
	})
	q := h.ask()
	msgs := []tea.Msg{
		msgBoardSet{ui.Board{Title: "Home"}}, msgBoardClose{}, msgClear{},
		msgStripIfEmpty{entry{kGood, "done"}}, msgLine{entry{kErr, "boom"}}, msgHeader{ui.Header{Product: "x"}},
		msgOp{kind: "plan", items: []string{"a"}}, msgOp{kind: "start", i: 0}, msgOp{kind: "writing", on: true},
		msgOp{kind: "writing", on: false}, msgProgressClose{}, msgProgress{"x", 50}, msgTick{},
	}
	for i := 0; i < 20; i++ {
		for _, m := range msgs {
			h.m.Update(m)
		}
	}
	// the board keys of the home and the chooser, and the keys of the other screens
	for _, k := range []string{"c", "s", "o", "t", "r", "f", "d", "a", "x", "1", "2", "9", "up", "down", "tab", "space", "?", "pgup", "pgdown", "home", "end"} {
		h.m.Update(key(k))
		if h.m.current() != q {
			t.Fatalf("the key %q closed or replaced the box", k)
		}
		if _, done := q.finished(); done {
			t.Fatalf("the key %q answered the box", k)
		}
	}
	select {
	case <-h.flowDone:
		t.Fatal("the flow was released without an answer")
	default:
	}
	h.press("enter")
	h.finish()
	if got {
		t.Error("answered yes")
	}
}

// TestNoTimerAnswersAQuestion: the widgets and the overlay that hold a question never start a timer,
// a tick or a deadline (a question waits for the user for ever).
func TestNoTimerAnswersAQuestion(t *testing.T) {
	for _, f := range []string{"overlay.go", "models.go", "checklist.go", "details.go", "errorview.go"} {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		for _, l := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(strings.TrimSpace(l), "//") {
				continue
			}
			for _, w := range []string{"time.After", "time.NewTimer", "time.AfterFunc", "tea.Tick", "time.Tick", "WithTimeout", "WithDeadline", "SetReadDeadline"} {
				if strings.Contains(l, w) {
					t.Errorf("%s uses %s: %s", f, w, strings.TrimSpace(l))
				}
			}
		}
	}
}

// TestInlineDangerDefaultsNo (the inline TUI, outside the session): the red box model answers No to
// everything but y or Yes then Enter and ignores window and mouse messages.
func TestInlineDangerDefaultsNo(t *testing.T) {
	for _, keys := range [][]string{{"enter"}, {"esc"}, {"q"}, {"Q"}, {"ctrl+c"}, {"n"}, {"right", "enter"}, {"x", "tab", "tab", "enter"}} {
		m := &YesNo{Prompt: "Write?", Title: "WRITE", Box: []string{"x"}, Danger: true}
		m.Update(tea.WindowSizeMsg{Width: 50, Height: 10})
		m.Update(tea.MouseMsg{})
		if m.Done {
			t.Fatal("a non-key message answered the box")
		}
		feed(m, keys...)
		if !m.Done || m.Result {
			t.Errorf("%v -> done=%v yes=%v", keys, m.Done, m.Result)
		}
	}
}

// TestFlashWriteCannotBeCancelledFromTheUI (T8 write lock): once the write is marked, no key stops,
// quits or hides it, with or without the details pane, and a resize or a new line changes nothing.
func TestFlashWriteCannotBeCancelledFromTheUI(t *testing.T) {
	m, adv := opModel(80, 24)
	backupOp(m, adv)
	op(m, msgOp{kind: "writing", on: true})
	keys := []string{"esc", "ctrl+c", "q", "Q", "ctrl+d", "n", "y", "enter", "ctrl+c", "esc", "?", "ctrl+c", "esc", "?", "q", "ctrl+c"}
	for _, k := range keys {
		_, cmd := m.Update(key(k))
		if cmd != nil {
			if msg := cmd(); msg != nil {
				if _, quit := msg.(tea.QuitMsg); quit {
					t.Fatalf("%s quit the program during the write", k)
				}
			}
		}
		if m.stopping || m.st.stopped() {
			t.Fatalf("%s asked the flow to stop during the write", k)
		}
		if !m.writing() {
			t.Fatalf("%s cleared the write mark", k)
		}
		m.Update(tea.WindowSizeMsg{Width: 60, Height: 14})
		m.Update(msgLine{entry{kSay, "  writing ..."}})
	}
}

const t10Secret = "Zq9-Hunter-Wifi-Pw"

// TestSecretsNeverReachTranscriptPaneOrErrorScreen: a registered secret that a camera text or an error
// message happens to carry is masked before it reaches the transcript, the details pane, the strip, the
// error screen or the log; the prompt that asked for it never shows it either.
func TestSecretsNeverReachTranscriptPaneOrErrorScreen(t *testing.T) {
	secrets := &state.Secrets{}
	base := &ui.Plain{In: bufio.NewReader(strings.NewReader("")), Out: io.Discard, ErrOut: io.Discard, ForceTT: true, Secrets: secrets}
	h := newHarness(t, 80, 24, base)
	var pw string
	h.run(func(u ui.UI) error {
		pw, _ = u.Secret("Wi-Fi password (never saved):")
		secrets.Add(pw)
		u.Say("camera said: " + pw)
		u.Warn("retry with " + pw)
		u.Err("join failed for " + pw)
		u.Hint("not " + pw)
		_ = u.(ui.TechFailer).FailTech("failed "+pw, "check "+pw, "tech "+pw)
		_, _ = u.(ui.ErrorScreener).ErrorScreen(ui.ErrorSpec{Context: "c " + pw, Title: "t " + pw, Body: []string{pw}, Todo: []string{pw},
			Tech: "x " + pw, Help: pw, Log: "line " + pw})
		return nil
	})
	h.ask()
	for _, k := range strings.Split(t10Secret, "") {
		h.press(k)
		if strings.Contains(h.m.View(), "Zq9") {
			t.Fatal("the secret is echoed")
		}
	}
	h.press("enter")
	h.ask() // the error screen
	if strings.Contains(h.m.View(), t10Secret) {
		t.Errorf("the error screen shows the secret:\n%s", h.m.View())
	}
	h.press("l") // the log pane of the error screen
	if strings.Contains(h.m.View(), t10Secret) {
		t.Errorf("the log pane shows the secret:\n%s", h.m.View())
	}
	h.press("esc")
	h.press("esc")
	h.finish()
	if pw != t10Secret {
		t.Fatalf("the flow got %q", pw)
	}
	for _, e := range h.m.transcript {
		if strings.Contains(e.text, t10Secret) {
			t.Errorf("the transcript holds the secret: %+v", e)
		}
	}
	if strings.Contains(h.m.transcriptText(), t10Secret) {
		t.Error("the details pane text holds the secret")
	}
	if strings.Contains(h.m.View(), t10Secret) {
		t.Errorf("the screen shows the secret:\n%s", h.m.View())
	}
	if f, ok := h.sui.LastFail(); ok && strings.Contains(f.Msg+f.Hint+f.Tech, t10Secret) {
		t.Errorf("the failure holds the secret: %+v", f)
	}
}
