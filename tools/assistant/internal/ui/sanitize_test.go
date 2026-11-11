package ui

import (
	"strings"
	"testing"
	"unicode/utf8"
)

func TestSanitize(t *testing.T) {
	cases := []struct{ in, want string }{
		{"akam-01", "akam-01"},
		{"a\x1b[2Jb\x1b]0;title\x07c", "abc"},
		{"name‮evil", "nameevil"},
		{"line1\nline2\r\nx", "line1 line2 x"},
		{"\ttab\t", "tab"},
		{"bad\xffbyte\x9b", "bad\ufffdbyte\ufffd"},
	}
	for _, c := range cases {
		if got := Sanitize(c.in); got != c.want {
			t.Errorf("Sanitize(%q) = %q, want %q", c.in, got, c.want)
		}
	}
	long := Sanitize(strings.Repeat("é", 1000))
	if n := utf8.RuneCountInString(long); n != SanitizeMax || !strings.HasSuffix(long, "…") {
		t.Errorf("overlong: %d runes", n)
	}
}
