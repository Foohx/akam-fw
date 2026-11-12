package tui

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// FormDebounce is the pause without a key before an edited row is applied: cycling a value does not
// send every step (spec 12, 4.4).
const FormDebounce = 400 * time.Millisecond

// Form edits the rows of a group of options in place (spec 12, 4.4). Enums are cycled with
// left/right/Space, numbers move by 1 (left/right) or 10 (Shift, PgUp/PgDn) or are typed, texts are
// edited inline. An edited row is applied FormDebounce after the last key by calling Apply off the
// screen goroutine, one call at a time; when Apply refuses, the row snaps back to its real value and
// shows the reason. The model holds no camera or network knowledge: Apply is the only way out.
//
// It reaches its own timer and the result of Apply as messages (formTick, formResult); standalone
// callers and the session both route them to Update.
type Form struct {
	Title string
	Rows  []ui.FormRow
	Apply func(id, value string) (ui.FormRow, error)
	Theme *Theme
	// After returns a command that delivers msg once d has passed. Nil means a real timer; tests
	// inject their own clock.
	After func(d time.Duration, msg tea.Msg) tea.Cmd
	W, H  int // size of the area it is drawn in; 0 means 80 columns and 24 lines

	Done bool

	Keys       []ui.FormKey // one-letter actions: the form closes and reports the key (Key)
	StartGroup string       // the group shown first
	Key        string       // the key that closed the form ("" for Esc)

	real     []string // the last value Apply confirmed (or the initial one) of each row
	rowErr   []string // why the last apply of the row failed
	dirty    []bool   // edited, not yet applied
	focus    int      // index in Rows
	group    int      // index in groups()
	started  bool
	typing   bool   // an int is being typed
	typed    string // the digits
	edit     *Input // a text row is being edited
	inflight int    // row being applied, -1 none
	closing  bool   // Esc pressed: apply what is pending, then close
	gen      int    // debounce generation: only the last tick counts
	want     bool   // a tick came while an apply was running
	help     bool   // "?" shows the detail of the focused row
}

// formMsg is a message that belongs to one Form (a session may hold several).
type formMsg interface{ target() *Form }

type formTick struct {
	f   *Form
	gen int
}

type formResult struct {
	f   *Form
	i   int
	row ui.FormRow
	err error
}

func (m formTick) target() *Form   { return m.f }
func (m formResult) target() *Form { return m.f }

func (m *Form) Init() tea.Cmd { return nil }

func (m *Form) themed() *Theme {
	if m.Theme != nil {
		return m.Theme
	}
	return theme()
}

func (m *Form) size() (w, h int) {
	w, h = m.W, m.H
	if w <= 0 {
		w = 80
	}
	if h <= 0 {
		h = 24
	}
	return w, h
}

// start fills the bookkeeping the first time the form is used (rows may be set by a literal).
func (m *Form) start() {
	if m.started {
		return
	}
	m.started = true
	n := len(m.Rows)
	m.real = make([]string, n)
	m.rowErr = make([]string, n)
	m.dirty = make([]bool, n)
	m.inflight = -1
	for i, r := range m.Rows {
		m.real[i] = r.Value
	}
	for j, g := range m.groups() {
		if g == m.StartGroup {
			m.group = j
		}
	}
	m.focus = m.firstVisible()
}

// ShownGroup is the group being shown ("" when the rows have none).
func (m *Form) ShownGroup() string {
	m.start()
	if gs := m.groups(); len(gs) > 0 {
		return gs[min(m.group, len(gs)-1)]
	}
	return ""
}

// groups lists the group names in order of appearance; empty when no row has one.
func (m *Form) groups() []string {
	var out []string
	seen := map[string]bool{}
	for _, r := range m.Rows {
		if r.Group != "" && !seen[r.Group] {
			seen[r.Group] = true
			out = append(out, r.Group)
		}
	}
	return out
}

// visible lists the row indexes of the current group (all rows when there are no groups).
func (m *Form) visible() []int {
	gs := m.groups()
	var out []int
	for i, r := range m.Rows {
		if len(gs) == 0 || r.Group == gs[min(m.group, len(gs)-1)] {
			out = append(out, i)
		}
	}
	return out
}

func (m *Form) firstVisible() int {
	if v := m.visible(); len(v) > 0 {
		return v[0]
	}
	return 0
}

func (m *Form) after(d time.Duration, msg tea.Msg) tea.Cmd {
	if m.After != nil {
		return m.After(d, msg)
	}
	return tea.Tick(d, func(time.Time) tea.Msg { return msg })
}

func (m *Form) editable(i int) bool {
	if i < 0 || i >= len(m.Rows) {
		return false
	}
	r := m.Rows[i]
	return !r.Unavailable && r.Kind != ui.FormReadonly
}

// touch marks a row edited and (re)starts the debounce.
func (m *Form) touch(i int) tea.Cmd {
	m.dirty[i] = true
	m.rowErr[i] = ""
	m.gen++
	return m.after(FormDebounce, formTick{m, m.gen})
}

// flush applies the first edited row when nothing is in flight.
func (m *Form) flush() tea.Cmd {
	if m.inflight >= 0 {
		m.want = true
		return nil
	}
	for i, d := range m.dirty {
		if !d {
			continue
		}
		m.dirty[i] = false
		m.inflight = i
		id, val, apply := m.Rows[i].ID, m.Rows[i].Value, m.Apply
		return func() tea.Msg {
			if apply == nil {
				return formResult{f: m, i: i}
			}
			row, err := apply(id, val)
			return formResult{f: m, i: i, row: row, err: err}
		}
	}
	if m.closing {
		m.Done = true
	}
	return nil
}

func (m *Form) anyDirty() bool {
	for _, d := range m.dirty {
		if d {
			return true
		}
	}
	return false
}

func (m *Form) result(v formResult) tea.Cmd {
	i := v.i
	m.inflight = -1
	if i >= 0 && i < len(m.Rows) {
		if v.err != nil {
			m.rowErr[i] = oneLine(textsafe.Clean(v.err.Error()))
			if m.rowErr[i] == "" {
				m.rowErr[i] = "refused"
			}
			if !m.dirty[i] { // snap back to what the camera really has
				m.Rows[i].Value = m.real[i]
			}
		} else {
			cur := m.Rows[i]
			if v.row.ID != "" {
				cur = v.row
				cur.Group = m.Rows[i].Group
				if v.row.Label == "" {
					cur.Label = m.Rows[i].Label
				}
			}
			m.real[i] = cur.Value
			if m.dirty[i] { // edited again meanwhile: keep the newer edit, take the new flags
				cur.Value = m.Rows[i].Value
			}
			m.Rows[i] = cur
			m.rowErr[i] = ""
		}
	}
	if m.closing || m.want {
		m.want = false
		return m.flush()
	}
	return nil
}

func (m *Form) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	m.start()
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.W, m.H = v.Width, v.Height
	case formTick:
		if v.f == m && v.gen == m.gen {
			return m, m.flush()
		}
	case formResult:
		if v.f == m {
			return m, m.result(v)
		}
	case tea.KeyMsg:
		return m, m.key(v)
	}
	return m, nil
}

func (m *Form) key(k tea.KeyMsg) tea.Cmd {
	if m.closing { // waiting for the pending changes: a second Esc or Ctrl-C leaves at once
		if s := k.String(); s == "esc" || s == "ctrl+c" {
			m.Done = true
		}
		return nil
	}
	if len(m.Rows) == 0 {
		if s := k.String(); s == "esc" || s == "q" || s == "ctrl+c" || s == "enter" {
			m.Done = true
		}
		return nil
	}
	wasHelp := m.help
	switch k.String() {
	case "up", "down", "j", "k": // the help follows the focus
	default:
		m.help = false
	}
	switch {
	case m.edit != nil:
		return m.keyText(k)
	case m.typing:
		return m.keyTyping(k)
	}
	i := m.focus
	r := m.Rows[i]
	if m.hasKey(k.String()) {
		m.Key = k.String()
		m.closing = true
		m.gen++ // apply what is pending now, then close
		if m.inflight >= 0 || m.anyDirty() {
			return m.flush()
		}
		m.Done = true
		return nil
	}
	switch k.String() {
	case "esc", "ctrl+c", "q":
		m.closing = true
		m.gen++ // the pending tick is not needed: apply now
		if m.inflight >= 0 || m.anyDirty() {
			return m.flush()
		}
		m.Done = true
	case "up", "k":
		m.move(-1)
	case "down", "j":
		m.move(1)
	case "tab":
		m.switchGroup(1)
	case "shift+tab":
		m.switchGroup(-1)
	case "?":
		m.help = !wasHelp
	case "left":
		return m.step(i, -1, 1)
	case "right":
		return m.step(i, 1, 1)
	case "shift+left":
		return m.step(i, -1, 10)
	case "shift+right":
		return m.step(i, 1, 10)
	case "pgdown":
		return m.step(i, -1, 10)
	case "pgup":
		return m.step(i, 1, 10)
	case " ", "space":
		if r.Kind == ui.FormEnum {
			return m.step(i, 1, 1)
		}
	case "enter":
		if !m.editable(i) {
			break
		}
		switch r.Kind {
		case ui.FormInt:
			m.typing, m.typed = true, ""
		case ui.FormText:
			e := &Input{Secret: r.Secret, Theme: m.Theme}
			if !r.Secret {
				e.val = []rune(r.Value)
				e.pos = len(e.val)
			}
			m.edit = e
		}
	default:
		if r.Kind == ui.FormInt && m.editable(i) && len(k.Runes) == 1 && (k.Runes[0] >= '0' && k.Runes[0] <= '9' || k.Runes[0] == '-' && r.Min < 0) {
			m.typing, m.typed = true, ""
			return m.keyTyping(k)
		}
	}
	return nil
}

func (m *Form) hasKey(s string) bool {
	for _, k := range m.Keys {
		if k.Key == s {
			return true
		}
	}
	return false
}

func (m *Form) move(d int) {
	v := m.visible()
	for p, i := range v {
		if i == m.focus {
			m.focus = v[min(max(p+d, 0), len(v)-1)]
			return
		}
	}
	if len(v) > 0 {
		m.focus = v[0]
	}
}

func (m *Form) switchGroup(d int) {
	gs := m.groups()
	if len(gs) < 2 {
		return
	}
	m.group = ((m.group+d)%len(gs) + len(gs)) % len(gs)
	m.focus = m.firstVisible()
}

// step changes the focused row by dir (+1 or -1) times n: an enum moves to the next choice (n is
// ignored), an int by n.
func (m *Form) step(i, dir, n int) tea.Cmd {
	if !m.editable(i) {
		return nil
	}
	r := &m.Rows[i]
	switch r.Kind {
	case ui.FormEnum:
		if len(r.Choices) == 0 {
			return nil
		}
		cur := -1
		for j, c := range r.Choices {
			if c == r.Value {
				cur = j
			}
		}
		if cur < 0 && dir < 0 {
			cur = 0
		}
		next := ((cur+dir)%len(r.Choices) + len(r.Choices)) % len(r.Choices)
		r.Value = r.Choices[next]
	case ui.FormInt:
		v, err := strconv.Atoi(strings.TrimSpace(r.Value))
		if err != nil {
			v = r.Min
		}
		nv := min(max(v+dir*n, r.Min), r.Max)
		if nv == v && err == nil {
			return nil
		}
		r.Value = strconv.Itoa(nv)
	default:
		return nil
	}
	return m.touch(i)
}

func (m *Form) keyTyping(k tea.KeyMsg) tea.Cmd {
	i := m.focus
	r := m.Rows[i]
	switch s := k.String(); {
	case s == "esc" || s == "ctrl+c":
		m.typing, m.typed = false, ""
	case s == "enter":
		m.typing = false
		txt := m.typed
		m.typed = ""
		v, err := strconv.Atoi(txt)
		switch {
		case txt == "":
		case err != nil || v < r.Min || v > r.Max:
			m.rowErr[i] = fmt.Sprintf("%s is not between %d and %d", oneLine(txt), r.Min, r.Max)
		case strconv.Itoa(v) != r.Value:
			m.Rows[i].Value = strconv.Itoa(v)
			return m.touch(i)
		}
	case s == "backspace":
		if rs := []rune(m.typed); len(rs) > 0 {
			m.typed = string(rs[:len(rs)-1])
		}
	case len(k.Runes) == 1 && len(m.typed) < 12 && (k.Runes[0] >= '0' && k.Runes[0] <= '9' || k.Runes[0] == '-' && r.Min < 0 && m.typed == ""):
		m.typed += string(k.Runes)
	}
	return nil
}

func (m *Form) keyText(k tea.KeyMsg) tea.Cmd {
	i := m.focus
	_, _ = m.edit.Update(k) // its own Quit command is not used
	if !m.edit.Done {
		return nil
	}
	e := m.edit
	m.edit = nil
	if e.Cancelled || (m.Rows[i].Secret && len(e.val) == 0) {
		return nil
	}
	if v := e.Value(); m.Rows[i].Secret || v != m.Rows[i].Value {
		m.Rows[i].Value = v
		m.dirty[i] = true
		m.rowErr[i] = ""
		m.gen++ // a text is applied at once: Enter was the "go"
		return m.flush()
	}
	return nil
}

// ---------------------------------------------------------------- drawing

func (m *Form) keys() []string { return m.keysFit(nil) }

// keysFit is the footer pairs; when fits is given and the full list does not fit, hints are dropped
// like on the board: the move and change hints first, then the group and type hints, then "? details",
// then the action keys from the last one. Esc and the first action key stay.
func (m *Form) keysFit(fits func([]string) bool) []string {
	switch {
	case m.edit != nil:
		return []string{"Enter", "apply", "Esc", "cancel"}
	case m.typing:
		return []string{"0-9", "type", "Enter", "apply", "Esc", "cancel"}
	}
	move := []string{"left/right", "change", "up/down", "move"}
	var extra []string
	if len(m.groups()) > 1 {
		extra = append(extra, "Tab", "group")
	}
	if m.focus < len(m.Rows) && m.editable(m.focus) && m.Rows[m.focus].Kind != ui.FormEnum {
		extra = append(extra, "Enter", "type a value")
	}
	var actions []string
	for _, fk := range m.Keys {
		actions = append(actions, fk.Key, fk.Label)
	}
	cat := func(parts ...[]string) []string {
		var out []string
		for _, p := range parts {
			out = append(out, p...)
		}
		return out
	}
	details, back := []string{"?", "details"}, []string{"Esc", "back"}
	all := cat(move, extra, actions, details, back)
	if fits == nil || fits(all) {
		return all
	}
	for _, p := range [][]string{cat(extra, actions, details, back), cat(actions, details, back), cat(actions, back)} {
		if fits(p) {
			return p
		}
	}
	a := actions
	for len(a) > 2 && !fits(cat(a, back)) {
		a = a[:len(a)-2]
	}
	return cat(a, back)
}

func (m *Form) View() string {
	w, _ := m.size()
	th := m.themed()
	fit := func(p []string) bool { return lipgloss.Width(th.Footer(p...))+2 <= w }
	return m.body() + clip("  "+th.Footer(m.keysFit(fit)...), w) + "\n"
}

// bar draws the value of an int row between its Min and Max.
func (m *Form) bar(r ui.FormRow, width int) string {
	th := m.themed()
	v, err := strconv.Atoi(strings.TrimSpace(r.Value))
	if err != nil || r.Max <= r.Min {
		return strings.Repeat(" ", width)
	}
	v = min(max(v, r.Min), r.Max)
	inner := max(width-2, 1)
	full := inner * (v - r.Min) / (r.Max - r.Min)
	return "[" + th.Accent.Render(strings.Repeat(th.Glyphs.BarFull, full)) +
		th.Muted.Render(strings.Repeat(th.Glyphs.BarEmpty, inner-full)) + "]"
}

// badge is what is said about a row at its right: a failure, a change on its way, or a flag.
func (m *Form) badge(i int) string {
	th, r := m.themed(), m.Rows[i]
	switch {
	case m.rowErr[i] != "":
		return th.Badge(BadgeErr, "refused")
	case m.inflight == i:
		return th.Badge(BadgeInfo, "sending")
	case r.Unavailable:
		return th.Badge(BadgeMute, "needs update")
	case r.NotSaved:
		return th.Badge(BadgeWarn, "not saved")
	}
	return ""
}

func (m *Form) valueCell(i, vw int) string {
	th, r := m.themed(), m.Rows[i]
	muted := r.Unavailable || r.Kind == ui.FormReadonly
	if m.edit != nil && i == m.focus {
		return m.edit.field(th, vw)
	}
	val := r.Value
	switch r.Kind {
	case ui.FormEnum:
		s := "< " + truncW(val, max(vw-4, 1), th.Glyphs.Ellipsis) + " >"
		if r.Unavailable {
			return th.Muted.Render(truncW(val, vw, th.Glyphs.Ellipsis))
		}
		return s
	case ui.FormInt:
		bw := min(vw-7, 12)
		num := val
		if m.typing && i == m.focus {
			num = th.CursorAt([]rune(m.typed), len([]rune(m.typed)))
		}
		cell := num
		if bw >= 6 {
			cell = m.bar(r, bw) + "  " + num
		}
		if muted {
			return th.Muted.Render(truncW(val, vw, th.Glyphs.Ellipsis))
		}
		return cell
	default: // text and readonly
		if r.Secret {
			if r.Value != "" || m.real[i] != "" {
				val = "(hidden)"
			} else {
				val = "(not set)"
			}
			return th.Muted.Render(truncW(val, vw, th.Glyphs.Ellipsis))
		}
		if val == "" {
			val = "(not set)"
			muted = true
		}
		val = truncW(oneLine(val), vw, th.Glyphs.Ellipsis)
		if muted {
			return th.Muted.Render(val)
		}
		return val
	}
}

// status is the line under the rows: what is happening, the refusal of the focused row, or its note.
func (m *Form) status(w int) string {
	th := m.themed()
	if m.focus >= len(m.Rows) {
		return ""
	}
	r := m.Rows[m.focus]
	maxw := max(w-8, 8)
	switch {
	case m.rowErr[m.focus] != "":
		return " " + th.Badge(BadgeErr, "") + " " + th.Err.Render(truncW(m.rowErr[m.focus], maxw, th.Glyphs.Ellipsis))
	case m.inflight >= 0:
		return "  " + th.Muted.Render("sent, waiting")
	case m.anyDirty():
		return "  " + th.Muted.Render("changed, sending in a moment")
	case r.Unavailable:
		return "  " + th.Muted.Render(truncW("update this camera to get it", maxw, th.Glyphs.Ellipsis))
	case r.NotSaved:
		return " " + th.Badge(BadgeWarn, "") + " " + th.Warn.Render(truncW("lost at the next reboot", maxw, th.Glyphs.Ellipsis))
	case r.Note != "":
		return "  " + th.Muted.Render(truncW(oneLine(r.Note), maxw, th.Glyphs.Ellipsis))
	}
	return ""
}

// body draws the title, the groups, the rows around the focus, the help and the status line.
func (m *Form) body() string {
	m.start()
	th := m.themed()
	w, h := m.size()
	var out []string
	out = append(out, " "+th.Bold.Render(truncW(m.Title, max(w-2, 8), th.Glyphs.Ellipsis)))
	if gs := m.groups(); len(gs) > 1 {
		var parts []string
		for j, g := range gs {
			if j == m.group {
				parts = append(parts, th.Sel.Render("["+g+"]"))
			} else {
				parts = append(parts, th.Muted.Render(g))
			}
		}
		out = append(out, "  "+strings.Join(parts, "  "))
	}
	var help []string
	if m.help && m.focus < len(m.Rows) {
		txt := m.Rows[m.focus].Detail
		if txt == "" {
			txt = "no more detail for this option"
		}
		for _, l := range wrapW(oneLine(txt), max(w-4, 8)) {
			help = append(help, "  "+th.Muted.Render(l))
		}
		help = append([]string{""}, help...)
		if len(help) > 5 {
			help = help[:5]
		}
	}

	vis := m.visible()
	room := max(h-len(out)-len(help)-1, 1) // the status line is the last
	// The window of rows shows the focus.
	start := 0
	for p, i := range vis {
		if i == m.focus {
			start = max(p-room+1, 0)
			if p < room {
				start = 0
			}
		}
	}
	end := min(start+room, len(vis))
	lw := 0
	for _, i := range vis {
		lw = max(lw, lipgloss.Width(m.Rows[i].Label))
	}
	lw = min(lw, max(w/3, 8))
	badgeW := th.BadgeWidth(maxBadgeWord)
	vw := w - 4 - lw - 2 - 2 - badgeW
	if vw < 8 { // a narrow window: the badge only appears when it fits
		badgeW = 0
		vw = max(w-4-lw-2, 4)
	}
	for _, i := range vis[start:end] {
		r := m.Rows[i]
		mark := "  "
		label := truncW(r.Label, lw, th.Glyphs.Ellipsis)
		switch {
		case i == m.focus:
			mark = th.Sel.Render(th.Glyphs.Marker) + " "
			label = th.Sel.Render(label)
		case r.Unavailable || r.Kind == ui.FormReadonly:
			label = th.Muted.Render(label)
		}
		line := " " + mark + pad(label, lw) + "  " + pad(m.valueCell(i, vw), vw)
		if b := m.badge(i); b != "" && badgeW > 0 {
			line += "  " + b
		}
		out = append(out, line)
	}
	out = append(out, help...)
	st := m.status(w)
	if len(vis) > end-start && st == "" {
		st = "  " + th.Muted.Render(fmt.Sprintf("%d/%d", posOf(vis, m.focus)+1, len(vis)))
	}
	out = append(out, st)
	return strings.Join(out, "\n") + "\n"
}

func posOf(vis []int, i int) int {
	for p, v := range vis {
		if v == i {
			return p
		}
	}
	return 0
}
