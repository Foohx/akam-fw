package ctl

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"
)

// serve runs handler on a real loopback connection (a net.Pipe has no buffer, so a server that
// speaks first would deadlock against a client that writes first).
func serve(t *testing.T, handler func(c net.Conn)) net.Conn {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		defer ln.Close()
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		handler(c)
	}()
	a, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { a.Close(); ln.Close() })
	return a
}

func TestExchangeDropsLogLinesAndStopsAfterOneAnswerPerLine(t *testing.T) {
	conn := serve(t, func(c net.Conn) {
		io := bufio.NewReader(c)
		c.Write([]byte("LOG [early] hello\n"))
		l1, _ := io.ReadString('\n')
		c.Write([]byte("LOG [middle]\n"))
		l2, _ := io.ReadString('\n')
		if l1 != "GET led.status\n" || l2 != "GET night.ir_led\n" {
			c.Write([]byte("ERR got " + l1 + l2))
			return
		}
		c.Write([]byte("led.status=auto\nLOG [x]\nnight.ir_led=off\n"))
		time.Sleep(time.Second) // the camera keeps the connection open
	})
	start := time.Now()
	out := Exchange(conn, []string{"GET led.status", "GET night.ir_led"}, 2*time.Second)
	if time.Since(start) > 900*time.Millisecond {
		t.Error("waited for the idle timeout although every answer had arrived")
	}
	if len(out) != 2 || Get("led.status", out) != "auto" || Get("night.ir_led", out) != "off" {
		t.Errorf("answers: %v", out)
	}
}

func TestExchangeToleratesSilenceAndEarlyClose(t *testing.T) {
	conn := serve(t, func(c net.Conn) { time.Sleep(300 * time.Millisecond) }) // never answers
	start := time.Now()
	out := Exchange(conn, []string{"GET led.status"}, 100*time.Millisecond)
	if len(out) != 0 || time.Since(start) > time.Second {
		t.Errorf("out=%v after %v", out, time.Since(start))
	}
	conn = serve(t, func(c net.Conn) {
		bufio.NewReader(c).ReadString('\n')
		c.Write([]byte("led.status=on\n"))
	}) // closes after one answer out of two
	out = Exchange(conn, []string{"GET led.status", "GET led.state"}, time.Second)
	if len(out) != 1 || Get("led.status", out) != "on" || Get("led.state", out) != "" {
		t.Errorf("out=%v", out)
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
