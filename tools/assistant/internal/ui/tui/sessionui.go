package tui

import (
	"fmt"
	"io"
	"runtime/debug"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// FlowPanic is what runSession panics with when the flow panicked: the value and the stack of the flow
// goroutine, taken where it happened.
type FlowPanic struct {
	Value any
	Stack []byte
}

func (p *FlowPanic) Error() string {
	return fmt.Sprintf("%v\n\ngoroutine stack of the flow:\n%s", p.Value, p.Stack)
}

// Unwrap lets errors.As find an error the flow panicked with.
func (p *FlowPanic) Unwrap() error {
	e, _ := p.Value.(error)
	return e
}

// errInterrupted is returned by a session the user left while the flow was still busy.
var errInterrupted = ui.ErrInterrupted

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

	failMu sync.Mutex
	fail   *ui.FailInfo // the last Fail, until it was shown or cleared

	resMu   sync.Mutex
	resLast ui.MenuResult // the last Good, Warn or Err of the action running
	resErr  ui.MenuResult // the last Err: it stays the result over a later success
}

var (
	_ ui.UI           = (*sessionUI)(nil)
	_ ui.HeaderSetter = (*sessionUI)(nil)
	_ ui.Former       = (*sessionUI)(nil)
	_ ui.Detailer     = (*sessionUI)(nil)
	_ ui.Checklister  = (*sessionUI)(nil)

	_ ui.ErrorScreener = (*sessionUI)(nil)
	_ ui.FailReader    = (*sessionUI)(nil)
	_ ui.TechFailer    = (*sessionUI)(nil)
	_ ui.OutputClearer = (*sessionUI)(nil)
)

func (s *sessionUI) log(format string, a ...any) {
	if s.logf != nil {
		s.logf(format, a...)
	}
}

func (s *sessionUI) line(k lineKind, text string) {
	text = textsafe.Clean(text)
	s.st.send(msgLine{entry{k, text}})
	level := ""
	switch k {
	case kGood:
		level = "ok"
	case kWarn:
		level = "warn"
	case kErr:
		level = "error"
	default:
		return
	}
	if t := oneLine(text); t != "" {
		res := ui.MenuResult{Level: level, Text: t}
		s.resMu.Lock()
		s.resLast = res
		if level == "error" {
			s.resErr = res
		}
		s.resMu.Unlock()
	}
}

var (
	_ ui.ActionResulter = (*sessionUI)(nil)
	_ ui.Stopper        = (*sessionUI)(nil)
)

// StopChan is closed when the user asked the current action to stop (ui.Stopper).
func (s *sessionUI) StopChan() <-chan struct{} { return s.st.stopChan() }

// ActionResult is the outcome of the action running since the last ClearOutput (ui.ActionResulter).
func (s *sessionUI) ActionResult() ui.MenuResult {
	s.resMu.Lock()
	defer s.resMu.Unlock()
	if s.resErr.Text != "" {
		return s.resErr
	}
	return s.resLast
}

func (s *sessionUI) Say(t string)  { t = textsafe.Clean(t); s.log("SAY %s", t); s.line(kSay, t) }
func (s *sessionUI) Bold(t string) { s.line(kBold, t) }
func (s *sessionUI) Good(t string) { s.line(kGood, t) }
func (s *sessionUI) Warn(t string) { t = textsafe.Clean(t); s.log("WARN %s", t); s.line(kWarn, t) }
func (s *sessionUI) Err(t string)  { t = textsafe.Clean(t); s.log("ERROR %s", t); s.line(kErr, t) }
func (s *sessionUI) Hint(t string) { s.line(kHint, t) }
func (s *sessionUI) Blank()        { s.line(kBlank, "") }

func (s *sessionUI) Fail(msg, hint string) error { return s.FailTech(msg, hint, "") }

// FailTech is Fail with a technical line for the error screen (ui.TechFailer). The transcript and the
// log get the same lines as before; the screen is shown by whoever called the flow.
func (s *sessionUI) FailTech(msg, hint, tech string) error {
	s.Err(msg)
	if hint != "" {
		s.Hint(hint)
	}
	s.failMu.Lock()
	s.fail = &ui.FailInfo{Msg: textsafe.Clean(msg), Hint: textsafe.Clean(hint), Tech: textsafe.Clean(tech)}
	s.failMu.Unlock()
	return ui.ErrFailed
}

// LastFail is the last Fail not shown yet (ui.FailReader).
func (s *sessionUI) LastFail() (ui.FailInfo, bool) {
	s.failMu.Lock()
	defer s.failMu.Unlock()
	if s.fail == nil {
		return ui.FailInfo{}, false
	}
	return *s.fail, true
}

func (s *sessionUI) ClearFail() {
	s.failMu.Lock()
	s.fail = nil
	s.failMu.Unlock()
}

// ClearOutput empties the transcript, the steps and the strip of the previous action
// (ui.OutputClearer); the flow calls it as an action starts.
func (s *sessionUI) ClearOutput() {
	s.ClearFail()
	s.resMu.Lock()
	s.resLast, s.resErr = ui.MenuResult{}, ui.MenuResult{}
	s.resMu.Unlock()
	s.st.rearm() // synchronously: the next question of this action must not see the old stop
	s.st.wrote.Store(false)
	s.st.writing.Store(false) // one flow goroutine runs the actions one after the other: the last one is over
	s.st.send(msgClear{})
}

// ErrorScreen shows the error screen and returns the key chosen (ui.ErrorScreener). Once the user is
// back, the title stays on the result strip of the screen below.
func (s *sessionUI) ErrorScreen(spec ui.ErrorSpec) (string, error) {
	c := textsafe.Clean
	spec.Context, spec.Title, spec.Tech, spec.Help, spec.Log = c(spec.Context), c(spec.Title), c(spec.Tech), c(spec.Help), c(spec.Log)
	spec.Body = append([]string(nil), spec.Body...)
	spec.Todo = append([]string(nil), spec.Todo...)
	for i := range spec.Body {
		spec.Body[i] = c(spec.Body[i])
	}
	for i := range spec.Todo {
		spec.Todo[i] = c(spec.Todo[i])
	}
	a, err := s.ask(&ErrorView{Spec: spec})
	if err != nil {
		return "", err
	}
	s.st.send(msgStripIfEmpty{entry{kErr, spec.Title}})
	return a.text, nil
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

// cleanRow makes the text of a row safe to draw (it comes from the camera). A secret row never
// carries a value.
func cleanRow(r ui.FormRow) ui.FormRow {
	r.ID = textsafe.Clean(r.ID)
	r.Group = textsafe.Clean(r.Group)
	r.Label = textsafe.Clean(r.Label)
	r.Value = textsafe.Clean(r.Value)
	r.Note = textsafe.Clean(r.Note)
	r.Detail = textsafe.Clean(r.Detail)
	cs := make([]string, len(r.Choices))
	for i, c := range r.Choices {
		cs[i] = textsafe.Clean(c)
	}
	r.Choices = cs
	if r.Secret {
		r.Value = ""
	}
	return r
}

// Form edits rows in place (ui.Former). It returns when the user leaves the form, after the pending
// changes were applied. apply runs off the screen goroutine; a panic in it is an error of that row.
func (s *sessionUI) Form(title string, rows []ui.FormRow, apply func(id, value string) (ui.FormRow, error)) error {
	_, err := s.FormKeys(ui.FormSpec{Title: title, Rows: rows, Apply: apply})
	return err
}

// FormKeys is Form with keys (ui.FormKeyer): pressing one applies what is pending, closes the form and
// reports the key and the group shown.
func (s *sessionUI) FormKeys(spec ui.FormSpec) (ui.FormResult, error) {
	clean := make([]ui.FormRow, len(spec.Rows))
	for i, r := range spec.Rows {
		clean[i] = cleanRow(r)
	}
	apply := spec.Apply
	safe := func(id, value string) (row ui.FormRow, err error) {
		defer func() {
			if p := recover(); p != nil {
				err = fmt.Errorf("internal error: %v", p)
			}
		}()
		row, err = apply(id, value)
		return cleanRow(row), err
	}
	if apply == nil {
		safe = nil
	}
	keys := make([]ui.FormKey, len(spec.Keys))
	for i, k := range spec.Keys {
		keys[i] = ui.FormKey{Key: textsafe.Clean(k.Key), Label: textsafe.Clean(k.Label)}
	}
	f := &Form{Title: textsafe.Clean(spec.Title), Rows: clean, Apply: safe, Keys: keys, StartGroup: textsafe.Clean(spec.Group)}
	_, err := s.ask(f)
	// An Apply still running (the form was left with a second Esc, or the program ended) must return
	// before the flow goes on: the flow may close the connection it uses or read the options again.
	wd := make(chan struct{})
	go func() { f.pending.Wait(); close(wd) }()
	select {
	case <-wd:
	case <-s.st.done: // the program is gone: wait for a running call, but not for ever
		select {
		case <-wd:
		case <-time.After(graceDefault):
		}
	}
	return ui.FormResult{Key: f.Key, Group: f.ShownGroup()}, err
}

// Details shows a long text in a scrollable pane (ui.Detailer).
func (s *sessionUI) Details(title, text string) {
	s.ask(&Details{Title: textsafe.Clean(title), Text: textsafe.Clean(text)}) //nolint:errcheck // nothing to report
}

// Checklist shows physical-action steps; Space or Enter acknowledges them all (ui.Checklister).
func (s *sessionUI) Checklist(title string, steps []string) error {
	clean := make([]string, len(steps))
	for i, st := range steps {
		clean[i] = textsafe.Clean(st)
	}
	a, err := s.ask(&Checklist{Title: textsafe.Clean(title), Steps: clean})
	if err != nil {
		return err
	}
	if a.cancelled {
		return ui.ErrNoInput
	}
	return nil
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
			if r := recover(); r != nil {
				// The stack is taken here, where the panic is still on it: raised again from runSession it
				// would show the wrong goroutine.
				flowPanic = &FlowPanic{Value: r, Stack: debug.Stack()}
			}
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
wait:
	for {
		select {
		case <-flowDone:
			break wait
		case <-timer.C:
			if st.writing.Load() {
				// A write to the camera is under way: leaving now would cut it short and could leave the
				// camera half written. Wait for the flow without limit.
				timer.Reset(grace)
				continue
			}
			interrupted = true
			break wait
		}
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

// RunSession is the full-screen session on the given streams instead of the terminal: fn runs against
// the real session UI, the program reads its keys from in and draws on out (no alternate screen). It
// exists for tests of the flows, which must see the same ui.UI the terminal gives them; the
// terminal path is (*UI).Session.
func RunSession(base ui.UI, in io.Reader, out io.Writer, fn func(ui.UI) error) error {
	_, _, err := runSession(theme(), base, nil, sessionOpts{in: in, out: out}, fn)
	return err
}
