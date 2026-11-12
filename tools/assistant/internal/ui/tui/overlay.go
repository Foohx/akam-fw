package tui

import (
	"strings"

	tea "github.com/charmbracelet/bubbletea"
)

// An overlay is a question the flow goroutine is blocked on: one of the widget models drawn over the
// main panel of the session. The models are the same as the inline ones (YesNo, Input, Picker, Menu),
// so their safety behaviour (the default is No, Esc, q and Ctrl-C answer No) is the same code.

// answer is what an overlay gives back to the blocked flow.
type answer struct {
	yes       bool
	idx       int
	text      string
	cancelled bool
}

// ask is one pending question. ch has room for the answer, so answering never blocks the program.
type ask struct {
	model tea.Model
	ch    chan answer
}

func newAsk(m tea.Model) *ask { return &ask{model: m, ch: make(chan answer, 1)} }

// finished returns the answer once the model is done.
func (a *ask) finished() (answer, bool) {
	switch m := a.model.(type) {
	case *YesNo:
		return answer{yes: m.Done && m.Result}, m.Done
	case *Input:
		return answer{text: m.Value(), cancelled: m.Cancelled}, m.Done
	case *Picker:
		return answer{idx: m.Cursor, cancelled: m.Cancelled}, m.Done
	case *Menu:
		return answer{idx: m.Cursor, cancelled: m.Quit || !m.Done}, m.Done
	case *Form:
		return answer{}, m.Done
	case *Details:
		return answer{}, m.Done
	case *Checklist:
		return answer{cancelled: m.Cancelled}, m.Done
	case *ErrorView:
		return answer{text: m.Key}, m.Done
	}
	return answer{}, false
}

// body is the model drawn without its key hints, and keys are the hints for the layout footer.
func (a *ask) body() string {
	switch m := a.model.(type) {
	case *YesNo:
		return m.body()
	case *Input:
		return m.body()
	case *Picker:
		return m.body()
	case *Menu:
		return m.body()
	case *Form:
		return m.body()
	case *Details:
		return m.body()
	case *Checklist:
		return m.body()
	case *ErrorView:
		return m.body()
	}
	return ""
}

func (a *ask) keys() []string {
	switch m := a.model.(type) {
	case *YesNo:
		return m.keys()
	case *Input:
		return m.keys()
	case *Picker:
		return m.keys()
	case *Menu:
		return m.keys()
	case *Form:
		return m.keys()
	case *Details:
		return m.keys()
	case *Checklist:
		return m.keys()
	case *ErrorView:
		return m.keys()
	}
	return nil
}

// update gives a message to the model. The models of the inline widgets end with a tea.Quit the
// session does not use; only a Form has commands of its own (its debounce timer and its apply).
func (a *ask) update(msg tea.Msg) tea.Cmd {
	var c tea.Cmd
	a.model, c = a.model.Update(msg)
	if _, ok := a.model.(*Form); ok {
		return c
	}
	return nil
}

// resize tells the model the size of the main panel it is drawn in.
func (a *ask) resize(w, rows int) {
	h := rows
	switch a.model.(type) {
	case *Picker, *Menu:
		h = rows + 1 // these count their footer line in the window height; the layout draws it
	}
	a.model, _ = a.model.Update(tea.WindowSizeMsg{Width: w, Height: h})
}

// lines is the body split into lines, without trailing empty ones.
func (a *ask) lines() []string {
	return strings.Split(strings.TrimRight(a.body(), "\n"), "\n")
}
