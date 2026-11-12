package cli

import (
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// ResultMenuer is the optional extension of Menuer: the menu also shows the outcome of the previous
// action on a strip above its footer, so no "Press Enter" stop is needed after an action.
type ResultMenuer interface {
	MenuWithResult(v ui.MenuView) (int, error)
}

// recorder wraps the UI while one menu action runs and remembers its last Good/Warn/Err/Fail line
// (an error stays the result of the action over a later success line).
type recorder struct {
	ui.UI
	last ui.MenuResult
	err  ui.MenuResult
}

func oneLine(s string) string { return strings.Join(strings.Fields(s), " ") }

func (r *recorder) note(level, s string) {
	res := ui.MenuResult{Level: level, Text: oneLine(s)}
	if res.Text == "" {
		return
	}
	r.last = res
	if level == "error" {
		r.err = res
	}
}

func (r *recorder) Good(s string) { r.note("ok", s); r.UI.Good(s) }
func (r *recorder) Warn(s string) { r.note("warn", s); r.UI.Warn(s) }
func (r *recorder) Err(s string)  { r.note("error", s); r.UI.Err(s) }
func (r *recorder) Fail(msg, hint string) error {
	r.note("error", msg)
	return r.UI.Fail(msg, hint)
}

func (r *recorder) result() ui.MenuResult {
	if r.err.Text != "" {
		return r.err
	}
	return r.last
}

// runAction runs one menu action with its outcome recorded, and returns it for the next menu.
func (a *App) runAction(act func() error) ui.MenuResult {
	inner := a.UI
	rec := &recorder{UI: inner}
	a.UI = rec
	defer func() { a.UI = inner }()
	e := act()
	a.reportFlowError(e)
	return rec.result()
}

// showMenu shows the menu, with the previous result when the UI can.
func (a *App) showMenu(m Menuer, title string, items []ui.MenuItem, res ui.MenuResult) (int, error) {
	if rm, ok := m.(ResultMenuer); ok {
		return rm.MenuWithResult(ui.MenuView{Title: title, Items: items, Result: res})
	}
	return m.Menu(title, items)
}
