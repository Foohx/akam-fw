package cli_test

import (
	"bytes"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// sessionUI is a plain UI that also has a session, like the TUI.
type sessionPlain struct {
	*ui.Plain
	sessions *int
	picks    *int
}

func (s sessionPlain) Session(fn func(ui.UI) error) error {
	*s.sessions++
	return fn(countingPick{s.Plain, s.picks})
}

// countingPick is the UI the flow gets inside the session: it is not a Sessioner itself.
type countingPick struct {
	*ui.Plain
	n *int
}

func (c countingPick) Pick(title string, items []string) (int, error) {
	*c.n++
	return c.Plain.Pick(title, items)
}

// The step menu runs inside the session when the UI has one, with the session's UI as a.UI, and
// does not open a second session from there.
func TestStepMenuRunsInSession(t *testing.T) {
	e := newEnv(t)
	var sessions, picks int
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		return sessionPlain{cli.NewPlainUI(o, st, log, secrets, dry), &sessions, &picks}
	}
	r := runWith(e, h, "", append(e.base(), "steps")...)
	expectRC(t, r, 0)
	if sessions != 1 || picks != 1 {
		t.Errorf("sessions=%d picks=%d: the menu must run once, inside one session", sessions, picks)
	}
}

func runWith(e *env, h cli.Hooks, stdin string, argv ...string) result {
	var out, errb bytes.Buffer
	rc := cli.Run(argv, cli.Streams{In: strings.NewReader(stdin), Out: &out, Err: &errb}, "test", h)
	return result{out.String(), errb.String(), rc}
}
