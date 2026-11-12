package tui

import (
	"strings"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// ErrorView is the error screen (spec 12, 4.7): a red title, what happened, what to do, a muted
// technical line. r retries (when offered), l shows the log tail in a pane, c asks for another camera,
// Esc goes back. It is an overlay of the session, like the questions.
type ErrorView struct {
	Spec  ui.ErrorSpec
	Theme *Theme
	W, H  int

	Key  string // the ui.ErrKey* chosen
	Done bool
	log  *Details
}

func (m *ErrorView) Init() tea.Cmd { return nil }

func (m *ErrorView) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *ErrorView) size() (int, int) {
	w, h := m.W, m.H
	if w <= 0 {
		w = 80
	}
	if h <= 0 {
		h = 24
	}
	return w, h
}

func (m *ErrorView) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W, m.H = v.Width, v.Height
		if m.log != nil {
			m.log.Update(v)
		}
		return m, nil
	}
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	if m.log != nil { // the log pane closes with Esc, q, ? or Enter and returns to the screen
		m.log.Update(k)
		if m.log.Done {
			m.log = nil
		}
		return m, nil
	}
	switch k.String() {
	case "r":
		if m.Spec.Retry {
			m.Key, m.Done = ui.ErrKeyRetry, true
		}
	case "l":
		w, h := m.size()
		m.log = &Details{Title: "Log (last lines)", Text: m.Spec.Log, Theme: m.themed(), AtEnd: true}
		m.log.Update(tea.WindowSizeMsg{Width: w, Height: h})
	case "c":
		m.Key, m.Done = ui.ErrKeyCamera, true
	case "esc", "ctrl+c":
		m.Key, m.Done = ui.ErrKeyBack, true
	}
	return m, nil
}

func (m *ErrorView) keys() []string {
	if m.log != nil {
		return m.log.keys()
	}
	var k []string
	if m.Spec.Retry {
		k = append(k, "r", "retry")
	}
	if w, _ := m.size(); w < 60 { // a narrow window gets the short words
		return append(k, "l", "log", "c", "camera", "Esc", "back")
	}
	return append(k, "l", "show the log", "c", "other camera", "Esc", "back")
}

func (m *ErrorView) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

// errLayout is one way to draw the screen, from the fullest to the barest; the first that fits the
// panel is used.
type errLayout struct {
	context, blanks, help bool
	bodyMax, todoMax      int
}

func (m *ErrorView) build(o errLayout) []string {
	th, s := m.themed(), m.Spec
	w, _ := m.size()
	var out []string
	blank := func() {
		if o.blanks {
			out = append(out, "")
		}
	}
	if o.context && s.Context != "" {
		out = append(out, " "+th.Bold.Render(truncW(s.Context, max(w-2, 8), th.Glyphs.Ellipsis)))
		blank()
	}
	for i, l := range wrapW(s.Title, max(w-8, 8)) {
		if i == 0 {
			out = append(out, "  "+th.Badge(BadgeErr, "")+" "+th.Err.Render(l))
		} else {
			out = append(out, "      "+th.Err.Render(l))
		}
	}
	if len(s.Body) > 0 && o.bodyMax > 0 {
		blank()
		for i, b := range s.Body {
			if i >= o.bodyMax {
				break
			}
			for _, l := range wrapW(b, max(w-8, 8)) {
				out = append(out, "      "+l)
			}
		}
	}
	if len(s.Todo) > 0 {
		blank()
		out = append(out, "      "+th.Bold.Render("What to do"))
		for i, t := range s.Todo {
			if i >= o.todoMax {
				break
			}
			for j, l := range wrapW(t, max(w-12, 8)) {
				if j == 0 {
					out = append(out, "        "+th.Glyphs.Bullet+" "+l)
				} else {
					out = append(out, "          "+l)
				}
			}
		}
	}
	if s.Tech != "" || (s.Help != "" && o.help) {
		blank()
	}
	if s.Tech != "" {
		for _, l := range wrapW(s.Tech, max(w-8, 8)) {
			out = append(out, "      "+th.Muted.Render(l))
		}
	}
	if s.Help != "" && o.help {
		for _, l := range wrapW(s.Help, max(w-8, 8)) {
			out = append(out, "      "+th.Muted.Render(l))
		}
	}
	return out
}

// body draws the screen in the panel, or the log pane while it is open.
func (m *ErrorView) body() string {
	if m.log != nil {
		return m.log.body()
	}
	_, h := m.size()
	layouts := []errLayout{
		{true, true, true, 9, 9}, {true, false, true, 9, 9}, {false, false, true, 9, 9}, {false, false, false, 9, 9},
		{false, false, false, 1, 9}, {false, false, false, 1, 3}, {false, false, false, 0, 2},
	}
	var lines []string
	for _, o := range layouts {
		if lines = m.build(o); len(lines) <= h {
			break
		}
	}
	if len(lines) > h {
		lines = lines[:h]
	}
	return strings.Join(lines, "\n") + "\n"
}
