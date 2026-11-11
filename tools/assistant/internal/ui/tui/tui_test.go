package tui

import (
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

var update = flag.Bool("update", false, "rewrite the golden files")

func init() { lipgloss.SetColorProfile(termenv.Ascii) } // no colour codes in the golden files

func key(s string) tea.KeyMsg {
	switch s {
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}
	case "esc":
		return tea.KeyMsg{Type: tea.KeyEsc}
	case "up":
		return tea.KeyMsg{Type: tea.KeyUp}
	case "down":
		return tea.KeyMsg{Type: tea.KeyDown}
	case "left":
		return tea.KeyMsg{Type: tea.KeyLeft}
	case "right":
		return tea.KeyMsg{Type: tea.KeyRight}
	case "tab":
		return tea.KeyMsg{Type: tea.KeyTab}
	case "backspace":
		return tea.KeyMsg{Type: tea.KeyBackspace}
	case "ctrl+c":
		return tea.KeyMsg{Type: tea.KeyCtrlC}
	case "space":
		return tea.KeyMsg{Type: tea.KeySpace, Runes: []rune{' '}}
	}
	return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
}

func feed(m tea.Model, keys ...string) tea.Model {
	for _, k := range keys {
		m, _ = m.Update(key(k))
	}
	return m
}

func golden(t *testing.T, name, got string) {
	t.Helper()
	got = strings.Join(trimRight(strings.Split(got, "\n")), "\n")
	p := filepath.Join("testdata", name+".golden")
	if *update {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("missing %s (run: go test ./internal/ui/tui -update): %v", p, err)
	}
	if string(want) != got {
		t.Errorf("%s differs\n--- want ---\n%s\n--- got ---\n%s", name, want, got)
	}
}

func trimRight(lines []string) []string {
	out := make([]string, len(lines))
	for i, l := range lines {
		out[i] = strings.TrimRight(l, " ")
	}
	return out
}

func yesno(keys ...string) *YesNo {
	m := &YesNo{Prompt: "Do it?"}
	return feed(m, keys...).(*YesNo)
}

// Every way out of a yes/no is No unless the user typed y or moved to Yes and pressed Enter.
func TestYesNoDefaultsToNo(t *testing.T) {
	for _, keys := range [][]string{{"enter"}, {"n"}, {"N"}, {"q"}, {"esc"}, {"ctrl+c"}, {"right", "enter"}, {"tab", "enter"}, {"left", "right", "enter"}} {
		m := yesno(keys...)
		if !m.Done || m.Result {
			t.Errorf("%v -> done=%v yes=%v", keys, m.Done, m.Result)
		}
	}
	for _, keys := range [][]string{{"y"}, {"Y"}, {"left", "enter"}, {"shift+tab", "enter"}} {
		var m *YesNo
		if keys[0] == "shift+tab" {
			m = &YesNo{Prompt: "x"}
			m.Update(tea.KeyMsg{Type: tea.KeyShiftTab})
			m.Update(key("enter"))
		} else {
			m = yesno(keys...)
		}
		if !m.Done || !m.Result {
			t.Errorf("%v -> done=%v yes=%v", keys, m.Done, m.Result)
		}
	}
	if m := yesno("x", "z", "1"); m.Done {
		t.Error("an unrelated key ended the question")
	}
}

func TestYesNoGolden(t *testing.T) {
	golden(t, "yesno_default", (&YesNo{Prompt: "Make the camera visible to ONVIF apps?"}).View())
	golden(t, "yesno_yes_selected", feed(&YesNo{Prompt: "Make the camera visible to ONVIF apps?"}, "left").View())
	d := &YesNo{Prompt: "Write the flash now?", Title: "WRITE THE CAMERA FLASH", Danger: true,
		Box: []string{"A power cut during the write can brick the camera.", "Image md5: 0123456789abcdef"}}
	golden(t, "danger_default", d.View())
}

func TestInput(t *testing.T) {
	m := feed(&Input{Prompt: "Name:", Def: "viewer"}).(*Input)
	if m.Value() != "viewer" {
		t.Error("default")
	}
	m = feed(&Input{Prompt: "Name:", Def: "viewer"}, "s", "a", "l", "x", "backspace", "o", "n", "enter").(*Input)
	if !m.Done || m.Cancelled || m.Value() != "salon" {
		t.Errorf("%+v %q", m, m.Value())
	}
	m = feed(&Input{Prompt: "Name:"}, "a", "space", "b").(*Input)
	if m.Value() != "a b" {
		t.Errorf("%q", m.Value())
	}
	if m = feed(&Input{Prompt: "Name:", Def: "x"}, "a", "esc").(*Input); !m.Cancelled {
		t.Error("esc")
	}
	// nothing typed is ever drawn on a hidden prompt, not even stars
	s := feed(&Input{Prompt: "Password:", Secret: true}, "h", "u", "n", "t", "e", "r", "2").(*Input)
	if v := s.View(); strings.Contains(v, "hunter") || strings.Contains(v, "*") {
		t.Errorf("a hidden prompt shows its input: %q", v)
	}
	if s.Value() != "hunter2" {
		t.Error("secret value")
	}
	golden(t, "input_plain", feed(&Input{Prompt: "Camera name [default]:", Def: "default"}).View())
	golden(t, "input_secret", s.View())
}

func TestPicker(t *testing.T) {
	m := feed(&Picker{Title: "Which one is your camera?", Items: []string{"192.168.1.60", "192.168.1.61", "None of these"}}, "down", "j", "j", "k", "enter").(*Picker)
	if !m.Done || m.Cursor != 1 || m.Cancelled {
		t.Errorf("%+v", m)
	}
	if m = feed(&Picker{Items: []string{"a", "b"}}, "esc").(*Picker); !m.Cancelled {
		t.Error("esc")
	}
	if m = feed(&Picker{Items: []string{"a", "b"}}, "2").(*Picker); !m.Done || m.Cursor != 1 {
		t.Error("digit")
	}
	if m = feed(&Picker{Items: []string{"a", "b"}}, "9").(*Picker); m.Done {
		t.Error("a digit beyond the list")
	}
	golden(t, "picker", feed(&Picker{Title: "Which one is your camera?", Items: []string{"192.168.1.60  ports 22 554 8091 (akam)", "192.168.1.61  ports 24 (stock)", "None of these"}}, "down").View())
}

func menuItems() []ui.MenuItem {
	return []ui.MenuItem{
		{Label: "1. Check this computer", Detail: "Why: tools must be present before anything else. Risk: none (read-only).", Status: "done"},
		{Label: "2. Get the Akam kit", Detail: "Why: the prebuilt kit means no compiling. Risk: none (download + signature check).", Status: "todo"},
		{Label: "3. Prepare the backup microSD card", Detail: "Why: boots the camera into a rescue shell. Risk: ERASES the microSD card.", Status: "blocked"},
		{Label: "4. Find the camera on your network", Detail: "Why: later steps need the camera address. Risk: none.", Status: "stale"},
		{Label: "Camera: show LEDs", Detail: "Front LED and infrared LEDs (read-only).", Status: "tool"},
	}
}

func sized(m *Menu) *Menu {
	m.Update(tea.WindowSizeMsg{Width: 80, Height: 24})
	return m
}

func TestMenu(t *testing.T) {
	m := sized(NewMenu("Akam - camera: salon", menuItems()))
	if m.Cursor != 1 {
		t.Errorf("the menu must start on the first step to do, not %d", m.Cursor)
	}
	golden(t, "menu_start", m.View())
	feed(m, "j", "j", "j", "j", "j", "j") // stops at the last line
	if m.Cursor != 4 {
		t.Errorf("cursor %d", m.Cursor)
	}
	feed(m, "k", "up")
	if m.Cursor != 2 {
		t.Errorf("cursor %d", m.Cursor)
	}
	feed(m, "?")
	golden(t, "menu_help", m.View())
	feed(m, "?", "enter")
	if !m.Done || m.Quit || m.Cursor != 2 {
		t.Errorf("%+v", m)
	}
	for _, k := range []string{"q", "esc", "ctrl+c"} {
		mm := sized(NewMenu("t", menuItems()))
		feed(mm, k)
		if !mm.Done || !mm.Quit {
			t.Errorf("%s did not quit", k)
		}
	}
	// a short window scrolls and never draws more rows than it has
	small := NewMenu("t", menuItems())
	small.Update(tea.WindowSizeMsg{Width: 40, Height: 9})
	feed(small, "G")
	if n := strings.Count(small.View(), "\n"); n > 9 {
		t.Errorf("%d lines in a 9 line window", n)
	}
	golden(t, "menu_small_window_end", small.View())
}

// The colour map: the same views carry colour codes on a colour terminal (golden files are
// compared without them).
func TestColourMap(t *testing.T) {
	lipgloss.SetColorProfile(termenv.ANSI)
	defer lipgloss.SetColorProfile(termenv.Ascii)
	d := &YesNo{Prompt: "Write?", Title: "DANGER", Danger: true, Box: []string{"x"}}
	if v := d.View(); !strings.Contains(v, "\x1b[") {
		t.Errorf("the danger box has no colour: %q", v)
	}
	menu := sized(NewMenu("t", menuItems())).View()
	if !strings.Contains(menu, "\x1b[") {
		t.Error("the menu has no colour")
	}
	// done is green, blocked is dim: different codes
	if statusTag("done") == statusTag("blocked") {
		t.Error("done and blocked look the same")
	}
}
