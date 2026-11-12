package tui

import (
	"errors"
	"fmt"
	"io"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// errInterrupted is returned by a session the user left while the flow was still busy.
var errInterrupted = errors.New("interrupted")

// graceDefault is how long Session waits for the flow after the program ended (a flow blocked in a
// question is released at once; one busy in a long operation is not waited for any longer).
const graceDefault = 3 * time.Second

// sessionUI is the ui.UI handed to the flow goroutine. Every method sends a message to the program;
// questions block the goroutine on a reply channel and also select on the end of the program, so a
// quit always releases the flow (R2 of the spec) and a pending question is answered No.
type sessionUI struct {
	base ui.UI // Interactive, Dry
	st   *sessState
	logf func(format string, a ...any)
}

var (
	_ ui.UI           = (*sessionUI)(nil)
	_ ui.HeaderSetter = (*sessionUI)(nil)
)

func (s *sessionUI) log(format string, a ...any) {
	if s.logf != nil {
		s.logf(format, a...)
	}
}

func (s *sessionUI) line(k lineKind, text string) {
	s.st.send(msgLine{entry{k, textsafe.Clean(text)}})
}

func (s *sessionUI) Say(t string)  { t = textsafe.Clean(t); s.log("SAY %s", t); s.line(kSay, t) }
func (s *sessionUI) Bold(t string) { s.line(kBold, t) }
func (s *sessionUI) Good(t string) { s.line(kGood, t) }
func (s *sessionUI) Warn(t string) { t = textsafe.Clean(t); s.log("WARN %s", t); s.line(kWarn, t) }
func (s *sessionUI) Err(t string)  { t = textsafe.Clean(t); s.log("ERROR %s", t); s.line(kErr, t) }
func (s *sessionUI) Hint(t string) { s.line(kHint, t) }
func (s *sessionUI) Blank()        { s.line(kBlank, "") }

func (s *sessionUI) Fail(msg, hint string) error {
	s.Err(msg)
	if hint != "" {
		s.Hint(hint)
	}
	return ui.ErrFailed
}

func (s *sessionUI) Banner(title string) {
	s.st.send(msgBanner{textsafe.Clean(title)})
}

func (s *sessionUI) Progress(label string, pct int) {
	s.st.send(msgProgress{textsafe.Clean(label), pct})
}

func (s *sessionUI) ProgressClose() { s.st.send(msgProgressClose{}) }

func (s *sessionUI) Dry() bool         { return s.base.Dry() }
func (s *sessionUI) Interactive() bool { return s.base.Interactive() }

func (s *sessionUI) RequireTTY(action string) error {
	if s.Dry() || s.Interactive() {
		return nil
	}
	s.Err(action + " needs an interactive terminal and refuses to run without one.")
	s.Hint("run the command from a terminal, or use --dry-run to rehearse.")
	return ui.ErrFailed
}

// SetHeader replaces the header of the session (ui.HeaderSetter).
func (s *sessionUI) SetHeader(h ui.Header) { s.st.send(msgHeader{h}) }

// ask shows the model as an overlay and blocks until it is answered, the user asked the flow to stop
// (a new question is then not asked at all) or the program ended; the last two answer "no".
func (s *sessionUI) ask(m tea.Model) (answer, error) {
	if s.st.stopped() {
		return answer{}, ui.ErrNoInput
	}
	q := newAsk(m)
	s.st.send(msgAsk{q})
	select {
	case a := <-q.ch:
		return a, nil
	case <-s.st.done:
		select { // an answer given just before the end still counts
		case a := <-q.ch:
			return a, nil
		default:
		}
		return answer{}, ui.ErrNoInput
	}
}

func (s *sessionUI) AskYN(prompt string) (bool, error) {
	a, err := s.ask(&YesNo{Prompt: textsafe.Clean(prompt), Frame: true, Title: "Question"})
	if err != nil {
		return false, err
	}
	return a.yes, nil
}

// Danger is the red box: No is highlighted, only y (or Yes then Enter) answers yes, nothing times out.
func (s *sessionUI) Danger(title, question string, lines ...string) (bool, error) {
	if s.Dry() {
		s.Say(fmt.Sprintf("WOULD ask: %s [y/N] (default: No) -- %s: %s", question, title, strings.Join(lines, " ")))
		return true, nil
	}
	if err := s.RequireTTY(title); err != nil {
		return false, err
	}
	title = textsafe.Clean(title)
	s.log("WARN %s", title)
	box := make([]string, len(lines))
	for i, l := range lines {
		l = textsafe.Clean(l)
		s.log("SAY   %s", l) // the command and the facts are in the log before the answer
		box[i] = l
	}
	a, err := s.ask(&YesNo{Prompt: textsafe.Clean(question), Title: title, Box: box, Danger: true})
	if err != nil {
		return false, err
	}
	return a.yes, nil
}

func (s *sessionUI) Prompt(prompt, def string) (string, error) {
	a, err := s.ask(&Input{Prompt: textsafe.Clean(prompt), Def: def})
	if err != nil {
		return "", err
	}
	if a.cancelled {
		return "", ui.ErrNoInput
	}
	return a.text, nil
}

// Secret never echoes and never enters the transcript.
func (s *sessionUI) Secret(prompt string) (string, error) {
	a, err := s.ask(&Input{Prompt: textsafe.Clean(prompt), Secret: true})
	if err != nil {
		return "", err
	}
	if a.cancelled {
		return "", ui.ErrNoInput
	}
	return a.text, nil
}

func (s *sessionUI) Pick(title string, items []string) (int, error) {
	shown, hidden := splitBack(items)
	clean := make([]string, len(shown))
	for i, it := range shown {
		clean[i] = textsafe.Clean(it)
	}
	a, err := s.ask(&Picker{Title: textsafe.Clean(title), Items: clean})
	if err != nil {
		return -1, err
	}
	if a.cancelled {
		if hidden >= 0 {
			return hidden, nil // Esc is the hidden Back / Cancel row
		}
		return -1, ui.ErrNoInput
	}
	return a.idx, nil
}

// Menu is the full-screen menu as an overlay (the cli.Menuer interface); -1 means quit.
func (s *sessionUI) Menu(title string, items []ui.MenuItem) (int, error) {
	return s.MenuWithResult(ui.MenuView{Title: title, Items: items})
}

// MenuWithResult is Menu with the outcome of the previous action (cli.ResultMenuer); the strip
// already holds the last result of the session, so it only fills an empty one.
func (s *sessionUI) MenuWithResult(v ui.MenuView) (int, error) {
	if v.Result.Text != "" {
		k := kErr
		switch v.Result.Level {
		case "ok":
			k = kGood
		case "warn":
			k = kWarn
		}
		s.st.send(msgStripIfEmpty{entry{k, textsafe.Clean(v.Result.Text)}})
	}
	a, err := s.ask(NewMenu(v.Title, v.Items))
	if err != nil || a.cancelled {
		return -1, nil
	}
	return a.idx, nil
}

// ---------------------------------------------------------------- running a session

type sessionOpts struct {
	in    io.Reader
	out   io.Writer
	alt   bool
	grace time.Duration
	hook  func(*tea.Program) // tests: gets the program before it runs
}

// runSession runs fn in a goroutine against a sessionUI and the program in the caller's goroutine. It
// returns the last result of the transcript (for the normal screen) and fn's error. A panic of fn is
// raised again here once the terminal is restored.
func runSession(th *Theme, base ui.UI, logf func(string, ...any), o sessionOpts, fn func(ui.UI) error) (entry, bool, error) {
	st := newSessState()
	m := newSessModel(th, st)
	opts := []tea.ProgramOption{tea.WithInput(o.in), tea.WithOutput(o.out)}
	if o.alt {
		opts = append(opts, tea.WithAltScreen())
	}
	p := tea.NewProgram(m, opts...)
	st.send = p.Send
	if o.hook != nil {
		o.hook(p)
	}
	sui := &sessionUI{base: base, st: st, logf: logf}

	flowDone := make(chan struct{})
	var flowErr error
	var flowPanic any
	go func() {
		defer close(flowDone)
		defer func() {
			flowPanic = recover()
			p.Send(msgFlowDone{}) // returns at once when the program has ended
		}()
		flowErr = fn(sui)
	}()

	_, runErr := p.Run()
	close(st.done) // releases every question the flow is blocked on
	grace := o.grace
	if grace <= 0 {
		grace = graceDefault
	}
	timer := time.NewTimer(grace)
	defer timer.Stop()
	interrupted := false
	select {
	case <-flowDone:
	case <-timer.C:
		interrupted = true
	}
	last, ok := m.lastResult()
	switch {
	case interrupted:
		return last, ok, errInterrupted
	case flowPanic != nil:
		panic(flowPanic)
	case runErr != nil:
		return last, ok, fmt.Errorf("session: %w", runErr)
	}
	return last, ok, flowErr
}

// Session shows the full-screen session and runs fn in a goroutine whose ui.UI renders inside it
// (the cli.Sessioner interface). Without a terminal, or in a rehearsal, fn runs against the plain
// behaviour of this UI. On the way out the last result is printed once on the normal screen.
func (u *UI) Session(fn func(ui.UI) error) error {
	if !u.widgets() || u.DryRun {
		return fn(u)
	}
	var logf func(string, ...any)
	if u.Log != nil {
		logf = u.Log.Line
	}
	last, ok, err := runSession(theme(), u, logf, sessionOpts{in: u.InFile, out: u.ErrFile, alt: true}, fn)
	if ok {
		switch last.kind {
		case kGood:
			fmt.Fprintln(u.Out, last.text)
		case kWarn:
			fmt.Fprintln(u.Out, "WARNING: "+last.text)
		case kErr:
			fmt.Fprintln(u.ErrOut, "ERROR: "+last.text)
		}
	}
	return err
}
