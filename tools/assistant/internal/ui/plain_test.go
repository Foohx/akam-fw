package ui

import (
	"bufio"
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

func newPlain(in string, tty, dry bool) (*Plain, *bytes.Buffer, *bytes.Buffer) {
	var out, errb bytes.Buffer
	return &Plain{In: bufio.NewReader(strings.NewReader(in)), Out: &out, ErrOut: &errb, ForceTT: tty, DryRun: dry,
		Secrets: &state.Secrets{}, progLast: -1}, &out, &errb
}

// Every yes/no defaults to No: Enter, n, q, Esc, junk, end of input.
func TestYesNoDefaultsToNo(t *testing.T) {
	for _, in := range []string{"", "\n", "n\n", "N\n", "q\n", "\x1b\n", "nope\n", "yy\n", "ye\n", " y\n", "1\n"} {
		p, _, _ := newPlain(in, true, false)
		ok, _ := p.AskYN("Do it?")
		if ok {
			t.Errorf("input %q answered yes", in)
		}
	}
	for _, in := range []string{"y\n", "Y\n", "yes\n", "YES\n", "Yes\n"} {
		p, _, errb := newPlain(in, true, false)
		ok, err := p.AskYN("Do it?")
		if !ok || err != nil {
			t.Errorf("input %q: %v %v", in, ok, err)
		}
		if !strings.Contains(errb.String(), "Do it? [y/N] ") {
			t.Errorf("prompt: %q", errb.String())
		}
	}
}

func TestAskWithoutTerminalRefusesAndNeverConfirms(t *testing.T) {
	p, _, errb := newPlain("y\n", false, false)
	ok, err := p.AskYN("Do it?")
	if ok || !errors.Is(err, ErrNoTTY) {
		t.Fatalf("%v %v", ok, err)
	}
	if !strings.Contains(errb.String(), "refusing to ask 'Do it?' without a terminal") {
		t.Errorf("message: %q", errb.String())
	}
}

func TestDangerRefusesWithoutTerminalAndDefaultsToNo(t *testing.T) {
	p, out, errb := newPlain("y\n", false, false)
	ok, err := p.Danger("ERASE", "Erase it?", "line one")
	if ok || err == nil {
		t.Fatal("danger accepted without a terminal")
	}
	if !strings.Contains(errb.String(), "ERASE needs an interactive terminal and refuses to run without one.") || out.String() == "" {
		t.Errorf("out=%q err=%q", out.String(), errb.String())
	}
	p, out, _ = newPlain("\n", true, false)
	ok, _ = p.Danger("ERASE", "Erase it?", "line one", "line two")
	if ok {
		t.Fatal("Enter means yes")
	}
	for _, want := range []string{"WARNING: ERASE", "  line one", "  line two"} {
		if !strings.Contains(out.String(), want) {
			t.Errorf("box lacks %q: %s", want, out.String())
		}
	}
	// a rehearsal only shows the question
	p, out, _ = newPlain("", false, true)
	if ok, err := p.Danger("ERASE", "Erase it?", "x"); !ok || err != nil || !strings.Contains(out.String(), "WOULD ask: Erase it? [y/N] (default: No) -- ERASE: x") {
		t.Errorf("rehearsal: %v %v %q", ok, err, out.String())
	}
}

func TestPromptSecretPick(t *testing.T) {
	p, _, errb := newPlain("\nhello\n", true, false)
	if v, err := p.Prompt("Name:", "dflt"); v != "dflt" || err != nil {
		t.Errorf("default: %q %v", v, err)
	}
	if v, _ := p.Prompt("Name:", "dflt"); v != "hello" {
		t.Error("typed value")
	}
	if !strings.Contains(errb.String(), "Name: ") {
		t.Errorf("prompt text %q", errb.String())
	}
	if _, err := p.Prompt("x", "d"); !errors.Is(err, ErrNoInput) {
		t.Error("end of input")
	}
	// a rehearsal with no input takes the default
	p, _, _ = newPlain("", false, true)
	if v, err := p.Prompt("x", "d"); v != "d" || err != nil {
		t.Errorf("rehearsal default: %q %v", v, err)
	}
	// a secret is registered so that no log or state can hold it
	p, _, _ = newPlain("s3cr3t-value\n", true, false)
	v, _ := p.Secret("Password:")
	if v != "s3cr3t-value" || !p.Secrets.Has("xx s3cr3t-value yy") {
		t.Error("secret not registered")
	}
	p, out, _ := newPlain("9\n0\n2\n", true, false)
	i, err := p.Pick("Which?", []string{"a", "b", "c"})
	if i != 1 || err != nil {
		t.Errorf("pick: %d %v", i, err)
	}
	if !strings.Contains(out.String(), "    1) a") || !strings.Contains(out.String(), "  Please type a number between 1 and 3.") {
		t.Errorf("list: %s", out.String())
	}
	p, _, _ = newPlain("\n", true, false)
	if i, _ := p.Pick("Which?", []string{"a", "b"}); i != 0 {
		t.Error("Enter picks the first")
	}
}

func TestOutputStreamsAndColour(t *testing.T) {
	p, out, errb := newPlain("", true, false)
	p.Say("hello")
	p.Warn("careful")
	p.Good("fine")
	p.Hint("do this")
	if err := p.Fail("broke", "fix it"); !errors.Is(err, ErrFailed) {
		t.Error("Fail must return ErrFailed")
	}
	if out.String() != "hello\nWARNING: careful\nfine\n  What to do: do this\n  What to do: fix it\n" {
		t.Errorf("stdout: %q", out.String())
	}
	if errb.String() != "ERROR: broke\n" {
		t.Errorf("stderr: %q", errb.String())
	}
	p, out, _ = newPlain("", true, false)
	p.Color = true
	p.Good("fine")
	p.Bold("title")
	if out.String() != "\x1b[32mfine\x1b[0m\n\x1b[1mtitle\x1b[0m\n" {
		t.Errorf("colour: %q", out.String())
	}
}

func TestProgressPrintsEveryTenPercent(t *testing.T) {
	p, out, _ := newPlain("", true, false)
	for i := 0; i <= 100; i++ {
		p.Progress("Writing", i)
	}
	if got := strings.Count(out.String(), "\n"); got != 11 {
		t.Errorf("%d lines: %s", got, out.String())
	}
}
