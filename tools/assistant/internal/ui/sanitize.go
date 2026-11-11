package ui

import (
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
)

// SanitizeMax is the longest string Sanitize lets through, in runes.
const SanitizeMax = 255

// Sanitize makes one string that came from the network (a camera name, a firmware string, a
// WS-Discovery scope) safe to show on a single terminal line: escape sequences, control and bidi
// characters are removed (textsafe), tabs and newlines become one space, and an overlong value is
// cut with an ellipsis.
func Sanitize(s string) string {
	s = textsafe.Clean(s)
	s = strings.Map(func(r rune) rune {
		if r == '\n' || r == '\t' {
			return ' '
		}
		return r
	}, s)
	if rs := []rune(s); len(rs) > SanitizeMax {
		s = string(rs[:SanitizeMax-1]) + "…"
	}
	return strings.TrimSpace(s)
}
