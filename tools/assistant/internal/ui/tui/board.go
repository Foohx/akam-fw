package tui

import (
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The board (ui.Boarder) is the persistent list of the home screen and of the camera chooser. It is
// part of the session model and owned by the program goroutine; the flow replaces its content with
// messages and receives the keys through a channel.

// boardEv is a key of the board. It carries the ID of the row it was pressed on, not only its index:
// the rows may change between the key and the moment the flow reads it.
type boardEv struct {
	key string
	row int
	id  string
}

type (
	msgBoardSet   struct{ b ui.Board }
	msgBoardClose struct{}
)

// boardView is the board on screen plus the state that belongs to the screen, not to the flow.
type boardView struct {
	b      ui.Board
	cursor int
	help   bool
	top    int
}

var _ ui.Boarder = (*sessionUI)(nil)

func cleanBoard(b ui.Board) ui.Board {
	c := textsafe.Clean
	out := b
	out.Title, out.Empty, out.Status = c(b.Title), c(b.Empty), c(b.Status)
	out.Rows = make([]ui.BoardRow, len(b.Rows))
	for i, r := range b.Rows {
		r.Label, r.Detail, r.Note, r.Badge.Text = c(r.Label), c(r.Detail), c(r.Note), c(r.Badge.Text)
		out.Rows[i] = r
	}
	out.Help = make([]string, len(b.Help))
	for i, l := range b.Help {
		out.Help[i] = c(l)
	}
	out.Keys = append([]ui.BoardKey(nil), b.Keys...)
	return out
}

func (s *sessionUI) SetBoard(b ui.Board) {
	s.st.rearm() // a board is where an action ends and the next one starts
	s.st.writing.Store(false)
	s.st.send(msgBoardSet{cleanBoard(b)})
}

func (s *sessionUI) CloseBoard() {
	s.st.send(msgBoardClose{})
	for { // keys pressed for the screen that is gone
		select {
		case <-s.st.boardKeys:
		default:
			return
		}
	}
}

func (s *sessionUI) BoardKey() (string, int, error) { return s.WaitBoardKey(nil) }

func (s *sessionUI) WaitBoardKey(stop <-chan struct{}) (string, int, error) {
	select {
	case <-stop:
		return "", -1, nil
	case ev := <-s.st.boardKeys:
		return ev.key, s.st.rowOf(ev), nil
	case <-s.st.done:
		return "", -1, ui.ErrNoInput
	}
}

// setBoard replaces the content; the cursor stays on the same row ID when it still exists.
func (m *sessModel) setBoard(b ui.Board) {
	if m.board == nil {
		m.board = &boardView{}
	}
	bv := m.board
	id := ""
	if bv.cursor >= 0 && bv.cursor < len(bv.b.Rows) {
		id = bv.b.Rows[bv.cursor].ID
	}
	bv.b = b
	ids := make([]string, len(b.Rows))
	for i, r := range b.Rows {
		ids[i] = r.ID
	}
	m.st.setBoardIDs(ids)
	if id != "" {
		for i, r := range b.Rows {
			if r.ID == id {
				bv.cursor = i
				return
			}
		}
	}
	bv.cursor = min(bv.cursor, max(len(b.Rows)-1, 0))
}

func (m *sessModel) boardSend(key string) {
	row, id := -1, ""
	if m.board != nil && len(m.board.b.Rows) > 0 {
		row = m.board.cursor
		id = m.board.b.Rows[row].ID
	}
	select {
	case m.st.boardKeys <- boardEv{key, row, id}:
	default: // nobody is reading: drop it
	}
}

// boardKey handles a key while the board is shown and no question is asked.
func (m *sessModel) boardKey(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	bv := m.board
	rows := bv.b.Rows
	s := k.String()
	switch s {
	case "up", "k":
		if bv.cursor > 0 {
			bv.cursor--
		}
		return m, nil
	case "down", "j":
		if bv.cursor < len(rows)-1 {
			bv.cursor++
		}
		return m, nil
	case "ctrl+c", "esc":
		m.boardSend("esc")
		return m, nil
	case "enter":
		if len(rows) > 0 {
			m.boardSend("enter")
		}
		return m, nil
	case "?":
		if len(m.transcript) > 0 { // the output of the last action stays readable from the home
			m.openLastOutput()
			return m, nil
		}
		if len(bv.b.Help) > 0 {
			bv.help = !bv.help
		}
		return m, nil
	}
	for i, r := range rows {
		if r.Hot != "" && r.Hot == s {
			bv.cursor = i
			m.boardSend("enter")
			return m, nil
		}
	}
	for _, bk := range bv.b.Keys {
		if strings.ToLower(bk.Key) == s {
			m.boardSend(s)
			return m, nil
		}
	}
	return m, nil
}

// openLastOutput opens the "?" pane over the board with what the last action printed (from its first
// line, like a report), then the help of the screen.
func (m *sessModel) openLastOutput() {
	text := m.transcriptText()
	if h := m.board.b.Help; len(h) > 0 {
		text += "\n" + strings.Join(h, "\n")
	}
	m.pane = &Details{Title: "Last action", Text: text, Theme: m.th}
	m.pane.Update(tea.WindowSizeMsg{Width: m.w, Height: m.panelRows()})
}

// boardFooter draws the key hints; on a narrow window "up/down move" goes first, then "? details",
// then the last keys, so the keys that act (and Esc) stay.
func (m *sessModel) boardFooter() string {
	th, bv := m.th, m.board
	var keys []string
	for _, k := range bv.b.Keys {
		if k.Word != "" {
			keys = append(keys, k.Key, k.Word)
		}
	}
	more := []string{}
	if len(bv.b.Help) > 0 || len(m.transcript) > 0 {
		more = []string{"?", "details"}
	}
	var move []string
	if len(bv.b.Rows) > 1 {
		move = []string{th.Glyphs.UpDown, "move"}
	}
	cat := func(parts ...[]string) []string {
		var out []string
		for _, p := range parts {
			out = append(out, p...)
		}
		return out
	}
	fits := func(p []string) bool { return lipgloss.Width(th.Footer(p...))+2 <= m.w }
	for _, p := range [][]string{cat(move, keys, more), cat(keys, more), cat(keys)} {
		if fits(p) {
			return " " + th.Footer(p...)
		}
	}
	p := keys
	for len(p) > 2 && !fits(p) {
		p = p[:len(p)-2]
	}
	return " " + th.Footer(p...)
}

func (m *sessModel) badgeOf(b ui.HeaderBadge) string {
	if b.Text == "" {
		return ""
	}
	return m.th.Badge(badgeKind(b.Level), b.Text)
}

// boardLines draws the board in exactly rows lines of w columns.
func (m *sessModel) boardLines(rows, w int) []string {
	th, bv := m.th, m.board
	b := bv.b
	var head, foot []string
	if b.Title != "" {
		head = append(head, " "+th.Bold.Render(truncW(b.Title, max(w-2, 8), th.Glyphs.Ellipsis)))
	}
	if b.Status != "" {
		mark := th.Badge(badgeKind(b.StatusLevel), "")
		foot = append(foot, " "+mark+" "+truncW(b.Status, max(w-6, 8), th.Glyphs.Ellipsis))
	}
	if bv.help {
		for _, l := range b.Help {
			for _, x := range wrapW(l, max(w-4, 8)) {
				foot = append(foot, "  "+th.Muted.Render(x))
			}
		}
	}
	avail := max(rows-len(head)-len(foot), 1)
	var body []string
	if len(b.Rows) == 0 {
		if b.Empty != "" {
			body = append(body, "  "+th.Muted.Render(truncW(b.Empty, max(w-4, 8), th.Glyphs.Ellipsis)))
		}
	} else {
		body = m.boardRows(avail, w)
	}
	out := append(head, body...)
	for len(out) < rows-len(foot) {
		out = append(out, "")
	}
	out = append(out, foot...)
	if len(out) > rows {
		out = out[:rows]
	}
	return out
}

// boardRows draws the rows with aligned columns, scrolled so the cursor is visible.
func (m *sessModel) boardRows(avail, w int) []string {
	th, bv := m.th, m.board
	b := bv.b
	if bv.cursor < bv.top {
		bv.top = bv.cursor
	}
	if bv.cursor >= bv.top+avail {
		bv.top = bv.cursor - avail + 1
	}
	lw, dw, bw := 0, 0, 0
	badges := make([]string, len(b.Rows))
	for i, r := range b.Rows {
		lw = max(lw, lipgloss.Width(r.Label))
		dw = max(dw, lipgloss.Width(r.Detail))
		badges[i] = m.badgeOf(r.Badge)
		bw = max(bw, lipgloss.Width(badges[i]))
	}
	lw = min(lw, max((w-4)/3, 8))
	var out []string
	for i := bv.top; i < len(b.Rows) && i < bv.top+avail; i++ {
		r := b.Rows[i]
		prefix := "   "
		label := pad(truncW(r.Label, lw, th.Glyphs.Ellipsis), lw)
		if i == bv.cursor {
			prefix = " " + th.Sel.Render(th.Glyphs.Marker) + " "
			label = th.Sel.Render(label)
		}
		line := prefix + label
		left := 3 + lw
		if dw > 0 {
			maxD := max(w-left-3, 4) // the home screen has no other column: the detail takes the rest
			if bw > 0 {
				maxD = min(dw, max((w-left)/3, 8))
			}
			d := truncW(r.Detail, maxD, th.Glyphs.Ellipsis)
			shown := min(dw, maxD)
			padn := max(shown-lipgloss.Width(d), 0)
			if b.DetailMuted {
				d = th.Muted.Render(d)
			}
			line += "  " + d + strings.Repeat(" ", padn)
			left += 2 + shown
		}
		if bw > 0 {
			line += "  " + pad(badges[i], bw)
			left += 2 + bw
		}
		if r.Note != "" {
			line += "  " + th.Muted.Render(truncW(r.Note, max(w-left-3, 4), th.Glyphs.Ellipsis))
		}
		out = append(out, line)
	}
	return out
}
