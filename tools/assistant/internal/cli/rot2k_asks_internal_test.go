package cli

import (
	"bufio"
	"bytes"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The three questions of a live 2K update, as the CLI asks them on a (fake) terminal. Every wrong answer must be a
// refusal: the typed confirmation is the only thing standing between a keystroke and a flash write.

const asksSHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func asksApp(input string, yes, atHand, tty bool) (*App, *bytes.Buffer, *bufio.Reader) {
	in := bufio.NewReader(strings.NewReader(input))
	var out bytes.Buffer
	u := &ui.Plain{In: in, Out: &out, ErrOut: &out, ForceTT: tty}
	return &App{UI: u, Opt: &Options{Yes: yes, RecoveryAtHand: atHand}}, &out, in
}

func TestConfirmWriteNeedsTheExactTypedString(t *testing.T) {
	want := asksSHA[:12] + " mtd3"
	for _, c := range []struct {
		name, typed string
		ok          bool
	}{
		{"the exact string", want + "\n", true},
		{"surrounding blanks are not part of the string", "  " + want + "  \n", true},
		{"an empty answer", "\n", false},
		{"yes", "yes\n", false},
		{"y", "y\n", false},
		{"only the hash", asksSHA[:12] + "\n", false},
		{"the full hash and the partition", asksSHA + " mtd3\n", false},
		{"another partition", asksSHA[:12] + " mtd2\n", false},
		{"another image", "fedcba987654 mtd3\n", false},
		{"upper case", strings.ToUpper(want) + "\n", false},
		{"one digit short", asksSHA[:11] + " mtd3\n", false},
		{"two spaces inside", asksSHA[:12] + "  mtd3\n", false},
		{"the string with a suffix", want + "x\n", false},
	} {
		t.Run(c.name, func(t *testing.T) {
			a, _, _ := asksApp(c.typed, false, false, true)
			ok, err := a.runDeps2K("192.0.2.10").Asks.ConfirmWrite(asksSHA)
			if err != nil || ok != c.ok {
				t.Fatalf("typed %q: ok=%v err=%v, want ok=%v", c.typed, ok, err, c.ok)
			}
		})
	}
	t.Run("end of input is an error and a refusal", func(t *testing.T) {
		a, _, _ := asksApp("", false, false, true)
		if ok, err := a.runDeps2K("192.0.2.10").Asks.ConfirmWrite(asksSHA); ok || err == nil {
			t.Fatalf("ok=%v err=%v", ok, err)
		}
	})
}

func TestRecoveryAndRehearsalQuestionsDefaultToNo(t *testing.T) {
	for _, c := range []struct {
		name, typed string
		ok          bool
	}{{"empty", "\n", false}, {"n", "n\n", false}, {"no", "no\n", false}, {"garbage", "ok sure\n", false}, {"y", "y\n", true}, {"yes", "yes\n", true}} {
		t.Run("recovery "+c.name, func(t *testing.T) {
			a, out, _ := asksApp(c.typed, false, false, true)
			ok, err := a.runDeps2K("192.0.2.10").Asks.AckRecovery()
			if err != nil || ok != c.ok {
				t.Fatalf("ok=%v err=%v", ok, err)
			}
			if !strings.Contains(out.String(), "RECOVERY MEANS") {
				t.Errorf("the red box was not shown:\n%s", out)
			}
		})
		t.Run("rehearsal "+c.name, func(t *testing.T) {
			a, _, _ := asksApp(c.typed, false, false, true)
			ok, err := a.runDeps2K("192.0.2.10").Asks.ConfirmRehearsal()
			if err != nil || ok != c.ok {
				t.Fatalf("ok=%v err=%v", ok, err)
			}
		})
	}
}

// --yes answers the rehearsal and the typed question; it never answers the recovery question alone.
func TestYesFlagCombinations(t *testing.T) {
	t.Run("--yes alone: the recovery red box is still asked, and No stops it", func(t *testing.T) {
		a, out, _ := asksApp("n\n", true, false, true)
		ok, err := a.runDeps2K("192.0.2.10").Asks.AckRecovery()
		if err != nil || ok {
			t.Fatalf("ok=%v err=%v", ok, err)
		}
		if !strings.Contains(out.String(), "RECOVERY MEANS") {
			t.Fatal("the red box was skipped by --yes")
		}
	})
	t.Run("--yes alone: an empty answer to the red box is No", func(t *testing.T) {
		a, _, _ := asksApp("\n", true, false, true)
		if ok, err := a.runDeps2K("192.0.2.10").Asks.AckRecovery(); err != nil || ok {
			t.Fatalf("ok=%v err=%v", ok, err)
		}
	})
	t.Run("--yes --recovery-at-hand on a terminal: the red box is not asked, nothing is read", func(t *testing.T) {
		a, out, in := asksApp("untouched\n", true, true, true)
		d := a.runDeps2K("192.0.2.10")
		ok, err := d.Asks.AckRecovery()
		if err != nil || !ok {
			t.Fatalf("ok=%v err=%v", ok, err)
		}
		if !strings.Contains(out.String(), "--recovery-at-hand") {
			t.Error("the statement made with the flag was not shown")
		}
		if rest, _ := in.ReadString('\n'); rest != "untouched\n" {
			t.Errorf("the flag consumed input: %q", rest)
		}
		if ok, err := d.Asks.ConfirmRehearsal(); err != nil || !ok {
			t.Fatalf("rehearsal with --yes: %v %v", ok, err)
		}
		if ok, err := d.Asks.ConfirmWrite(asksSHA); err != nil || !ok {
			t.Fatalf("write with --yes: %v %v", ok, err)
		}
	})
	t.Run("--recovery-at-hand without --yes does not skip the red box", func(t *testing.T) {
		a, out, _ := asksApp("n\n", false, true, true)
		ok, err := a.runDeps2K("192.0.2.10").Asks.AckRecovery()
		if err != nil || ok || !strings.Contains(out.String(), "RECOVERY MEANS") {
			t.Fatalf("ok=%v err=%v\n%s", ok, err, out)
		}
	})
	t.Run("no terminal: the red box refuses, with or without the flags", func(t *testing.T) {
		for _, c := range [][2]bool{{false, false}, {true, false}} {
			a, _, _ := asksApp("y\n", c[0], c[1], false)
			if ok, err := a.runDeps2K("192.0.2.10").Asks.AckRecovery(); ok || err == nil {
				t.Fatalf("yes=%v atHand=%v: ok=%v err=%v", c[0], c[1], ok, err)
			}
		}
	})
}
