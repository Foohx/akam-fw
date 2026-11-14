package tui

import (
	"strconv"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
)

// Checklist is a list of physical actions (unplug, take the card out, plug back in) the user does by
// hand (specs/installer.md). One key acknowledges them all: Space or Enter. q, Esc and Ctrl-C
// stop. The acknowledgement only means "go on and check": it authorises nothing destructive.
type Checklist struct {
	Title string
	Steps []string
	Theme *Theme
	W     int // window width; 0 means 80

	Done      bool
	Cancelled bool
}

func (m *Checklist) Init() tea.Cmd { return nil }

func (m *Checklist) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Checklist) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W = v.Width
		return m, nil
	}
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	switch k.String() {
	case "enter", " ", "space":
		m.Done = true
		return m, tea.Quit
	case "esc", "q", "ctrl+c":
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	}
	return m, nil
}

func (m *Checklist) keys() []string {
	return []string{"Space/Enter", "done", "q/Esc", "stop"}
}

func (m *Checklist) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

func (m *Checklist) body() string {
	th := m.themed()
	w := m.W
	if w <= 0 {
		w = 80
	}
	var b strings.Builder
	b.WriteString(" " + th.Bold.Render(truncW(m.Title, max(w-2, 8), th.Glyphs.Ellipsis)) + "\n\n")
	for i, s := range m.Steps {
		prefix := "[ ] " + strconv.Itoa(i+1) + ". "
		for j, l := range wrapW(s, max(w-4-len(prefix), 8)) {
			if j == 0 {
				b.WriteString("  " + prefix + l + "\n")
			} else {
				b.WriteString("  " + strings.Repeat(" ", len(prefix)) + l + "\n")
			}
		}
	}
	return b.String()
}
