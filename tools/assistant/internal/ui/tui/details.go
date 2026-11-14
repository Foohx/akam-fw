package tui

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
)

// Details is the "?" pane (specs/installer.md): a long text in a scrollable window. Up/Down (j/k) move by a
// line, PgUp/PgDn (Space) by a page, Home/End (g/G) jump. Esc, q, ? and Enter close it.
type Details struct {
	Title string
	Text  string
	Theme *Theme
	W, H  int // size of the area it is drawn in; 0 means 80 columns and 24 lines
	// AtEnd starts the view on the last lines (a transcript is read from its end).
	AtEnd bool

	Done bool
	top  int
	init bool // the view was positioned
	rows []string
}

func (m *Details) Init() tea.Cmd { return nil }

func (m *Details) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Details) size() (w, h int) {
	w, h = m.W, m.H
	if w <= 0 {
		w = 80
	}
	if h <= 0 {
		h = 24
	}
	return w, h
}

// lines is the text wrapped to the width, computed again when the width changed.
func (m *Details) lines(w int) []string {
	var out []string
	for _, raw := range strings.Split(strings.TrimRight(m.Text, "\n"), "\n") {
		if strings.TrimSpace(raw) == "" {
			out = append(out, "")
			continue
		}
		indent := raw[:len(raw)-len(strings.TrimLeft(raw, " "))]
		for _, l := range wrapW(strings.TrimLeft(raw, " "), max(w-2-len(indent), 8)) {
			out = append(out, indent+l)
		}
	}
	return out
}

// view is the number of text lines shown: the height minus the title and the position line.
func (m *Details) view() int {
	_, h := m.size()
	return max(h-2, 1)
}

func (m *Details) clampTop(n int) {
	m.top = min(max(m.top, 0), max(n-m.view(), 0))
}

func (m *Details) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W, m.H = v.Width, v.Height
		m.init = false
		return m, nil
	}
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	w, _ := m.size()
	n := len(m.lines(w))
	m.position(n)
	page := max(m.view()-1, 1)
	switch k.String() {
	case "esc", "q", "?", "enter", "ctrl+c":
		m.Done = true
		return m, tea.Quit
	case "up", "k":
		m.top--
	case "down", "j":
		m.top++
	case "pgup", "b":
		m.top -= page
	case "pgdown", " ", "space":
		m.top += page
	case "home", "g":
		m.top = 0
	case "end", "G":
		m.top = n
	}
	m.clampTop(n)
	return m, nil
}

// position puts the view on its first line, or on the end for a transcript, once per size.
func (m *Details) position(n int) {
	if m.init {
		return
	}
	m.init = true
	if m.AtEnd {
		m.top = n
	}
	m.clampTop(n)
}

func (m *Details) keys() []string {
	return []string{"up/down", "scroll", "PgUp/PgDn", "page", "Esc/?", "close"}
}

func (m *Details) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

// body draws the title, the visible lines and, when the text is longer than the window, the position.
func (m *Details) body() string {
	th := m.themed()
	w, _ := m.size()
	all := m.lines(w)
	m.position(len(all))
	m.clampTop(len(all))
	var b strings.Builder
	b.WriteString(" " + th.Bold.Render(truncW(m.Title, max(w-2, 8), th.Glyphs.Ellipsis)) + "\n")
	end := min(m.top+m.view(), len(all))
	for _, l := range all[m.top:end] {
		b.WriteString("  " + l + "\n")
	}
	if len(all) > m.view() {
		for i := end - m.top; i < m.view(); i++ {
			b.WriteString("\n")
		}
		b.WriteString("  " + th.Muted.Render(fmt.Sprintf("lines %d-%d of %d", m.top+1, end, len(all))) + "\n")
	}
	return b.String()
}
