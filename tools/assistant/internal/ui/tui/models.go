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

// ---------------------------------------------------------------- yes / no

// YesNo is the one yes/no question of the wizard: the default is No. y answers yes; n, q, Esc and
// Ctrl-C answer no; Enter takes the highlighted choice (No at the start); arrows and Tab move it.
type YesNo struct {
	Prompt string
	Box    []string // lines of the warning box (danger)
	Title  string
	Danger bool
	Theme  *Theme // nil: the package theme (SetTheme)
	W      int    // window width (WindowSizeMsg); 0 means 80

	yes    bool // highlighted choice
	Done   bool
	Result bool
}

func (m *YesNo) Init() tea.Cmd { return nil }

func (m *YesNo) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W = v.Width
		return m, nil
	}
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
	th := m.Theme
	if th == nil {
		th = theme()
	}
	yes, no := th.Glyphs.RadioOff+" Yes", th.Glyphs.RadioOff+" No"
	if m.yes {
		yes = th.Glyphs.RadioOn + " Yes"
	} else {
		no = th.Glyphs.RadioOn + " No"
	}
	radios := yes + "   " + no // measured unstyled: the styling adds no column
	if m.yes {
		yes = th.Sel.Render(yes)
	} else {
		no = th.Sel.Render(no)
	}
	styled := yes + "   " + no
	footer := "  " + th.Footer("y", "yes", "n/Enter/Esc/q", "no") + "\n"
	var b strings.Builder
	if !m.Danger {
		fmt.Fprintf(&b, "%s\n  %s\n%s", th.Bold.Render(m.Prompt), styled, footer)
		return b.String()
	}
	outer := m.W
	if outer <= 0 {
		outer = 80
	}
	outer = max(min(outer, 100), 24)
	inner := outer - 4
	var lines []string
	lines = append(lines, "")
	for _, l := range m.Box {
		lines = append(lines, wrapW(l, inner)...)
	}
	lines = append(lines, "")
	if q := m.Prompt; lipgloss.Width(q)+3+lipgloss.Width(radios) <= inner {
		lines = append(lines, q+"   "+styled)
	} else {
		for _, l := range wrapW(q, inner) {
			lines = append(lines, l)
		}
		lines = append(lines, styled)
	}
	b.WriteString(th.Box(th.Err, strings.ToUpper(truncW(m.Title, inner-3, th.Glyphs.Ellipsis)), lines, outer))
	b.WriteString(footer)
	return b.String()
}

// ---------------------------------------------------------------- text input

// Input reads one line; Secret hides it (nothing is echoed, not even stars). Esc cancels.
type Input struct {
	Prompt string
	Def    string
	Secret bool
	Theme  *Theme

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
	th := m.Theme
	if th == nil {
		th = theme()
	}
	// the prompt text already shows its default
	return fmt.Sprintf("%s %s\n  %s\n", th.Bold.Render(m.Prompt), shown, th.Footer("Enter", "ok", "Esc", "cancel"))
}

// ---------------------------------------------------------------- picker

// Picker chooses one item: arrows or j/k, Enter, a digit, Esc or q to cancel.
type Picker struct {
	Title string
	Items []string
	Theme *Theme

	Cursor    int
	Done      bool
	Cancelled bool
	W         int // window width; 0 means 80
}

func (m *Picker) Init() tea.Cmd { return nil }

func (m *Picker) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W = v.Width
		return m, nil
	}
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
	th := m.Theme
	if th == nil {
		th = theme()
	}
	w := m.W
	if w <= 0 {
		w = 80
	}
	var b strings.Builder
	b.WriteString(th.Bold.Render(m.Title) + "\n")
	for i, it := range m.Items {
		num := fmt.Sprintf("%d)", i+1)
		it = truncW(it, max(w-len(num)-5, 8), th.Glyphs.Ellipsis)
		if i == m.Cursor {
			b.WriteString(" " + th.Sel.Render(th.Glyphs.Marker+" "+num+" "+it) + "\n")
		} else {
			b.WriteString("   " + th.Muted.Render(num) + " " + it + "\n")
		}
	}
	b.WriteString("  " + th.Footer(th.Glyphs.UpDown, "move", "Enter", "choose", "1-9", "jump", "Esc", "cancel") + "\n")
	return b.String()
}

// ---------------------------------------------------------------- main menu

// Menu is the full-screen step menu with status (done / todo / blocked / stale), like tui_menu.sh:
// arrows or j/k, Enter runs, ? shows what the selected line does, q quits.
type Menu struct {
	Title string
	Items []ui.MenuItem
	Theme *Theme

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
		default:
			for i, it := range m.Items {
				if it.Key != "" && it.Key == v.String() {
					m.Cursor, m.Done = i, true
					return m, tea.Quit
				}
			}
		}
	}
	return m, nil
}

// statusBadge is the badge of a step status; ok is false for a line without one (tool, plain).
func (m *Menu) statusBadge(th *Theme, s string) (string, bool) {
	switch s {
	case "done":
		return th.Badge(BadgeOK, "done"), true
	case "stale":
		return th.Badge(BadgeWarn, "stale"), true
	case "blocked":
		return th.Badge(BadgeMute, "blocked"), true
	case "tool", "plain":
		return "", false
	}
	return th.Badge(BadgeInfo, "todo"), true
}

func (m *Menu) View() string {
	th := m.Theme
	if th == nil {
		th = theme()
	}
	h := m.H
	if h < 8 {
		h = 8
	}
	rows := h - 6 - strings.Count(m.Title, "\n") // title (several lines for the home header), rule, detail (2), blank, footer
	if m.Cursor < m.top {
		m.top = m.Cursor
	}
	if m.Cursor >= m.top+rows {
		m.top = m.Cursor - rows + 1
	}
	var b strings.Builder
	tl := strings.Split(m.Title, "\n")
	for i, l := range tl {
		if len(tl) > 1 && i == 0 {
			b.WriteString(th.Muted.Render(l) + "\n") // product line of the home header
		} else {
			b.WriteString(th.Bold.Render(l) + "\n")
		}
	}
	b.WriteString(th.Rule(min(m.W, 78)) + "\n")
	tagged := false
	for _, it := range m.Items {
		if _, ok := m.statusBadge(th, it.Status); ok {
			tagged = true
		}
	}
	tagW := 0
	if tagged {
		tagW = th.BadgeWidth(len("blocked")) + 2
	}
	for i := m.top; i < len(m.Items) && i < m.top+rows; i++ {
		it := m.Items[i]
		prefix := "   "
		if i == m.Cursor {
			prefix = " " + th.Sel.Render(th.Glyphs.Marker) + " "
		}
		tag := ""
		if tagged {
			bd, _ := m.statusBadge(th, it.Status)
			tag = pad(bd, tagW)
		}
		label := truncW(it.Label, max(m.W-3-tagW-1, 8), th.Glyphs.Ellipsis)
		if i == m.Cursor {
			label = th.Sel.Render(label)
		}
		b.WriteString(prefix + tag + label + "\n")
	}
	for i := len(m.Items) - m.top; i < rows; i++ {
		b.WriteString("\n")
	}
	b.WriteString("\n")
	if m.Cursor < len(m.Items) {
		d := m.Items[m.Cursor].Detail
		if !m.Help && m.W > 8 {
			d = truncW(d, m.W-4, th.Glyphs.Ellipsis)
		}
		b.WriteString("  " + d + "\n")
	}
	if m.Help {
		b.WriteString("  " + th.Muted.Render("Enter runs the line. Every action shows what it will do first and asks one yes/no (default No).") + "\n")
	} else {
		b.WriteString("\n")
	}
	b.WriteString("  " + th.Footer(th.Glyphs.UpDown, "move", "Enter", "run", "?", "help", "q", "quit") + "\n")
	return b.String()
}
