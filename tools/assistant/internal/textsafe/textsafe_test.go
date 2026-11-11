package textsafe

import "testing"

func TestClean(t *testing.T) {
	for _, c := range []struct{ in, want string }{
		{"plain text é", "plain text é"},
		{"a\tb\nc", "a\tb\nc"},
		{"\x1b[2J\x1b[Hhello", "hello"},
		{"x\x1b[31;1mred\x1b[0my", "xredy"},
		{"\x1b]0;evil title\x07ok", "ok"},
		{"\x1b]0;evil\x1b\\ok", "ok"},
		{"line\rovertype", "lineovertype"},
		{"bell\x07del\x7fnul\x00", "belldelnul"},
		{"c1\u009b31mx", "c131mx"},
		{"\x1bc reset", " reset"},
		{"trunc\x1b[", "trunc"},
		{"trunc\x1b", "trunc"},
	} {
		if got := Clean(c.in); got != c.want {
			t.Errorf("Clean(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}
