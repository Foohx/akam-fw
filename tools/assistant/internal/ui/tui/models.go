// Package tui is the bubbletea user interface: a full-screen menu and inline widgets (yes/no with
// the default on No, text and hidden input, picker). Commands never import it: they only see
// ui.UI, and the models here are plain bubbletea models that tests drive with key messages.
package tui

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

var (
	styBold   = lipgloss.NewStyle().Bold(true)
	styDim    = lipgloss.NewStyle().Faint(true)
	styRed    = lipgloss.NewStyle().Foreground(lipgloss.Color("1")).Bold(true)
	styGreen  = lipgloss.NewStyle().Foreground(lipgloss.Color("2"))
	styYellow = lipgloss.NewStyle().Foreground(lipgloss.Color("3"))
	stySel    = lipgloss.NewStyle().Reverse(true)
)

// ---------------------------------------------------------------- yes / no

// YesNo is the one yes/no question of the wizard: the default is No. y answers yes; n, q, Esc and
// Ctrl-C answer no; Enter takes the highlighted choice (No at the start); arrows and Tab move it.
type YesNo struct {
	Prompt string
	Box    []string // lines of the warning box (danger)
	Title  string
	Danger bool

	yes    bool // highlighted choice
	Done   bool
	Result bool
}

func (m *YesNo) Init() tea.Cmd { return nil }

func (m *YesNo) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	switch k.String() {
	case "y", "Y":
		m.Done, m.Result = true, true
		return m, tea.Quit
	case "n", "N", "q", "Q", "esc", "ctrl+c":
		m.Done, m.Result = true, false
		return m, tea.Quit
	case "enter":
		m.Done, m.Result = true, m.yes
		return m, tea.Quit
	case "left", "h", "shift+tab":
		m.yes = true
	case "right", "l", "tab":
		m.yes = false
	}
	return m, nil
}

func (m *YesNo) View() string {
	var b strings.Builder
	if m.Danger {
		w := len(m.Title) + 2
		for _, l := range m.Box {
			if len(l)+2 > w {
				w = len(l) + 2
			}
		}
		b.WriteString(styRed.Render("+"+strings.Repeat("-", w)+"+") + "\n")
		b.WriteString(styRed.Render("| "+pad(m.Title, w-1)+"|") + "\n")
		for _, l := range m.Box {
			b.WriteString(styRed.Render("| "+pad(l, w-1)+"|") + "\n")
		}
		b.WriteString(styRed.Render("+"+strings.Repeat("-", w)+"+") + "\n")
	}
	yes, no := "( ) Yes", "( ) No"
	if m.yes {
		yes = stySel.Render("(*) Yes")
	} else {
		no = stySel.Render("(*) No")
	}
	fmt.Fprintf(&b, "%s\n  %s  %s   %s\n", m.Prompt, yes, no, styDim.Render("(y/N: Enter, Esc and q mean No)"))
	return b.String()
}

func pad(s string, n int) string {
	if len(s) >= n {
		return s
	}
	return s + strings.Repeat(" ", n-len(s))
}

// ---------------------------------------------------------------- text input

// Input reads one line; Secret hides it (nothing is echoed, not even stars). Esc cancels.
type Input struct {
	Prompt string
	Def    string
	Secret bool

	val       []rune
	Done      bool
	Cancelled bool
}

func (m *Input) Init() tea.Cmd { return nil }

func (m *Input) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	switch k.Type {
	case tea.KeyEnter:
		m.Done = true
		return m, tea.Quit
	case tea.KeyEsc, tea.KeyCtrlC:
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	case tea.KeyBackspace, tea.KeyCtrlH:
		if len(m.val) > 0 {
			m.val = m.val[:len(m.val)-1]
		}
	case tea.KeyCtrlU:
		m.val = nil
	case tea.KeyRunes, tea.KeySpace:
		m.val = append(m.val, k.Runes...)
		if k.Type == tea.KeySpace && len(k.Runes) == 0 {
			m.val = append(m.val, ' ')
		}
	}
	return m, nil
}

// Value is what was typed (the default when nothing was).
func (m *Input) Value() string {
	if len(m.val) == 0 {
		return m.Def
	}
	return string(m.val)
}

func (m *Input) View() string {
	shown := string(m.val)
	if m.Secret {
		shown = ""
	}
	return fmt.Sprintf("%s %s\n", m.Prompt, shown) // the prompt text already shows its default
}

// ---------------------------------------------------------------- picker

// Picker chooses one item: arrows or j/k, Enter, a digit, Esc or q to cancel.
type Picker struct {
	Title string
	Items []string

	Cursor    int
	Done      bool
	Cancelled bool
}

func (m *Picker) Init() tea.Cmd { return nil }

func (m *Picker) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	switch s := k.String(); s {
	case "up", "k":
		if m.Cursor > 0 {
			m.Cursor--
		}
	case "down", "j":
		if m.Cursor < len(m.Items)-1 {
			m.Cursor++
		}
	case "enter":
		m.Done = true
		return m, tea.Quit
	case "esc", "q", "ctrl+c":
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	default:
		if len(s) == 1 && s[0] >= '1' && s[0] <= '9' && int(s[0]-'1') < len(m.Items) {
			m.Cursor = int(s[0] - '1')
			m.Done = true
			return m, tea.Quit
		}
	}
	return m, nil
}

func (m *Picker) View() string {
	var b strings.Builder
	b.WriteString(styBold.Render(m.Title) + "\n")
	for i, it := range m.Items {
		line := fmt.Sprintf("  %d) %s", i+1, it)
		if i == m.Cursor {
			line = stySel.Render("> " + line[2:])
		}
		b.WriteString(line + "\n")
	}
	b.WriteString(styDim.Render("  up/down or j/k, Enter, or the number; Esc cancels") + "\n")
	return b.String()
}

// ---------------------------------------------------------------- main menu

// Menu is the full-screen step menu with status (done / todo / blocked / stale), like tui_menu.sh:
// arrows or j/k, Enter runs, ? shows what the selected line does, q quits.
type Menu struct {
	Title string
	Items []ui.MenuItem

	Cursor int
	Help   bool
	Done   bool
	Quit   bool
	W, H   int
	top    int
}

// NewMenu starts the menu on the first step that is not done.
func NewMenu(title string, items []ui.MenuItem) *Menu {
	m := &Menu{Title: title, Items: items, W: 80, H: 24}
	for i, it := range items {
		if it.Status == "todo" || it.Status == "stale" {
			m.Cursor = i
			break
		}
	}
	return m
}

func (m *Menu) Init() tea.Cmd { return nil }

func (m *Menu) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.W, m.H = v.Width, v.Height
	case tea.KeyMsg:
		switch v.String() {
		case "up", "k":
			if m.Cursor > 0 {
				m.Cursor--
			}
		case "down", "j":
			if m.Cursor < len(m.Items)-1 {
				m.Cursor++
			}
		case "home", "g":
			m.Cursor = 0
		case "end", "G":
			m.Cursor = len(m.Items) - 1
		case "?":
			m.Help = !m.Help
		case "enter":
			m.Done = true
			return m, tea.Quit
		case "q", "esc", "ctrl+c":
			m.Done, m.Quit = true, true
			return m, tea.Quit
		}
	}
	return m, nil
}

func statusTag(s string) string {
	switch s {
	case "done":
		return styGreen.Render("done   ")
	case "blocked":
		return styDim.Render("blocked")
	case "stale":
		return styYellow.Render("stale  ")
	case "tool":
		return "tool   "
	}
	return "todo   "
}

func (m *Menu) View() string {
	h := m.H
	if h < 8 {
		h = 8
	}
	rows := h - 6 // title, rule, detail (2), blank, footer
	if m.Cursor < m.top {
		m.top = m.Cursor
	}
	if m.Cursor >= m.top+rows {
		m.top = m.Cursor - rows + 1
	}
	var b strings.Builder
	b.WriteString(styBold.Render(m.Title) + "\n")
	b.WriteString(strings.Repeat("-", min(m.W, 78)) + "\n")
	for i := m.top; i < len(m.Items) && i < m.top+rows; i++ {
		it := m.Items[i]
		line := fmt.Sprintf("  %s  %s", statusTag(it.Status), it.Label)
		if i == m.Cursor {
			line = stySel.Render(">" + line[1:])
		}
		b.WriteString(line + "\n")
	}
	for i := len(m.Items) - m.top; i < rows; i++ {
		b.WriteString("\n")
	}
	b.WriteString("\n")
	if m.Cursor < len(m.Items) {
		d := m.Items[m.Cursor].Detail
		if !m.Help && len(d) > m.W-4 && m.W > 8 {
			d = d[:m.W-7] + "..."
		}
		b.WriteString("  " + d + "\n")
	}
	if m.Help {
		b.WriteString(styDim.Render("  Enter runs the line. Every action shows what it will do first and asks one yes/no (default No).") + "\n")
	} else {
		b.WriteString("\n")
	}
	b.WriteString(styDim.Render("  up/down or j/k move, Enter runs, ? help, q quits") + "\n")
	return b.String()
}
