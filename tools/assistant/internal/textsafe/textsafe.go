// Package textsafe cleans text that came from the network (a camera's output, a WS-Discovery
// name) before it is printed on the terminal, so that it cannot move the cursor, change the title
// or hide other lines.
package textsafe

import "strings"

// Clean removes terminal escape sequences (CSI, OSC and the two-byte ESC forms) and every control
// character except tab and newline: the C0 set (CR and BS included), DEL and the C1 set.
func Clean(s string) string {
	if !needs(s) {
		return s
	}
	var b strings.Builder
	b.Grow(len(s))
	rs := []rune(s)
	for i := 0; i < len(rs); i++ {
		r := rs[i]
		switch {
		case r == 0x1b:
			i = skipEscape(rs, i)
		case r == '\t' || r == '\n':
			b.WriteRune(r)
		case r < 0x20 || r == 0x7f || (r >= 0x80 && r <= 0x9f):
			// dropped
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

func needs(s string) bool {
	for _, r := range s {
		if (r < 0x20 && r != '\t' && r != '\n') || r == 0x7f || (r >= 0x80 && r <= 0x9f) {
			return true
		}
	}
	return false
}

// skipEscape returns the index of the last rune of the escape sequence that starts at rs[i].
func skipEscape(rs []rune, i int) int {
	if i+1 >= len(rs) {
		return i
	}
	switch rs[i+1] {
	case '[': // CSI: parameters 0x30-0x3f, intermediates 0x20-0x2f, final 0x40-0x7e
		j := i + 2
		for j < len(rs) && rs[j] >= 0x20 && rs[j] <= 0x3f {
			j++
		}
		if j < len(rs) && rs[j] >= 0x40 && rs[j] <= 0x7e {
			return j
		}
		return j - 1
	case ']', 'P', 'X', '^', '_': // OSC, DCS, SOS, PM, APC: up to BEL or ST
		for j := i + 2; j < len(rs); j++ {
			if rs[j] == 7 {
				return j
			}
			if rs[j] == 0x1b && j+1 < len(rs) && rs[j+1] == '\\' {
				return j + 1
			}
		}
		return len(rs) - 1
	}
	// two-byte form (ESC c, ESC 7 ...), possibly with intermediates 0x20-0x2f
	j := i + 1
	for j < len(rs) && rs[j] >= 0x20 && rs[j] <= 0x2f {
		j++
	}
	if j < len(rs) {
		return j
	}
	return len(rs) - 1
}
