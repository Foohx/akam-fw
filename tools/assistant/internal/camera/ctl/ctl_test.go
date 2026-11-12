package ctl

import (
	"strings"
	"testing"
)

func TestInputEndsWithQuit(t *testing.T) {
	if got := Input([]string{"GET a", "SET b 1"}); got != "GET a\nSET b 1\nQUIT\n" {
		t.Errorf("input: %q", got)
	}
}

func TestAnswersDropsLogByeAndBlank(t *testing.T) {
	out := Answers("LOG [early] hello\r\nled.status=auto\r\n\nLOG [x]\nnight.ir_led=off\nOK bye\n", 2)
	if len(out) != 2 || Get("led.status", out) != "auto" || Get("night.ir_led", out) != "off" {
		t.Errorf("answers: %q", out)
	}
	// a missing answer: the QUIT reply must not take its place
	if out = Answers("led.status=on\nOK bye\n", 2); len(out) != 1 {
		t.Errorf("answers: %q", out)
	}
	if out = Answers("", 1); len(out) != 0 {
		t.Errorf("answers: %q", out)
	}
}

func TestGetTakesTheValueUpToTheFirstSpace(t *testing.T) {
	ans := []string{"audio.out_state=idle (since 3 s)", "other=1", "led.status=auto"}
	if Get("audio.out_state", ans) != "idle" || Get("led.status", ans) != "auto" || Get("missing", ans) != "" {
		t.Error("Get")
	}
	if Get("led", []string{"led.status=auto"}) != "" {
		t.Error("a prefix of a key must not match")
	}
}

func TestParseSet(t *testing.T) {
	cases := []struct {
		in             string
		ok, ns, rs, an bool
		err            string
	}{
		{"OK audio.out_volume=30", true, false, false, true, ""},
		{"OK audio.out_volume=30 (not saved)", true, true, false, true, ""},
		{"OK audio.out_enable=1 (restart to apply)", true, false, true, true, ""},
		{"ERR out of range", false, false, false, true, "out of range"},
		{"garbage", false, false, false, false, ""},
	}
	for _, c := range cases {
		r := ParseSet([]string{c.in})
		if r.OK != c.ok || r.NotSaved != c.ns || r.Restart != c.rs || r.Answered != c.an || r.Err != c.err {
			t.Errorf("%q: %+v", c.in, r)
		}
	}
	if r := ParseSet(nil); r.Answered {
		t.Error("no answer")
	}
	r := ParseLedSet("led.status", "on", []string{"OK led.status=on"})
	if !r.OK {
		t.Error("led ok")
	}
	if r := ParseLedSet("led.status", "on", []string{"ERR unknown param led.status"}); r.OK || !r.Answered || !strings.Contains(r.Err, "led.status") {
		t.Errorf("led err: %+v", r)
	}
	if r := ParseLedSet("led.status", "on", []string{"OK led.status=off"}); r.Answered {
		t.Error("an OK for another value is not a confirmation")
	}
}
