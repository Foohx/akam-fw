package ui

import "testing"

// TestPlainDangerDefaultsNo (specs/installer.md, section 12): Enter, q, Esc, n, junk and the end of the input are all No; only
// y / yes answers yes.
func TestPlainDangerDefaultsNo(t *testing.T) {
	for _, in := range []string{"", "\n", "n\n", "N\n", "q\n", "\x1b\n", "nope\n", "yy\n", " y\n", "1\n", "\x03\n", "\x04"} {
		p, _, _ := newPlain(in, true, false)
		if ok, _ := p.Danger("ERASE", "Erase it?", "line"); ok {
			t.Errorf("input %q answered yes to a red box", in)
		}
	}
	for _, in := range []string{"y\n", "yes\n", "YES\n"} {
		p, _, _ := newPlain(in, true, false)
		if ok, err := p.Danger("ERASE", "Erase it?", "line"); !ok || err != nil {
			t.Errorf("input %q: %v %v", in, ok, err)
		}
	}
}

// TestPlainDryRunNeverAsks: a rehearsal reads nothing from the input, for a red box.
func TestPlainDryRunNeverAsks(t *testing.T) {
	p, out, errb := newPlain("n\nn\n", true, true)
	if ok, err := p.Danger("ERASE", "Erase it?", "line"); !ok || err != nil {
		t.Errorf("danger: %v %v", ok, err)
	}
	if rest, _ := p.In.ReadString(0); rest != "n\nn\n" {
		t.Errorf("the rehearsal consumed input: %q", rest)
	}
	if errb.Len() != 0 || out.Len() == 0 {
		t.Errorf("out=%q err=%q", out.String(), errb.String())
	}
}
