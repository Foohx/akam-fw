package ui

import (
	"strings"
	"testing"
)

// Without colour (a console that refused VT mode, NO_COLOR, a pipe) no escape byte is ever printed.
func TestPlainPrintsNoEscapeWhenColorIsOff(t *testing.T) {
	p, out, errb := newPlain("", true, false)
	p.Color = false
	p.Say("a")
	p.Bold("b")
	p.Good("c")
	p.Warn("d")
	p.Hint("e")
	_ = p.Fail("f", "g")
	for i := 0; i <= 100; i += 10 {
		p.Progress("Writing", i)
	}
	if strings.ContainsRune(out.String()+errb.String(), '\x1b') {
		t.Errorf("escape sequence with Color=false: %q %q", out.String(), errb.String())
	}
}
