package tui

import (
	"strings"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// resultMenu is the full-screen menu with the outcome of the previous action on a strip above the
// footer (it replaces the "Press Enter to go back" stop). It stays until the user leaves the menu.
type resultMenu struct {
	*Menu
	res ui.MenuResult
}

func (r resultMenu) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	_, cmd := r.Menu.Update(msg)
	return r, cmd
}

func (r resultMenu) View() string {
	v := r.Menu.View()
	if r.res.Text == "" {
		return v
	}
	tag := map[string]string{"ok": "[ok] ", "warn": "[!] ", "error": "[x] "}[r.res.Level]
	line := "  " + tag + r.res.Text
	if r.Menu.W > 8 && len(line) > r.Menu.W-1 {
		line = line[:r.Menu.W-4] + "..."
	}
	switch r.res.Level {
	case "ok":
		line = styGreen.Render(line)
	case "warn":
		line = styYellow.Render(line)
	default:
		line = styRed.Render(line)
	}
	body := strings.TrimRight(v, "\n")
	i := strings.LastIndex(body, "\n")
	if i < 0 {
		return v
	}
	return body[:i] + "\n" + line + body[i:] + "\n"
}

// MenuWithResult is Menu with a result strip (the cli.ResultMenuer interface).
func (u *UI) MenuWithResult(v ui.MenuView) (int, error) {
	m := NewMenu(v.Title, v.Items)
	if err := u.run(resultMenu{m, v.Result}, true); err != nil {
		return -1, err
	}
	if m.Quit || !m.Done {
		return -1, nil
	}
	return m.Cursor, nil
}
