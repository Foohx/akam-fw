package tui

import (
	"fmt"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/muesli/termenv"
)

func nameList(n int) []string {
	out := make([]string, n)
	for i := range out {
		out[i] = fmt.Sprintf("camera-%02d 192.0.2.%d", i+1, i+10)
	}
	return out
}

// A long list numbers only the nine digit shortcuts; typing narrows it; the safety keys still work.
func TestPickerLongList(t *testing.T) {
	m := &Picker{Title: "Which camera?", Items: nameList(12)}
	v := m.View()
	if !strings.Contains(v, "9) camera-09") || strings.Contains(v, "10)") || strings.Contains(v, "12)") {
		t.Errorf("only the first nine items are numbered:\n%s", v)
	}
	if !strings.Contains(v, "type filter") {
		t.Errorf("the footer must tell about the filter:\n%s", v)
	}
	golden(t, "picker_long", v)

	// digits still select, a short list keeps its numbers and has no filter hint
	if m = feed(&Picker{Items: nameList(12)}, "3").(*Picker); !m.Done || m.Cursor != 2 {
		t.Errorf("digit select: %+v", m)
	}
	if s := (&Picker{Items: nameList(3)}).View(); strings.Contains(s, "filter") {
		t.Errorf("a short list has no filter: %s", s)
	}

	// typing filters over the visible rows; Enter picks the original index
	m = feed(&Picker{Items: nameList(12)}, "c", "a", "m", "e", "r", "a", "-", "1", "1").(*Picker)
	if got := m.visible(); len(got) != 1 || got[0] != 10 || m.Cursor != 10 {
		t.Fatalf("filter result %v cursor %d", got, m.Cursor)
	}
	golden(t, "picker_filtered", m.View())
	feed(m, "enter")
	if !m.Done || m.Cancelled || m.Cursor != 10 {
		t.Errorf("%+v", m)
	}

	// the first Esc clears the filter, the second cancels
	m = feed(&Picker{Items: nameList(12)}, "/", "z", "esc").(*Picker)
	if m.Done || m.filter != "" || m.filtering {
		t.Errorf("first esc: %+v", m)
	}
	feed(m, "esc")
	if !m.Done || !m.Cancelled {
		t.Errorf("second esc: %+v", m)
	}
	// Enter on an empty result does nothing; Ctrl-C always cancels
	m = feed(&Picker{Items: nameList(12)}, "z", "z", "enter").(*Picker)
	if m.Done {
		t.Error("enter with no match picked something")
	}
	feed(m, "ctrl+c")
	if !m.Cancelled {
		t.Error("ctrl+c")
	}
	// j, k and q keep their meaning as first letters; "/" lets a filter start with them
	m = feed(&Picker{Items: nameList(12)}, "j").(*Picker)
	if m.Cursor != 1 || m.filtering {
		t.Errorf("j moves: %+v", m)
	}
	if m = feed(&Picker{Items: nameList(12)}, "q").(*Picker); !m.Cancelled {
		t.Error("q cancels")
	}
	m = feed(&Picker{Items: []string{"alpha", "kilo", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8"}}, "/", "k").(*Picker)
	if len(m.visible()) != 1 || m.Cursor != 1 {
		t.Errorf("/ k: %v cursor %d", m.visible(), m.Cursor)
	}
}

// A window shorter than the list scrolls and never draws more lines than it has.
func TestPickerScrolls(t *testing.T) {
	m := &Picker{Title: "t", Items: nameList(30)}
	m.Update(tea.WindowSizeMsg{Width: 60, Height: 8})
	feed(m, "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down")
	v := m.View()
	if n := strings.Count(v, "\n"); n > 8 {
		t.Errorf("%d lines in an 8 line window", n)
	}
	if !strings.Contains(v, "camera-13") {
		t.Errorf("the cursor row is not visible:\n%s", v)
	}
}

func TestSplitBack(t *testing.T) {
	for _, c := range []struct {
		in     []string
		n, hid int
	}{
		{[]string{"a", "Back"}, 1, 1},
		{[]string{"a", "b", "Cancel"}, 2, 2},
		{[]string{"a", "b"}, 2, -1},
		{[]string{"Back"}, 1, -1}, // never an empty list
	} {
		got, hid := splitBack(c.in)
		if len(got) != c.n || hid != c.hid {
			t.Errorf("%v -> %v %d", c.in, got, hid)
		}
	}
}

func TestInputEditing(t *testing.T) {
	m := feed(&Input{Prompt: "n:"}, "a", "b", "c", "left", "left", "X").(*Input)
	if m.Value() != "aXbc" {
		t.Errorf("insert in the middle: %q", m.Value())
	}
	feed(m, "backspace")
	if m.Value() != "abc" || m.pos != 1 {
		t.Errorf("backspace at the cursor: %q %d", m.Value(), m.pos)
	}
	m.Update(tea.KeyMsg{Type: tea.KeyDelete})
	if m.Value() != "ac" {
		t.Errorf("delete: %q", m.Value())
	}
	m.Update(tea.KeyMsg{Type: tea.KeyHome})
	feed(m, "Z")
	m.Update(tea.KeyMsg{Type: tea.KeyEnd})
	feed(m, "!")
	if m.Value() != "Zac!" {
		t.Errorf("home/end: %q", m.Value())
	}
	// Left at the start and Right at the end stay put
	m = feed(&Input{Prompt: "n:"}, "left", "a", "right", "right", "b").(*Input)
	if m.Value() != "ab" {
		t.Errorf("bounds: %q", m.Value())
	}
	// non-ASCII text is edited by rune
	m = feed(&Input{Prompt: "n:"}, "é", "世", "left", "backspace").(*Input)
	if m.Value() != "世" {
		t.Errorf("runes: %q", m.Value())
	}
	// the default is a ghost until something is typed, and the value falls back to it
	g := &Input{Prompt: "n:", Def: "salon"}
	if !strings.Contains(g.View(), "salon") || g.Value() != "salon" {
		t.Errorf("ghost: %q", g.View())
	}
	feed(g, "x")
	if strings.Contains(g.View(), "salon") {
		t.Errorf("the ghost must go once typing starts: %q", g.View())
	}
	// Enter and Esc as before
	if m = feed(&Input{Def: "d"}, "enter").(*Input); !m.Done || m.Cancelled || m.Value() != "d" {
		t.Errorf("%+v", m)
	}
}

// The cursor is a reverse video cell under a colour profile; a hidden prompt shows only the cursor.
func TestInputCursorAndSecret(t *testing.T) {
	th := NewTheme(Env{Profile: termenv.ANSI, ASCII: true})
	m := &Input{Prompt: "n:", Theme: th}
	feed(m, "a", "b", "left")
	v := m.View()
	if !strings.Contains(v, "\x1b[7mb\x1b[0m") {
		t.Errorf("no reverse cursor on the character under it: %q", v)
	}
	s := &Input{Prompt: "pw:", Secret: true, Theme: th}
	feed(s, "h", "u", "n", "t", "e", "r", "2", "left", "left")
	if v := s.View(); strings.Contains(v, "hunter") || strings.Contains(v, "*") || strings.Contains(v, "\x1b[7mu") {
		t.Errorf("a hidden prompt leaks its input: %q", v)
	}
	if s.Value() != "hunter2" {
		t.Error("secret value")
	}
	// a narrow window keeps the cursor in view
	n := &Input{Prompt: "n:", W: 14}
	feed(n, "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n")
	if v := n.View(); strings.Contains(v, "abc") || !strings.Contains(v, "n") {
		t.Errorf("narrow input: %q", v)
	}
}
