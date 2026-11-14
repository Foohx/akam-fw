package telnet_test

import (
	"net"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/telnet"
)

// A camera that streams far more than MaxLine bytes without a line end must neither hang the
// session nor make it spend quadratic time: the line is capped and the session ends with an error.
func TestEndlessLineIsCappedAndFast(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		junk := []byte(strings.Repeat("x", 1<<16))
		for i := 0; i < 64; i++ { // 4 MiB, no newline
			if _, err := c.Write(junk); err != nil {
				return
			}
		}
	}()
	s := telnet.New(telnet.Config{Host: "127.0.0.1", Port: ln.Addr().(*net.TCPAddr).Port, Timeout: 500 * time.Millisecond, Warn: func(string) {}})
	if err := s.Open(); err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	start := time.Now()
	_ = s.Exec("true", 0)
	if d := time.Since(start); d > 5*time.Second {
		t.Errorf("took %v", d)
	}
}
