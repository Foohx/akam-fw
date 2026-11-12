package cli

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"unicode"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The error screen of the session (spec 12, 4.7). A flow still ends with Fail(msg, hint); the session
// UI remembers it and, once the flow has returned to the home loop, the loop shows it as a screen: a
// title, what happened, what to do, a muted technical line, with r retry, l log, c other camera, Esc.
// Without a session (plain, one-shot commands) nothing here runs and Fail prints as before.

// failTech is Fail with a technical line the error screen shows muted; plain and the inline widgets
// just call Fail.
func (a *App) failTech(msg, hint, tech string) error {
	if tf, ok := a.UI.(ui.TechFailer); ok {
		return tf.FailTech(msg, hint, tech)
	}
	return a.UI.Fail(msg, hint)
}

// clearOutput starts an action on a clean panel (the transcript of the previous one is dropped).
func (a *App) clearOutput() {
	if c, ok := a.UI.(ui.OutputClearer); ok {
		c.ClearOutput()
	}
}

// sentences splits a text at ". " followed by a capital letter, keeping the full stops.
func sentences(s string) []string {
	s = strings.Join(strings.Fields(s), " ")
	var out []string
	start := 0
	rs := []rune(s)
	for i := 0; i+2 < len(rs); i++ {
		if rs[i] == '.' && rs[i+1] == ' ' && unicode.IsUpper(rs[i+2]) {
			out = append(out, string(rs[start:i+1]))
			start = i + 2
		}
	}
	if start < len(rs) {
		out = append(out, string(rs[start:]))
	}
	return out
}

func capitalize(s string) string {
	rs := []rune(s)
	if len(rs) > 0 {
		rs[0] = unicode.ToUpper(rs[0])
	}
	return string(rs)
}

// errorSpec turns what a flow passed to Fail into the words of the screen: the first sentence is the
// title, the others say what happened, the hint becomes the "What to do" list.
func (a *App) errorSpec(context string, f ui.FailInfo, retry bool) ui.ErrorSpec {
	sp := ui.ErrorSpec{Context: context, Tech: f.Tech, Retry: retry, Log: a.logTail(40)}
	ss := sentences(f.Msg)
	if len(ss) == 0 {
		ss = []string{"The action failed."}
	}
	sp.Title = capitalize(ss[0])
	for _, s := range ss[1:] {
		sp.Body = append(sp.Body, s)
	}
	for _, s := range sentences(f.Hint) {
		for _, t := range strings.Split(s, "; ") {
			t = strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(t), "."))
			switch {
			case t == "":
			case strings.Contains(t, "lines above"):
				sp.Todo = append(sp.Todo, "press l to read what the tool did")
			default:
				sp.Todo = append(sp.Todo, t)
			}
		}
	}
	if len(sp.Todo) == 0 {
		sp.Todo = []string{"press l to read what the tool did"}
	}
	sp.Help = fmt.Sprintf("More help: l shows the log; %s doctor checks this computer.", Prog)
	return sp
}

// logTail is the last n lines of the log file (the log redacts secrets when it writes).
func (a *App) logTail(n int) string {
	p := a.Log.Path()
	if p == "" {
		return "No log file."
	}
	f, err := os.Open(p)
	if err != nil {
		return "The log file cannot be read."
	}
	defer f.Close()
	if st, err := f.Stat(); err == nil && st.Size() > 32<<10 {
		f.Seek(-(32 << 10), io.SeekEnd) //nolint:errcheck // a short read is fine
	}
	b, _ := io.ReadAll(f)
	lines := strings.Split(strings.TrimRight(string(b), "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

// actionContext is the title line of the error screen: the flow and the chosen camera.
func (a *App) actionContext(label string) string {
	if ip := a.camAddr(); ip != "" && a.Cams != nil {
		return fmt.Sprintf("%s - %s  %s", label, a.Cams.Name, ip)
	}
	return label
}

// runScreen runs one action of the session. When it fails and the UI has an error screen, the screen
// is shown: r runs the action again (on a clean panel), c opens the camera chooser, Esc returns. It
// returns the key chosen ("retry" only happens inside the loop). Anything not shown as a screen is
// reported as before.
func (a *App) runScreen(label string, act func() error) {
	for {
		e := act()
		if e == nil {
			return
		}
		key, shown := a.failureScreen(label, e)
		if !shown {
			a.reportFlowError(e)
			return
		}
		switch key {
		case ui.ErrKeyRetry:
			a.clearOutput()
			continue
		case ui.ErrKeyCamera:
			a.clearOutput()
			a.reportFlowError(a.homeAction("camera"))
		}
		return
	}
}

// failureScreen shows the error screen for e when the UI can and there is something to say; shown is
// false for a question the user answered by stopping (nothing failed) and for a UI without a screen.
func (a *App) failureScreen(label string, e error) (key string, shown bool) {
	es, ok := a.UI.(ui.ErrorScreener)
	if !ok || e == nil || errors.Is(e, ui.ErrNoInput) {
		return "", false
	}
	info := ui.FailInfo{Msg: e.Error()}
	var ue *ui.UsageError
	switch {
	case errors.Is(e, ui.ErrFailed):
		fr, ok := a.UI.(ui.FailReader)
		if !ok {
			return "", false
		}
		f, have := fr.LastFail()
		if !have {
			return "", false // the flow showed its own message
		}
		info = f
	case asUsage(e, &ue):
		info = ui.FailInfo{Msg: ue.Msg}
	}
	if fr, ok := a.UI.(ui.FailReader); ok {
		fr.ClearFail()
	}
	k, err := es.ErrorScreen(a.errorSpec(a.actionContext(label), info, true))
	if err != nil {
		return "", true // the session ended
	}
	return k, true
}
