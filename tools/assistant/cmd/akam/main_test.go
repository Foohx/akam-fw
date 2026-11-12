package main

import (
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

// The full-screen interface is used only on a capable terminal: --plain, no terminal, TERM=dumb and a
// console that refused VT output all give the plain interface.
func TestWantPlain(t *testing.T) {
	cases := []struct {
		name  string
		plain bool
		tty   bool
		env   tui.Env
		vt    bool
		want  bool
	}{
		{"capable terminal", false, true, tui.Env{GOOS: "linux"}, true, false},
		{"plain flag", true, true, tui.Env{GOOS: "linux"}, true, true},
		{"not a terminal", false, false, tui.Env{GOOS: "linux"}, true, true},
		{"TERM=dumb", false, true, tui.Env{GOOS: "linux", Dumb: true}, true, true},
		{"console refused VT", false, true, tui.Env{GOOS: "windows"}, false, true},
		{"NO_COLOR keeps the full screen", false, true, tui.Env{GOOS: "linux", NoColor: true}, true, false},
	}
	for _, c := range cases {
		if got := wantPlain(c.plain, c.tty, c.env, c.vt); got != c.want {
			t.Errorf("%s: wantPlain = %v, want %v", c.name, got, c.want)
		}
	}
}
