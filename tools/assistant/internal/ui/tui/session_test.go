package tui

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// ---------------------------------------------------------------- harness

// harness drives a sessModel by hand: the flow runs in a goroutine against a real sessionUI, its
// messages arrive on a channel and the test feeds them (and the keys) to Update, so there is no
// terminal and no timing involved.
type harness struct {
	t        *testing.T
	m        *sessModel
	st       *sessState
	sui      *sessionUI
	msgs     chan tea.Msg
	flowDone chan struct{}
	err      error
	logs     []string
	logMu    sync.Mutex
}

func newBase() ui.UI {
	return &ui.Plain{In: bufio.NewReader(strings.NewReader("")), Out: io.Discard, ErrOut: io.Discard, ForceTT: true}
}

func newHarness(t *testing.T, w, h int, base ui.UI) *harness {
	t.Helper()
	hn := &harness{t: t, msgs: make(chan tea.Msg, 512), flowDone: make(chan struct{})}
	hn.st = newSessState()
	hn.st.send = func(m tea.Msg) { hn.msgs <- m }
	hn.m = newSessModel(NewTheme(Env{Profile: termenv.Ascii, ASCII: true}), hn.st)
	hn.m.Update(tea.WindowSizeMsg{Width: w, Height: h})
	if base == nil {
		base = newBase()
	}
	hn.sui = &sessionUI{base: base, st: hn.st, logf: func(f string, a ...any) {
		hn.logMu.Lock()
		hn.logs = append(hn.logs, fmt.Sprintf(f, a...))
		hn.logMu.Unlock()
	}}
	return hn
}

func (h *harness) run(fn func(ui.UI) error) {
	go func() {
		h.err = fn(h.sui)
		close(h.flowDone)
		h.msgs <- msgFlowDone{}
	}()
}

func (h *harness) pump() {
	h.t.Helper()
	select {
	case msg := <-h.msgs:
		h.m.Update(msg)
	case <-time.After(5 * time.Second):
		h.t.Fatal("timeout waiting for the flow")
	}
}

func (h *harness) until(cond func() bool) {
	h.t.Helper()
	for !cond() {
		h.pump()
	}
}

// ask waits for a question on screen and returns its overlay.
func (h *harness) ask() *ask {
	h.t.Helper()
	h.until(func() bool { return h.m.current() != nil })
	return h.m.current()
}

// press feeds keys to the program.
func (h *harness) press(keys ...string) {
	for _, k := range keys {
		h.m.Update(key(k))
	}
}

// finish pumps until the flow has returned and its end was processed.
func (h *harness) finish() {
	h.t.Helper()
	h.until(func() bool { return h.m.finished })
	<-h.flowDone
}

func (h *harness) logged() []string {
	h.logMu.Lock()
	defer h.logMu.Unlock()
	return append([]string(nil), h.logs...)
}

// ---------------------------------------------------------------- flows through the session

// A flow written against ui.UI only runs unchanged in the session; each question blocks its goroutine
// until the user answers, and the result of the flow is on the strip.
func TestSessionRunsAFlowWrittenAgainstUI(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var picked int
	var yes bool
	h.run(func(u ui.UI) error {
		u.Banner("Camera options - salon")
		u.Say("Reading the options")
		var err error
		if picked, err = u.Pick("Which group?", []string{"Video", "Picture", "LED", "Back"}); err != nil {
			return err
		}
		if yes, err = u.AskYN("Turn the front LED off?"); err != nil {
			return err
		}
		u.Progress("Applying", 40)
		u.Progress("Applying", 100)
		u.ProgressClose()
		u.Good("Front LED set to off")
		return nil
	})
	q := h.ask()
	if _, ok := q.model.(*Picker); !ok {
		t.Fatalf("first question is %T", q.model)
	}
	if strings.Contains(q.body(), "Back") {
		t.Error("the trailing Back row must not be drawn in the TUI")
	}
	h.press("2")
	if _, ok := h.ask().model.(*YesNo); !ok {
		t.Fatal("second question is not a yes/no")
	}
	h.press("y")
	h.finish()
	if h.err != nil || picked != 1 || !yes {
		t.Errorf("err=%v picked=%d yes=%v", h.err, picked, yes)
	}
	if e := h.m.strip; e == nil || e.kind != kGood || e.text != "Front LED set to off" {
		t.Errorf("strip: %+v", e)
	}
	if last, ok := h.m.lastResult(); !ok || last.text != "Front LED set to off" {
		t.Errorf("last result: %+v %v", last, ok)
	}
	if !strings.Contains(h.m.View(), "Front LED set to off") {
		t.Errorf("the result is not on screen:\n%s", h.m.View())
	}
}

// Esc on a Pick that has a trailing Back row answers that row, like choosing it.
func TestSessionPickEscIsBack(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var idx int
	var err error
	h.run(func(u ui.UI) error { idx, err = u.Pick("t", []string{"a", "b", "Back"}); return nil })
	h.ask()
	h.press("esc")
	h.finish()
	if idx != 2 || err != nil {
		t.Errorf("idx=%d err=%v", idx, err)
	}
	// without a Back row Esc is a cancel
	h = newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error { idx, err = u.Pick("t", []string{"a", "b"}); return nil })
	h.ask()
	h.press("ctrl+c")
	h.finish()
	if idx != -1 || !errors.Is(err, ui.ErrNoInput) {
		t.Errorf("idx=%d err=%v", idx, err)
	}
}

func TestSessionPromptAndSecret(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var name, pw string
	h.run(func(u ui.UI) error {
		name, _ = u.Prompt("Name [salon]:", "salon")
		pw, _ = u.Secret("Password:")
		u.Good("Saved")
		return nil
	})
	h.ask()
	h.press("enter") // the default
	h.ask()
	for _, k := range []string{"h", "u", "n", "t", "e", "r", "2"} {
		h.press(k)
		if v := h.m.View(); strings.Contains(v, "hunter") || strings.Contains(v, "*") {
			t.Fatalf("the secret is on screen: %q", v)
		}
	}
	h.press("enter")
	h.finish()
	if name != "salon" || pw != "hunter2" {
		t.Errorf("name=%q pw=%q", name, pw)
	}
	for _, e := range h.m.transcript {
		if strings.Contains(e.text, "hunter") {
			t.Errorf("the secret reached the transcript: %+v", e)
		}
	}
	for _, l := range h.logged() {
		if strings.Contains(l, "hunter") {
			t.Errorf("the secret reached the log: %q", l)
		}
	}
	if strings.Contains(h.m.View(), "hunter") {
		t.Error("the secret is on the final screen")
	}
}

// ---------------------------------------------------------------- safety invariants

// TestDangerDefaultsNo: the red box starts on No; Enter, Esc, q and Ctrl-C answer No; only y or
// Yes then Enter answer yes. Also as a session overlay.
func TestDangerDefaultsNo(t *testing.T) {
	cases := []struct {
		keys []string
		want bool
	}{
		{[]string{"enter"}, false}, {[]string{"esc"}, false}, {[]string{"q"}, false}, {[]string{"Q"}, false},
		{[]string{"ctrl+c"}, false}, {[]string{"n"}, false}, {[]string{"x", "1", "space", "enter"}, false},
		{[]string{"right", "enter"}, false}, {[]string{"left", "right", "enter"}, false},
		{[]string{"y"}, true}, {[]string{"left", "enter"}, true},
	}
	for _, c := range cases {
		h := newHarness(t, 80, 24, nil)
		var got bool
		var err error
		h.run(func(u ui.UI) error {
			got, err = u.Danger("ERASE THE SD CARD", "Erase the card?", "This erases /dev/sdb.", "Command:  mkfs.fat -F32 /dev/sdb1")
			return nil
		})
		q := h.ask()
		v := h.m.View()
		if !strings.Contains(v, "(*) No") || strings.Contains(v, "(*) Yes") || !strings.Contains(v, "mkfs.fat -F32 /dev/sdb1") {
			t.Fatalf("the box must show the command and highlight No:\n%s", v)
		}
		if _, ok := q.model.(*YesNo); !ok || !q.model.(*YesNo).Danger {
			t.Fatal("not a danger box")
		}
		h.press(c.keys...)
		h.finish()
		if err != nil || got != c.want {
			t.Errorf("%v -> %v, %v (want %v)", c.keys, got, err, c.want)
		}
	}
}

// Nothing answers a danger box by itself: not time, not messages, not the end of the flow's other
// output. Only a key does.
func TestDangerNoAutoAnswer(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var got bool
	h.run(func(u ui.UI) error {
		got, _ = u.Danger("WRITE THE FLASH", "Write it?", "cannot be undone")
		return nil
	})
	h.ask()
	for i := 0; i < 500; i++ {
		h.m.Update(msgTick{})
		h.m.Update(msgLine{entry{kGood, "toast"}})
		h.m.Update(msgProgress{"x", i % 100})
		h.m.Update(tea.WindowSizeMsg{Width: 70 + i%20, Height: 20})
		h.m.Update(tea.MouseMsg{})
	}
	if h.m.current() == nil {
		t.Fatal("the box went away without a key")
	}
	select {
	case <-h.flowDone:
		t.Fatal("the flow was released without an answer")
	default:
	}
	h.press("n")
	h.finish()
	if got {
		t.Error("answered yes")
	}
}

// The end of the program while a question is pending answers No and releases the flow.
func TestSessionEndAnswersNo(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var yn, dg bool
	var e1, e2 error
	h.run(func(u ui.UI) error {
		yn, e1 = u.AskYN("Do it?")
		dg, e2 = u.Danger("T", "Sure?", "x")
		return nil
	})
	h.ask()
	close(h.st.done) // what runSession does once the program has ended
	select {
	case <-h.flowDone:
	case <-time.After(5 * time.Second):
		t.Fatal("the flow is still blocked after the program ended")
	}
	if yn || dg || !errors.Is(e1, ui.ErrNoInput) || !errors.Is(e2, ui.ErrNoInput) {
		t.Errorf("yn=%v dg=%v e1=%v e2=%v", yn, dg, e1, e2)
	}
}

func TestAskYNDefaultNo(t *testing.T) {
	for _, keys := range [][]string{{"enter"}, {"esc"}, {"q"}, {"ctrl+c"}, {"n"}, {"tab", "enter"}} {
		h := newHarness(t, 80, 24, nil)
		var got bool
		h.run(func(u ui.UI) error { got, _ = u.AskYN("Restart the video service?"); return nil })
		h.ask()
		if v := h.m.View(); !strings.Contains(v, "(*) No") {
			t.Fatalf("No must be highlighted:\n%s", v)
		}
		h.press(keys...)
		h.finish()
		if got {
			t.Errorf("%v answered yes", keys)
		}
	}
	h := newHarness(t, 80, 24, nil)
	var got bool
	h.run(func(u ui.UI) error { got, _ = u.AskYN("ok?"); return nil })
	h.ask()
	h.press("y")
	h.finish()
	if !got {
		t.Error("y")
	}
}

func TestDangerRefusesWithoutTTY(t *testing.T) {
	var errOut bytes.Buffer
	base := &ui.Plain{In: bufio.NewReader(strings.NewReader("")), Out: io.Discard, ErrOut: &errOut}
	h := newHarness(t, 80, 24, base)
	var got bool
	var err error
	h.run(func(u ui.UI) error { got, err = u.Danger("ERASE", "Erase?", "x"); return nil })
	h.finish() // no question may appear
	if got || !errors.Is(err, ui.ErrFailed) {
		t.Errorf("got=%v err=%v", got, err)
	}
	if h.m.strip == nil || h.m.strip.kind != kErr || !strings.Contains(h.m.strip.text, "needs an interactive terminal") {
		t.Errorf("strip: %+v", h.m.strip)
	}
}

// A rehearsal never opens a box: the same WOULD line as the plain Danger, and the answer is yes.
func TestSessionDangerDryRun(t *testing.T) {
	base := &ui.Plain{In: bufio.NewReader(strings.NewReader("")), Out: io.Discard, ErrOut: io.Discard, DryRun: true}
	h := newHarness(t, 80, 24, base)
	var got bool
	h.run(func(u ui.UI) error { got, _ = u.Danger("ERASE", "Erase?", "a", "b"); return nil })
	h.finish()
	if !got || h.m.strip == nil || h.m.strip.text != "WOULD ask: Erase? [y/N] (default: No) -- ERASE: a b" {
		t.Errorf("got=%v strip=%+v", got, h.m.strip)
	}
}

// The command is in the log before the answer is given (the facts of a danger box are logged as the
// plain Danger logs them).
func TestSessionLogsBeforeAnswer(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		u.Say("hello")
		u.Warn("careful")
		u.Err("broke")
		_, _ = u.Danger("ERASE", "Erase?", "Command:  mkfs.fat -F32 /dev/sdb1")
		return nil
	})
	h.ask()
	got := strings.Join(h.logged(), "|")
	for _, want := range []string{"SAY hello", "WARN careful", "ERROR broke", "WARN ERASE", "SAY   Command:  mkfs.fat -F32 /dev/sdb1"} {
		if !strings.Contains(got, want) {
			t.Errorf("log lacks %q: %s", want, got)
		}
	}
	h.press("n")
	h.finish()
}

// ---------------------------------------------------------------- Ctrl-C and the end

// Ctrl-C on a question answers it (No / cancel) and the flow ends cleanly.
func TestSessionCtrlCDuringQuestion(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var perr, yerr error
	var yes bool
	h.run(func(u ui.UI) error {
		_, perr = u.Prompt("Name:", "x")
		if perr != nil {
			return perr
		}
		yes, yerr = u.AskYN("never reached")
		return nil
	})
	h.ask()
	h.press("ctrl+c")
	h.finish()
	if !errors.Is(h.err, ui.ErrNoInput) || !errors.Is(perr, ui.ErrNoInput) || yes || yerr != nil {
		t.Errorf("flow error %v, prompt %v, yes=%v/%v", h.err, perr, yes, yerr)
	}
}

// Ctrl-C while the flow works asks it to stop (no new question is asked); a second one quits now.
func TestSessionCtrlCStopsTheFlow(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	release := make(chan struct{})
	var yes bool
	var err error
	h.run(func(u ui.UI) error {
		u.Say("working")
		<-release
		yes, err = u.AskYN("Continue?") // must not be shown after the stop
		return nil
	})
	h.until(func() bool { return len(h.m.transcript) == 1 })
	_, cmd := h.m.Update(key("ctrl+c"))
	if cmd != nil || !h.m.stopping || !h.st.stopped() {
		t.Fatalf("first ctrl+c: cmd=%v stopping=%v", cmd != nil, h.m.stopping)
	}
	if h.m.strip == nil || !strings.Contains(h.m.strip.text, "Stopping") {
		t.Errorf("strip: %+v", h.m.strip)
	}
	close(release)
	h.finish()
	if yes || !errors.Is(err, ui.ErrNoInput) || h.m.current() != nil {
		t.Errorf("yes=%v err=%v: a question was asked after the stop", yes, err)
	}
	// second Ctrl-C quits
	m2 := newSessModel(h.m.th, newSessState())
	m2.Update(key("ctrl+c"))
	if _, cmd := m2.Update(key("ctrl+c")); cmd == nil {
		t.Error("the second ctrl+c must quit")
	}
}

// ---------------------------------------------------------------- the real program

type syncBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuf) Write(p []byte) (int, error) { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Write(p) }
func (s *syncBuf) String() string              { s.mu.Lock(); defer s.mu.Unlock(); return s.b.String() }

func waitFor(t *testing.T, b *syncBuf, text string) {
	t.Helper()
	for i := 0; i < 500; i++ {
		if strings.Contains(b.String(), text) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for %q in:\n%s", text, b.String())
}

func testTheme() *Theme { return NewTheme(Env{Profile: termenv.Ascii, ASCII: true}) }

// noLeak fails when a goroutine of the session UI is still around (a flow left blocked).
func noLeak(t *testing.T) {
	t.Helper()
	for i := 0; i < 200; i++ {
		buf := make([]byte, 1<<20)
		n := runtime.Stack(buf, true)
		if !strings.Contains(string(buf[:n]), "tui.(*sessionUI)") && !strings.Contains(string(buf[:n]), "tui.runSession.func") {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	buf := make([]byte, 1<<20)
	n := runtime.Stack(buf, true)
	t.Fatalf("a flow goroutine is still blocked:\n%s", buf[:n])
}

// A flow through a real tea.Program: keys come from a pipe, the screen goes to a buffer.
func TestSessionRealProgram(t *testing.T) {
	inR, inW := io.Pipe()
	defer inW.Close()
	out := &syncBuf{}
	var yes bool
	done := make(chan struct{})
	var last entry
	var ok bool
	var err error
	go func() {
		defer close(done)
		last, ok, err = runSession(testTheme(), newBase(), nil, sessionOpts{in: inR, out: out, alt: true},
			func(u ui.UI) error {
				if hs, isHS := u.(ui.HeaderSetter); isHS {
					hs.SetHeader(ui.Header{Name: "salon", IP: "192.0.2.41", Firmware: "Akam 1.4.2", Badges: []ui.HeaderBadge{hb("ok", "ready")}})
				}
				u.Banner("Camera options - salon")
				i, perr := u.Pick("Which group?", []string{"Video", "LED"})
				if perr != nil {
					return perr
				}
				u.Say(fmt.Sprintf("picked %d", i))
				yes, _ = u.AskYN("Turn it off?")
				u.Good("LED off")
				return nil
			})
	}()
	waitFor(t, out, "Which group?")
	waitFor(t, out, "salon")
	if _, werr := inW.Write([]byte("2")); werr != nil {
		t.Fatal(werr)
	}
	waitFor(t, out, "Turn it off?")
	if _, werr := inW.Write([]byte("y")); werr != nil {
		t.Fatal(werr)
	}
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("the session did not end")
	}
	if err != nil || !yes || !ok || last.text != "LED off" || last.kind != kGood {
		t.Errorf("err=%v yes=%v last=%+v ok=%v", err, yes, last, ok)
	}
	noLeak(t)
}

// R1/R2: the program ends while the flow is blocked in a question; the flow is released, answered
// No, and no goroutine is left behind.
func TestSessionQuitReleasesBlockedFlow(t *testing.T) {
	inR, inW := io.Pipe()
	defer inW.Close()
	out := &syncBuf{}
	var prog *tea.Program
	var pm sync.Mutex
	started := make(chan struct{})
	var yn, dg bool
	var e1 error
	flowReturned := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		_, _, err := runSession(testTheme(), newBase(), nil, sessionOpts{in: inR, out: out, alt: true, grace: 5 * time.Second,
			hook: func(p *tea.Program) { pm.Lock(); prog = p; pm.Unlock(); close(started) }},
			func(u ui.UI) error {
				yn, e1 = u.AskYN("Blocked here?")
				dg, _ = u.Danger("T", "Sure?", "x")
				close(flowReturned)
				return e1
			})
		done <- err
	}()
	<-started
	waitFor(t, out, "Blocked here?")
	pm.Lock()
	prog.Quit() // the program ends with the question pending
	pm.Unlock()
	select {
	case err := <-done:
		if !errors.Is(err, ui.ErrNoInput) {
			t.Errorf("session error %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("runSession did not return")
	}
	select {
	case <-flowReturned:
	default:
		t.Fatal("the flow was not released")
	}
	if yn || dg || !errors.Is(e1, ui.ErrNoInput) {
		t.Errorf("yn=%v dg=%v e1=%v", yn, dg, e1)
	}
	noLeak(t)
}

// A flow that is busy (not in a question) when the user quits is not waited for beyond the grace.
func TestSessionQuitWithBusyFlow(t *testing.T) {
	inR, inW := io.Pipe()
	defer inW.Close()
	out := &syncBuf{}
	release := make(chan struct{})
	defer close(release)
	done := make(chan error, 1)
	go func() {
		_, _, err := runSession(testTheme(), newBase(), nil, sessionOpts{in: inR, out: out, alt: true, grace: 100 * time.Millisecond},
			func(u ui.UI) error {
				u.Say("working hard")
				<-release
				return nil
			})
		done <- err
	}()
	waitFor(t, out, "working hard")
	inW.Write([]byte{3}) // Ctrl-C: asks the flow to stop
	inW.Write([]byte{3}) // Ctrl-C again: quits
	select {
	case err := <-done:
		if !errors.Is(err, errInterrupted) {
			t.Errorf("err %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the session did not end")
	}
}

// A panic in the flow is raised again once the terminal is restored.
func TestSessionFlowPanic(t *testing.T) {
	inR, inW := io.Pipe()
	defer inW.Close()
	defer func() {
		if r := recover(); r != "boom" {
			t.Errorf("recovered %v", r)
		}
	}()
	_, _, _ = runSession(testTheme(), newBase(), nil, sessionOpts{in: inR, out: &syncBuf{}, alt: true},
		func(u ui.UI) error { panic("boom") })
	t.Error("no panic")
}

// ---------------------------------------------------------------- transcript, strip, header

func TestSessionTranscriptBounded(t *testing.T) {
	m := newSessModel(testTheme(), newSessState())
	for i := 0; i < maxTranscript+500; i++ {
		m.Update(msgLine{entry{kSay, fmt.Sprintf("line %d", i)}})
	}
	if len(m.transcript) != maxTranscript || m.transcript[0].text != "line 500" {
		t.Errorf("%d entries, first %q", len(m.transcript), m.transcript[0].text)
	}
}

// Text from a camera never carries an escape into the transcript, the strip or the screen.
func TestSessionCleansText(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		u.Say("name \x1b]0;pwned\x07 ok")
		u.Warn("a\x1b[2Jb")
		u.Banner("T\x1b[31mitle")
		return nil
	})
	h.finish()
	if v := h.m.View(); strings.ContainsRune(v, '\x1b') || strings.Contains(v, "pwned") {
		t.Errorf("escape on screen: %q", v)
	}
}

// The strip keeps an error over a later success, Say never replaces a result, any key clears it.
func TestSessionStrip(t *testing.T) {
	m := newSessModel(testTheme(), newSessState())
	m.Update(msgLine{entry{kGood, "saved"}})
	m.Update(msgLine{entry{kSay, "details"}})
	if m.strip.kind != kGood {
		t.Errorf("a Say replaced a result: %+v", m.strip)
	}
	m.Update(msgLine{entry{kErr, "refused"}})
	m.Update(msgLine{entry{kGood, "later"}})
	if m.strip.kind != kErr || m.strip.text != "refused" {
		t.Errorf("an error must stay: %+v", m.strip)
	}
	m.Update(key("x"))
	if m.strip != nil {
		t.Error("a key must clear the strip")
	}
	m.Update(msgLine{entry{kSay, "hint"}})
	if m.strip.kind != kSay {
		t.Error("Say fills an empty strip")
	}
}

func TestSessionHeaderBadgesDropFromTheRight(t *testing.T) {
	m := newSessModel(testTheme(), newSessState())
	m.Update(msgHeader{ui.Header{Name: "salon", IP: "192.0.2.41", Firmware: "Akam 1.4.2",
		Badges: []ui.HeaderBadge{hb("ok", "ready"), hb("warn", "clock off"), hb("ok", "-58 dBm")}}})
	cases := map[int][]string{120: {"[ok] ready", "[!] clock off", "[ok] -58 dBm"}, 62: {"[ok] ready"}}
	for w, want := range cases {
		m.Update(tea.WindowSizeMsg{Width: w, Height: 24})
		l2 := m.header(w)[1]
		for _, s := range want {
			if !strings.Contains(l2, s) {
				t.Errorf("w=%d: %q lacks %q", w, l2, s)
			}
		}
		if w == 62 && strings.Contains(l2, "-58") {
			t.Errorf("w=%d: the last badge must drop first: %q", w, l2)
		}
		if !strings.Contains(l2, "salon") || !strings.Contains(l2, "192.0.2.41") {
			t.Errorf("w=%d: name and IP never drop: %q", w, l2)
		}
	}
	// no camera
	m.Update(msgHeader{ui.Header{}})
	if l := m.header(80)[1]; !strings.Contains(l, "no camera") {
		t.Errorf("%q", l)
	}
}

func TestSessionMenuOverlay(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	var idx int
	h.run(func(u ui.UI) error {
		mu := u.(interface {
			MenuWithResult(ui.MenuView) (int, error)
		})
		var err error
		idx, err = mu.MenuWithResult(ui.MenuView{Title: "Akam", Items: menuItems(), Result: ui.MenuResult{Level: "ok", Text: "Backup saved"}})
		return err
	})
	h.ask()
	h.until(func() bool { return h.m.strip != nil })
	if h.m.strip.text != "Backup saved" || h.m.strip.kind != kGood {
		t.Errorf("strip: %+v", h.m.strip)
	}
	h.press("down", "enter")
	h.finish()
	if idx != 2 { // the menu starts on the first todo step (index 1)
		t.Errorf("idx %d", idx)
	}
	h = newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		idx, _ = u.(interface {
			Menu(string, []ui.MenuItem) (int, error)
		}).Menu("Akam", menuItems())
		return nil
	})
	h.ask()
	h.press("q")
	h.finish()
	if idx != -1 {
		t.Errorf("quit must be -1, got %d", idx)
	}
}

// ---------------------------------------------------------------- layout goldens

func layoutModel(w, h int) *sessModel {
	m := newSessModel(testTheme(), newSessState())
	m.Update(tea.WindowSizeMsg{Width: w, Height: h})
	m.Update(msgHeader{ui.Header{Product: "Akam 0.9  linux/amd64", Name: "salon", IP: "192.0.2.41", Firmware: "Akam 1.4.2",
		Badges: []ui.HeaderBadge{hb("ok", "ready"), hb("warn", "clock off"), hb("ok", "-58 dBm")}}})
	return m
}

func checkFits(t *testing.T, name, v string, w, h int) {
	t.Helper()
	lines := strings.Split(v, "\n")
	if len(lines) > h {
		t.Errorf("%s: %d lines in a %d line window", name, len(lines), h)
	}
	for _, l := range lines {
		if lw := lipgloss.Width(l); lw > w {
			t.Errorf("%s: a line is %d columns in a %d column window: %q", name, lw, w, l)
		}
	}
}

func TestSessionLayoutGoldens(t *testing.T) {
	sizes := [][2]int{{80, 24}, {60, 16}, {120, 40}, {50, 12}}
	for _, sz := range sizes {
		w, h := sz[0], sz[1]
		tag := fmt.Sprintf("%dx%d", w, h)

		// a long operation: steps, progress, transcript and a result on the strip
		m := layoutModel(w, h)
		m.Update(msgBanner{"Back up the camera - salon"})
		m.Update(msgBanner{"Step 1/4 - Connect over SSH"})
		m.Update(msgLine{entry{kGood, "Connected"}})
		m.Update(msgBanner{"Step 2/4 - Check free space"})
		m.Update(msgLine{entry{kSay, "12.4 GB free"}})
		m.Update(msgBanner{"Step 3/4 - Dump the flash"})
		m.Update(msgProgress{"mtd2 rootfs", 52})
		m.Update(msgLine{entry{kWarn, "The camera clock is off"}})
		v := m.View()
		checkFits(t, "running "+tag, v, w, h)
		golden(t, "session_running_"+tag, v)

		// a list question over the panel
		m = layoutModel(w, h)
		m.Update(msgLine{entry{kGood, "Backup saved: 3 files, 16.0 MB"}})
		m.Update(msgAsk{newAsk(&Picker{Title: "What do you want to do?", Items: []string{"Set up a new camera", "Update this camera", "Camera options", "Back up / restore"}})})
		v = m.View()
		checkFits(t, "pick "+tag, v, w, h)
		golden(t, "session_pick_"+tag, v)

		// the red box
		m = layoutModel(w, h)
		m.Update(msgAsk{newAsk(&YesNo{Prompt: "Erase the card?", Title: "Erase the SD card", Danger: true, Box: []string{
			"This erases everything on /dev/sdb (Sample 32 GB, 31.9 GB).",
			"The camera is not touched. This cannot be undone.", "", "Command:  mkfs.fat -F32 /dev/sdb1"}})})
		v = m.View()
		checkFits(t, "danger "+tag, v, w, h)
		if !strings.Contains(v, "ERASE THE SD CARD") || !strings.Contains(v, "(*) No") {
			t.Errorf("danger %s: title or default:\n%s", tag, v)
		}
		golden(t, "session_danger_"+tag, v)
	}
	// an empty session, a normal question and a text field, at 80x24
	m := layoutModel(80, 24)
	golden(t, "session_empty_80x24", m.View())
	m.Update(msgAsk{newAsk(&YesNo{Prompt: "Restart the video service?", Frame: true, Title: "Question"})})
	checkFits(t, "askyn", m.View(), 80, 24)
	golden(t, "session_askyn_80x24", m.View())
	m = layoutModel(80, 24)
	m.Update(msgAsk{newAsk(&Input{Prompt: "Camera name [salon]:", Def: "salon"})})
	golden(t, "session_prompt_80x24", m.View())
}

// A resize while a question is open redraws it for the new size.
func TestSessionResize(t *testing.T) {
	m := layoutModel(120, 40)
	m.Update(msgAsk{newAsk(&YesNo{Prompt: "Erase?", Title: "Erase", Danger: true, Box: []string{strings.Repeat("long line ", 12)}})})
	for _, sz := range [][2]int{{120, 40}, {70, 20}, {45, 10}, {120, 40}} {
		m.Update(tea.WindowSizeMsg{Width: sz[0], Height: sz[1]})
		checkFits(t, fmt.Sprintf("resize %v", sz), m.View(), sz[0], sz[1])
	}
}

func hb(level, text string) ui.HeaderBadge { return ui.HeaderBadge{Level: level, Text: text} }
