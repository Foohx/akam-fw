package cli

import "strings"

// plainText makes a line supplied by the camera safe to print: every control character (escape sequences,
// bell, carriage return, C1 codes) becomes '?', so a camera cannot drive the user's terminal.
func plainText(s string) string {
	return strings.Map(func(r rune) rune {
		if r < 0x20 || (r >= 0x7f && r < 0xa0) {
			return '?'
		}
		return r
	}, s)
}
