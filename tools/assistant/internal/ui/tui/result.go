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
	th := r.Menu.Theme
	if th == nil {
		th = theme()
	}
	kind, st := BadgeErr, th.Err
	switch r.res.Level {
	case "ok":
		kind, st = BadgeOK, th.OK
	case "warn":
		kind, st = BadgeWarn, th.Warn
	}
	text := r.res.Text
	if r.Menu.W > 8 {
		text = truncW(text, max(r.Menu.W-6, 8), th.Glyphs.Ellipsis)
	}
	line := "  " + th.Badge(kind, "") + " " + st.Render(text)
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
