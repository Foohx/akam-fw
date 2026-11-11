package textsafe

import (
	"math/rand"
	"strings"
	"testing"
	"unicode/utf8"
)

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
		{"\x1b]0;t\u009cok", "ok"},             // OSC ended by C1 ST
		{"\x1b]0;unterminated\nmore", ""},      // swallowed to the end, as a terminal would
		{"a\x1b\nb", "a\nb"},                   // ESC then a newline keeps the newline
		{"a\x9bb\x80c", "a\ufffdb\ufffdc"},     // raw C1 bytes are not valid UTF-8
		{"x\u202eevil\u200b\u2028y", "xevily"}, // bidi override, zero-width, line separator
		{"\x1b[31m\u009b2Jz", "2Jz"},
	} {
		if got := Clean(c.in); got != c.want {
			t.Errorf("Clean(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// Random and hostile input: no panic, the output is valid UTF-8 and holds no control character
// (but tab and newline), no ESC and no C1 character.
func TestCleanRandomInput(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	pieces := []string{"\x1b", "[", "]", "P", "\\", "\x07", "\x9b", "\x9c", "\u009b", "\u009d", "\r", "\n", "\t", "0", ";", "m", "é", "\xff", "\xc3", "\u202e", " ", "a", "\x00", "\x7f"}
	for i := 0; i < 20000; i++ {
		var b strings.Builder
		for n := r.Intn(24); n > 0; n-- {
			b.WriteString(pieces[r.Intn(len(pieces))])
		}
		out := Clean(b.String())
		if !utf8.ValidString(out) {
			t.Fatalf("invalid UTF-8 from %q: %q", b.String(), out)
		}
		for _, c := range out {
			if (c < 0x20 && c != '\t' && c != '\n') || c == 0x7f || (c >= 0x80 && c <= 0x9f) || c == 0x1b {
				t.Fatalf("control %U left from %q: %q", c, b.String(), out)
			}
		}
		if Clean(out) != out {
			t.Fatalf("not idempotent: %q", out)
		}
	}
}

func FuzzClean(f *testing.F) {
	f.Add("\x1b[31m\x1b]0;x\x07")
	f.Fuzz(func(t *testing.T, s string) {
		out := Clean(s)
		if !utf8.ValidString(out) || strings.ContainsRune(out, 0x1b) {
			t.Fatalf("%q -> %q", s, out)
		}
	})
}
