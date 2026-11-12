package tui

import (
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// clock is the injectable timer of a Form: it records what was scheduled and delivers nothing by itself.
type clock struct {
	durs []time.Duration
	msgs []tea.Msg
}

func (c *clock) after(d time.Duration, m tea.Msg) tea.Cmd {
	c.durs = append(c.durs, d)
	c.msgs = append(c.msgs, m)
	return nil
}

func (c *clock) last() tea.Msg { return c.msgs[len(c.msgs)-1] }

type applied struct{ id, val string }

type formRig struct {
	t     *testing.T
	f     *Form
	clk   *clock
	calls []applied
	fail  map[string]string // id -> refusal text
}

func sampleRows() []ui.FormRow {
	return []ui.FormRow{
		{ID: "picture.brightness", Label: "Brightness", Kind: ui.FormInt, Value: "50", Min: 0, Max: 100},
		{ID: "picture.saturation", Label: "Saturation", Kind: ui.FormInt, Value: "40", Min: 0, Max: 100, NotSaved: true},
		{ID: "picture.mirror", Label: "Mirror", Kind: ui.FormEnum, Value: "off", Choices: []string{"off", "on"}},
		{ID: "net.hostname", Label: "Host name", Kind: ui.FormText, Value: "salon"},
		{ID: "net.mqtt_password", Label: "MQTT password", Kind: ui.FormText, Secret: true},
		{ID: "picture.wdr", Label: "Wide range", Kind: ui.FormEnum, Value: "off", Choices: []string{"off", "on"}, Unavailable: true},
		{ID: "info.model", Label: "Model", Kind: ui.FormReadonly, Value: "Sample cam"},
	}
}

func newRig(t *testing.T, rows []ui.FormRow, w, h int) *formRig {
	r := &formRig{t: t, clk: &clock{}, fail: map[string]string{}}
	r.f = &Form{Title: "Camera options - salon", Rows: rows, Theme: testTheme(), After: r.clk.after, W: w, H: h,
		Apply: func(id, v string) (ui.FormRow, error) {
			r.calls = append(r.calls, applied{id, v})
			if why := r.fail[id]; why != "" {
				return ui.FormRow{}, errors.New(why)
			}
			return ui.FormRow{}, nil
		}}
	return r
}

// press feeds keys; a command that is a function (Apply) is run and its result is delivered.
func (r *formRig) press(keys ...string) {
	for _, k := range keys {
		_, c := r.f.Update(key(k))
		r.run(c)
	}
}

func (r *formRig) run(c tea.Cmd) {
	if c == nil {
		return
	}
	if m := c(); m != nil {
		_, c2 := r.f.Update(m)
		r.run(c2)
	}
}

// fire delivers the last scheduled tick.
func (r *formRig) fire() {
	_, c := r.f.Update(r.clk.last())
	r.run(c)
}

func (r *formRig) val(id string) string {
	for _, row := range r.f.Rows {
		if row.ID == id {
			return row.Value
		}
	}
	r.t.Fatalf("no row %s", id)
	return ""
}

func TestFormEnumDebounce(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("down", "down") // Mirror
	r.press("right")
	if r.val("picture.mirror") != "on" {
		t.Fatalf("right must cycle: %q", r.val("picture.mirror"))
	}
	r.press("space") // off
	r.press("left")  // and back to on
	if len(r.clk.msgs) != 3 || r.clk.durs[0] != FormDebounce || FormDebounce != 400*time.Millisecond {
		t.Fatalf("each key restarts the 400 ms timer: %v", r.clk.durs)
	}
	if len(r.calls) != 0 {
		t.Fatalf("nothing is sent before the pause: %v", r.calls)
	}
	// The earlier ticks are stale: only the last one flushes.
	_, c := r.f.Update(r.clk.msgs[0])
	if c != nil || len(r.calls) != 0 {
		t.Fatal("a stale tick must do nothing")
	}
	r.fire()
	if len(r.calls) != 1 || r.calls[0] != (applied{"picture.mirror", "on"}) {
		t.Fatalf("one apply with the final value: %v", r.calls)
	}
}

func TestFormEnumWraps(t *testing.T) {
	r := newRig(t, []ui.FormRow{{ID: "a", Label: "A", Kind: ui.FormEnum, Value: "x", Choices: []string{"x", "y", "z"}}}, 80, 24)
	r.press("left")
	if r.val("a") != "z" {
		t.Errorf("left wraps to the last choice: %q", r.val("a"))
	}
	r.press("right", "right")
	if r.val("a") != "y" {
		t.Errorf("right wraps: %q", r.val("a"))
	}
}

func TestFormNumbers(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("right")
	r.press("shift+right")
	r.press("pgup")
	if got := r.val("picture.brightness"); got != "71" {
		t.Errorf("+1 +10 +10: %q", got)
	}
	r.press("left", "shift+left", "pgdown")
	if got := r.val("picture.brightness"); got != "50" {
		t.Errorf("-1 -10 -10: %q", got)
	}
	for i := 0; i < 8; i++ {
		r.press("pgup")
	}
	if got := r.val("picture.brightness"); got != "100" {
		t.Errorf("clamped to Max: %q", got)
	}
	n := len(r.clk.msgs)
	r.press("right") // already at Max: nothing changes, nothing is scheduled
	if len(r.clk.msgs) != n {
		t.Error("a key that changes nothing must not start the timer")
	}
	// typed digits then Enter; same debounce
	r.press("enter", "7", "5", "enter")
	if got := r.val("picture.brightness"); got != "75" {
		t.Errorf("typed: %q", got)
	}
	if len(r.calls) != 0 {
		t.Error("a typed number goes through the debounce too")
	}
	r.fire()
	if len(r.calls) != 1 || r.calls[0].val != "75" {
		t.Errorf("%v", r.calls)
	}
	// a digit typed straight away starts the entry; Backspace edits; Esc cancels the entry only
	r.press("9", "9", "backspace")
	if !strings.Contains(r.f.body(), "9") || !r.f.typing {
		t.Error("digits typed on a number row start the entry")
	}
	r.press("esc")
	if r.f.Done || r.f.typing || r.val("picture.brightness") != "75" {
		t.Errorf("Esc cancels the entry: %+v", r.f)
	}
	// out of range: refused on the spot, no apply, the value stays
	before := len(r.calls)
	r.press("enter", "9", "9", "9", "enter")
	if r.val("picture.brightness") != "75" || len(r.calls) != before {
		t.Error("an out of range number is not applied")
	}
	if v := r.f.body(); !strings.Contains(v, "between 0 and 100") || !strings.Contains(v, "[x] refused") {
		t.Errorf("the reason shows:\n%s", v)
	}
}

func TestFormTextInline(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("down", "down", "down", "enter") // Host name
	if !strings.Contains(r.f.View(), "Enter apply") {
		t.Errorf("editing footer:\n%s", r.f.View())
	}
	r.press("backspace", "backspace", "x", "y")
	r.press("enter")
	if len(r.calls) != 1 || r.calls[0] != (applied{"net.hostname", "salxy"}) {
		t.Errorf("Enter applies a text at once: %v", r.calls)
	}
	// Esc cancels the edit without leaving the form
	r.press("enter", "z", "esc")
	if r.f.Done || r.val("net.hostname") != "salxy" || len(r.calls) != 1 {
		t.Errorf("%+v %v", r.f, r.calls)
	}
}

func TestFormSnapBack(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.fail["picture.brightness"] = "the camera refused: out of range"
	r.press("pgup")
	if r.val("picture.brightness") != "60" {
		t.Fatal("optimistic value")
	}
	r.fire()
	if got := r.val("picture.brightness"); got != "50" {
		t.Errorf("a failed row snaps back to its real value: %q", got)
	}
	v := r.f.body()
	if !strings.Contains(v, "[x] refused") || !strings.Contains(v, "the camera refused: out of range") {
		t.Errorf("badge and reason:\n%s", v)
	}
	// the next edit clears the failure
	r.fail["picture.brightness"] = ""
	r.press("right")
	if strings.Contains(r.f.body(), "refused") {
		t.Error("an edit clears the failure")
	}
	r.fire()
	if r.val("picture.brightness") != "51" {
		t.Errorf("%q", r.val("picture.brightness"))
	}
	// a returned row replaces the flags (not saved) and may correct the value
	r.f.Apply = func(id, v string) (ui.FormRow, error) {
		return ui.FormRow{ID: id, Value: "52", Kind: ui.FormInt, Min: 0, Max: 100, NotSaved: true}, nil
	}
	r.press("right")
	r.fire()
	row := r.f.Rows[0]
	if row.Value != "52" || !row.NotSaved || row.Label != "Brightness" {
		t.Errorf("returned row: %+v", row)
	}
	if !strings.Contains(r.f.body(), "[!] not saved") {
		t.Errorf("badge:\n%s", r.f.body())
	}
}

func TestFormEditedAgainWhileSending(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("right") // 51
	_, c := r.f.Update(r.clk.last())
	if c == nil || r.f.inflight != 0 {
		t.Fatal("flush starts the apply")
	}
	r.press("right") // 52 while 51 is in flight
	// a tick arrives while the apply runs: it waits
	_, c2 := r.f.Update(r.clk.last())
	if c2 != nil {
		t.Fatal("never two applies at once")
	}
	r.run(c) // the first result: the flush of 52 follows
	if len(r.calls) != 2 || r.calls[0].val != "51" || r.calls[1].val != "52" {
		t.Errorf("applied in order: %v", r.calls)
	}
}

func TestFormEscAppliesPending(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("right")
	r.press("esc")
	if len(r.calls) != 1 || !r.f.Done {
		t.Errorf("Esc applies what is pending before closing: calls %v done %v", r.calls, r.f.Done)
	}
	// the stale tick of the closed form does nothing
	if _, c := r.f.Update(r.clk.msgs[0]); c != nil {
		t.Error("stale tick")
	}
	// Esc with nothing pending closes at once
	r = newRig(t, sampleRows(), 80, 24)
	r.press("esc")
	if !r.f.Done || len(r.calls) != 0 {
		t.Error("plain Esc")
	}
	// a second Esc leaves even while an apply never answers
	r = newRig(t, sampleRows(), 80, 24)
	r.press("right")
	_, c := r.f.Update(key("esc")) // flush started, result pending
	if c == nil || r.f.Done {
		t.Fatal("closing waits for the apply")
	}
	r.f.Update(key("esc"))
	if !r.f.Done {
		t.Error("the second Esc leaves at once")
	}
}

func TestFormUnavailableAndReadonly(t *testing.T) {
	r := newRig(t, sampleRows(), 80, 24)
	r.press("down", "down", "down", "down", "down") // Wide range
	n := len(r.clk.msgs)
	r.press("right", "space", "enter")
	if r.val("picture.wdr") != "off" || len(r.clk.msgs) != n || r.f.edit != nil {
		t.Error("an unavailable row is not editable")
	}
	v := r.f.body()
	if !strings.Contains(v, "needs update") || !strings.Contains(v, "update this camera to get it") {
		t.Errorf("badge and sentence:\n%s", v)
	}
	r.press("down", "right", "enter", "5")
	if r.val("info.model") != "Sample cam" || r.f.typing {
		t.Error("readonly")
	}
}

func TestFormSecretNeverShown(t *testing.T) {
	const secret = "hunter2-Zq"
	r := newRig(t, sampleRows(), 80, 24)
	r.press("down", "down", "down", "down", "enter")
	for _, ch := range secret {
		r.press(string(ch))
		if v := r.f.View(); strings.Contains(v, "hun") {
			t.Fatalf("typed secret echoed:\n%s", v)
		}
	}
	r.press("enter")
	if len(r.calls) != 1 || r.calls[0] != (applied{"net.mqtt_password", secret}) {
		t.Fatalf("the secret reaches apply: %v", r.calls)
	}
	v := r.f.View()
	if strings.Contains(v, secret) || strings.Contains(v, "hunter") || !strings.Contains(v, "(hidden)") {
		t.Errorf("after apply:\n%s", v)
	}
	// a secret row given a value by mistake is not drawn either
	f := &Form{Theme: testTheme(), Rows: []ui.FormRow{{ID: "p", Label: "Password", Kind: ui.FormText, Secret: true, Value: "leak"}}}
	if strings.Contains(f.View(), "leak") {
		t.Error("secret value drawn")
	}
	// the session wrapper strips it before it gets to the model (see TestSessionForm)
	if cleanRow(ui.FormRow{Secret: true, Value: "leak"}).Value != "" {
		t.Error("cleanRow keeps a secret")
	}
}

func TestFormGroups(t *testing.T) {
	rows := []ui.FormRow{
		{ID: "a", Group: "Video", Label: "Size", Kind: ui.FormEnum, Value: "1080p", Choices: []string{"1080p", "720p"}},
		{ID: "b", Group: "Picture", Label: "Contrast", Kind: ui.FormInt, Value: "60", Min: 0, Max: 100},
		{ID: "c", Group: "Picture", Label: "Mirror", Kind: ui.FormEnum, Value: "off", Choices: []string{"off", "on"}},
	}
	r := newRig(t, rows, 80, 24)
	v := r.f.body()
	if !strings.Contains(v, "[Video]") || !strings.Contains(v, "Size") || strings.Contains(v, "Contrast") {
		t.Errorf("first group:\n%s", v)
	}
	r.press("tab")
	v = r.f.body()
	if !strings.Contains(v, "[Picture]") || !strings.Contains(v, "Contrast") || strings.Contains(v, "Size ") {
		t.Errorf("second group:\n%s", v)
	}
	r.press("down", "right")
	if r.val("c") != "on" || r.val("a") != "1080p" {
		t.Error("focus moves inside the group")
	}
	r.press("shift+tab")
	if !strings.Contains(r.f.body(), "[Video]") {
		t.Error("shift+tab")
	}
}

func TestFormHelp(t *testing.T) {
	rows := sampleRows()
	rows[0].Detail = "Brightness of the picture, from dark to bright."
	r := newRig(t, rows, 80, 24)
	r.press("?")
	if !strings.Contains(r.f.body(), "from dark to bright") {
		t.Errorf("? shows the detail:\n%s", r.f.body())
	}
	r.press("?")
	if strings.Contains(r.f.body(), "from dark") {
		t.Error("? again hides it")
	}
}

func TestFormGoldens(t *testing.T) {
	for _, sz := range [][2]int{{80, 24}, {60, 16}, {120, 40}} {
		w, h := sz[0], sz[1]
		tag := fmt.Sprintf("%dx%d", w, h)
		r := newRig(t, sampleRows(), w, h-1)
		r.press("down") // Saturation: not saved
		v := r.f.View()
		checkFits(t, "form "+tag, strings.TrimRight(v, "\n"), w, h)
		golden(t, "form_"+tag, v)

		// an edit in flight and an unavailable row focused
		r = newRig(t, sampleRows(), w, h-1)
		r.press("down", "down", "right")
		_, c := r.f.Update(r.clk.last())
		_ = c // the result never arrives: "sending"
		v = r.f.View()
		checkFits(t, "form sending "+tag, strings.TrimRight(v, "\n"), w, h)
		golden(t, "form_sending_"+tag, v)
	}
	r := newRig(t, sampleRows(), 80, 24)
	r.fail["picture.brightness"] = "the camera refused the value"
	r.press("right")
	r.fire()
	golden(t, "form_refused_80x24", r.f.View())
	r = newRig(t, sampleRows(), 80, 24)
	r.press("down", "down", "down", "down", "down")
	golden(t, "form_unavailable_80x24", r.f.View())
	// more rows than lines: the window follows the focus
	many := make([]ui.FormRow, 30)
	for i := range many {
		many[i] = ui.FormRow{ID: fmt.Sprint(i), Label: fmt.Sprintf("Option %02d", i), Kind: ui.FormInt, Value: fmt.Sprint(i * 3), Min: 0, Max: 100}
	}
	r = newRig(t, many, 60, 15)
	for i := 0; i < 20; i++ {
		r.press("down")
	}
	v := r.f.View()
	checkFits(t, "form many", strings.TrimRight(v, "\n"), 60, 16)
	if !strings.Contains(v, "Option 20") || strings.Contains(v, "Option 00") {
		t.Errorf("window follows the focus:\n%s", v)
	}
	golden(t, "form_many_60x16", v)
}
