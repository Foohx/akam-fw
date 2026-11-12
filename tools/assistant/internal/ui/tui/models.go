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

func (m *YesNo) keys() []string { return []string{"y", "yes", "n/Enter/Esc/q", "no"} }

func (m *YesNo) View() string {
	th := m.themed()
	return m.body() + "  " + th.Footer(m.keys()...) + "\n"
}

func (m *YesNo) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

// body is the question without its key hints (the session layout draws the footer itself).
func (m *YesNo) body() string {
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
	var b strings.Builder
	if !m.Danger {
		fmt.Fprintf(&b, "%s\n  %s\n", th.Bold.Render(m.Prompt), styled)
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
	return b.String()
}

// ---------------------------------------------------------------- text input

// Input reads one line; Secret hides it (nothing is echoed, not even stars). Esc cancels. The default
// is shown as ghost text until something is typed; Left, Right, Home, End, Delete and Backspace edit.
type Input struct {
	Prompt string
	Def    string
	Secret bool
	Theme  *Theme
	W      int // window width (WindowSizeMsg); 0 means no limit

	val       []rune
	pos       int // cursor position in val
	Done      bool
	Cancelled bool
}

func (m *Input) Init() tea.Cmd { return nil }

func (m *Input) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W = v.Width
		return m, nil
	}
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
		if m.pos > 0 {
			m.val = append(m.val[:m.pos-1], m.val[m.pos:]...)
			m.pos--
		}
	case tea.KeyDelete:
		if m.pos < len(m.val) {
			m.val = append(m.val[:m.pos], m.val[m.pos+1:]...)
		}
	case tea.KeyLeft:
		if m.pos > 0 {
			m.pos--
		}
	case tea.KeyRight:
		if m.pos < len(m.val) {
			m.pos++
		}
	case tea.KeyHome, tea.KeyCtrlA:
		m.pos = 0
	case tea.KeyEnd, tea.KeyCtrlE:
		m.pos = len(m.val)
	case tea.KeyCtrlU:
		m.val, m.pos = nil, 0
	case tea.KeyRunes, tea.KeySpace:
		rs := k.Runes
		if k.Type == tea.KeySpace && len(rs) == 0 {
			rs = []rune{' '}
		}
		tail := append([]rune(nil), m.val[m.pos:]...)
		m.val = append(append(m.val[:m.pos], rs...), tail...)
		m.pos += len(rs)
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

func (m *Input) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Input) keys() []string { return []string{"Enter", "ok", "Esc", "cancel"} }

func (m *Input) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

// field draws the text with the cursor, or the default as ghost text; width 0 means no limit.
func (m *Input) field(th *Theme, width int) string {
	if m.Secret {
		return th.CursorAt(nil, 0) // nothing typed is ever drawn
	}
	if len(m.val) == 0 {
		g := []rune(m.Def)
		if len(g) == 0 {
			return th.CursorAt(nil, 0)
		}
		ghost := string(g[1:])
		if width > 0 {
			ghost = truncW(ghost, max(width-1, 0), "")
		}
		return th.CursorAt(g[:1], 0) + th.Muted.Render(ghost)
	}
	rs, pos := m.val, m.pos
	if width > 0 {
		// keep the cursor in view: drop leading runes until the part up to the cursor fits
		for len(rs) > 0 && pos > 0 && lipgloss.Width(string(rs[:pos]))+1 > width {
			rs, pos = rs[1:], pos-1
		}
		for len(rs) > pos && lipgloss.Width(string(rs))+1 > width {
			rs = rs[:len(rs)-1]
		}
	}
	return th.CursorAt(rs, pos)
}

func (m *Input) body() string {
	th := m.themed()
	width := 0
	if m.W > 0 {
		width = max(m.W-lipgloss.Width(m.Prompt)-2, 4)
	}
	return th.Bold.Render(m.Prompt) + " " + m.field(th, width) + "\n"
}

// ---------------------------------------------------------------- picker

// maxNumbered is the number of items that get a digit shortcut; a longer list is filtered by typing.
const maxNumbered = 9

// Picker chooses one item: arrows or j/k, Enter, a digit (the first nine items), Esc or q to cancel.
// Past nine items, typing narrows the list ("/" starts the filter when its first letter is j, k or q).
type Picker struct {
	Title string
	Items []string
	Theme *Theme

	Cursor    int // index in Items
	Done      bool
	Cancelled bool
	W, H      int // window size; 0 means 80 wide and unlimited height

	filter    string
	filtering bool
	top       int
}

func (m *Picker) Init() tea.Cmd { return nil }

func (m *Picker) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Picker) filterable() bool { return len(m.Items) > maxNumbered }

// visible is the indexes of the items that match the filter.
func (m *Picker) visible() []int {
	var out []int
	f := strings.ToLower(m.filter)
	for i, it := range m.Items {
		if f == "" || strings.Contains(strings.ToLower(it), f) {
			out = append(out, i)
		}
	}
	return out
}

func (m *Picker) move(d int) {
	vis := m.visible()
	for p, i := range vis {
		if i == m.Cursor {
			if q := p + d; q >= 0 && q < len(vis) {
				m.Cursor = vis[q]
			}
			return
		}
	}
	if len(vis) > 0 {
		m.Cursor = vis[0]
	}
}

// refilter keeps the cursor on a visible item.
func (m *Picker) refilter() {
	vis := m.visible()
	for _, i := range vis {
		if i == m.Cursor {
			return
		}
	}
	if len(vis) > 0 {
		m.Cursor = vis[0]
	}
	m.top = 0
}

func (m *Picker) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if v, ok := msg.(tea.WindowSizeMsg); ok {
		m.W, m.H = v.Width, v.Height
		return m, nil
	}
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}
	s := k.String()
	switch s {
	case "up":
		m.move(-1)
		return m, nil
	case "down":
		m.move(1)
		return m, nil
	case "enter":
		if len(m.visible()) > 0 {
			m.Done = true
			return m, tea.Quit
		}
		return m, nil
	case "ctrl+c":
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	case "esc":
		if m.filtering || m.filter != "" { // the first Esc clears the filter, the next one cancels
			m.filter, m.filtering = "", false
			m.refilter()
			return m, nil
		}
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	}
	if m.filtering {
		switch k.Type {
		case tea.KeyBackspace, tea.KeyCtrlH:
			if rs := []rune(m.filter); len(rs) > 0 {
				m.filter = string(rs[:len(rs)-1])
			}
			if m.filter == "" {
				m.filtering = false
			}
		case tea.KeyRunes, tea.KeySpace:
			m.filter += string(k.Runes)
			if k.Type == tea.KeySpace && len(k.Runes) == 0 {
				m.filter += " "
			}
		}
		m.refilter()
		return m, nil
	}
	switch s {
	case "k":
		m.move(-1)
	case "j":
		m.move(1)
	case "q":
		m.Done, m.Cancelled = true, true
		return m, tea.Quit
	default:
		if len(s) == 1 && s[0] >= '1' && s[0] <= '9' && int(s[0]-'1') < len(m.Items) {
			m.Cursor = int(s[0] - '1')
			m.Done = true
			return m, tea.Quit
		}
		if m.filterable() && (s == "/" || (k.Type == tea.KeyRunes && len(k.Runes) == 1 && s != "?")) {
			m.filtering = true
			if s != "/" {
				m.filter += s
			}
			m.refilter()
		}
	}
	return m, nil
}

func (m *Picker) keys() []string {
	ud := m.themed().Glyphs.UpDown
	switch {
	case m.filtering || m.filter != "":
		return []string{ud, "move", "Enter", "choose", "Esc", "clear filter"}
	case m.filterable():
		return []string{ud, "move", "Enter", "choose", "1-9", "jump", "type", "filter", "Esc", "cancel"}
	}
	return []string{ud, "move", "Enter", "choose", "1-9", "jump", "Esc", "cancel"}
}

func (m *Picker) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

func (m *Picker) body() string {
	th := m.themed()
	w := m.W
	if w <= 0 {
		w = 80
	}
	title := th.Bold.Render(m.Title)
	if m.filtering || m.filter != "" {
		title += "  " + th.Accent.Render("/") + m.filter + th.CursorAt(nil, 0)
	}
	var b strings.Builder
	b.WriteString(title + "\n")
	vis := m.visible()
	rows := len(vis)
	if m.H > 0 {
		rows = min(rows, max(m.H-2, 1)) // the title and the footer take two lines
	}
	cp := 0
	for p, i := range vis {
		if i == m.Cursor {
			cp = p
		}
	}
	if cp < m.top {
		m.top = cp
	}
	if cp >= m.top+rows {
		m.top = cp - rows + 1
	}
	numbered := !m.filtering && m.filter == ""
	for p := m.top; p < len(vis) && p < m.top+rows; p++ {
		i := vis[p]
		num := "  "
		if numbered && i < maxNumbered {
			num = fmt.Sprintf("%d)", i+1)
		}
		it := truncW(m.Items[i], max(w-len(num)-5, 8), th.Glyphs.Ellipsis)
		if i == m.Cursor {
			b.WriteString(" " + th.Sel.Render(th.Glyphs.Marker+" "+num+" "+it) + "\n")
		} else {
			b.WriteString("   " + th.Muted.Render(num) + " " + it + "\n")
		}
	}
	if len(vis) == 0 {
		b.WriteString("   " + th.Muted.Render("no match") + "\n")
	}
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

func (m *Menu) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Menu) keys() []string {
	return []string{m.themed().Glyphs.UpDown, "move", "Enter", "run", "?", "help", "q", "quit"}
}

func (m *Menu) View() string {
	return m.body() + "  " + m.themed().Footer(m.keys()...) + "\n"
}

// body is the menu without its key hints (the session layout draws the footer itself).
func (m *Menu) body() string {
	th := m.themed()
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
	return b.String()
}
