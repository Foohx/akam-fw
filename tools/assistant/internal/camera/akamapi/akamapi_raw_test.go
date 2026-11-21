package akamapi

import (
	"context"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// The bounds of the raw HTTP client (T4): each test below fails when the guard it names is removed.

// serve runs one connection handler on a loopback listener and returns its address.
func serve(t *testing.T, handle func(c net.Conn)) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip("skip: no loopback")
	}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() { defer c.Close(); handle(c) }()
		}
	}()
	return ln.Addr().String()
}

func readRequest(c net.Conn) {
	buf := make([]byte, 4096)
	_ = c.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _ = c.Read(buf)
}

// within runs f and fails the test when it takes longer than d (a guard that bounds the exchange is missing).
func within(t *testing.T, d time.Duration, what string, f func()) {
	t.Helper()
	done := make(chan struct{})
	go func() { f(); close(done) }()
	select {
	case <-done:
	case <-time.After(d):
		t.Fatalf("%s: not bounded (still running after %v)", what, d)
	}
}

// An answer that never ends is cut at maxBody+8 KiB: the call returns at once, long before the 5 s deadline.
func TestRawGetReadsAtMostTheBound(t *testing.T) {
	var sent atomic.Int64
	addr := serve(t, func(c net.Conn) {
		readRequest(c)
		_, _ = c.Write([]byte("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"))
		chunk := []byte(strings.Repeat("a", 8192))
		for {
			n, err := c.Write(chunk)
			sent.Add(int64(n))
			if err != nil {
				return
			}
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	start := time.Now()
	var body []byte
	var err error
	within(t, 4*time.Second, "an endless answer", func() { _, body, err = rawGet(ctx, addr, "/akam/v1/info", nil) })
	if err != nil {
		t.Fatal(err)
	}
	if len(body) > maxBody {
		t.Fatalf("the body is %d bytes, the bound is %d", len(body), maxBody)
	}
	if time.Since(start) > 2*time.Second {
		t.Fatalf("took %v: the read was not cut at the bound", time.Since(start))
	}
}

// A 1 MiB answer with a Content-Length is truncated to maxBody, never returned whole.
func TestRawGetTruncatesAnOversizedBody(t *testing.T) {
	addr := serve(t, func(c net.Conn) {
		readRequest(c)
		_, _ = c.Write([]byte("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\nConnection: close\r\n\r\n"))
		_, _ = c.Write([]byte(strings.Repeat("b", 1<<20)))
	})
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, body, err := rawGet(ctx, addr, "/x", nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(body) > maxBody {
		t.Fatalf("a %d bytes body came back, the bound is %d", len(body), maxBody)
	}
}

// A server that accepts and then says nothing, and one that sends a head and keeps the connection open, are
// cut by the deadline of the whole exchange.
func TestRawGetDeadlineCutsASilentOrSlowServer(t *testing.T) {
	for name, handle := range map[string]func(c net.Conn){
		"silent": func(c net.Conn) { readRequest(c); time.Sleep(10 * time.Second) },
		"head and then nothing": func(c net.Conn) {
			readRequest(c)
			_, _ = c.Write([]byte("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{"))
			time.Sleep(10 * time.Second)
		},
	} {
		t.Run(name, func(t *testing.T) {
			addr := serve(t, handle)
			ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
			defer cancel()
			start := time.Now()
			within(t, 3*time.Second, name, func() { _, _, _ = rawGet(ctx, addr, "/x", nil) })
			if time.Since(start) > 2*time.Second {
				t.Fatalf("took %v with a 300 ms deadline", time.Since(start))
			}
		})
	}
}

func TestRawGetRefusesMalformedAnswers(t *testing.T) {
	for name, c := range map[string]struct{ reply, want string }{
		"no end of header":    {"HTTP/1.1 200 OK\r\nX: y", "no HTTP answer"},
		"not http":            {"SSH-2.0-dropbear\r\n\r\n", "not an HTTP answer"},
		"no status":           {"HTTP/1.1 \r\n\r\n", "not an HTTP answer"},
		"chunked":             {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n", "chunked"},
		"chunked any case":    {"HTTP/1.1 200 OK\r\ntransfer-ENCODING: Chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n", "chunked"},
		"status not a number": {"HTTP/1.1 abc OK\r\n\r\n", "not an HTTP answer"},
	} {
		t.Run(name, func(t *testing.T) {
			addr := serve(t, func(conn net.Conn) { readRequest(conn); _, _ = conn.Write([]byte(c.reply)) })
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			code, body, err := rawGet(ctx, addr, "/x", nil)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("want error %q, got code %d body %q err %v", c.want, code, body, err)
			}
			if name == "chunked" && body != nil {
				t.Fatal("a chunked body was returned")
			}
		})
	}
}

// CR, LF or a blank in the host or the path (they are written into the request line and the Host header), and in a
// header name or value, are refused before a single byte is sent.
func TestRawGetRefusesInjectionInTheRequest(t *testing.T) {
	var got atomic.Int64
	addr := serve(t, func(c net.Conn) {
		buf := make([]byte, 4096)
		_ = c.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, _ := c.Read(buf)
		got.Add(int64(n))
	})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	for name, c := range map[string]struct {
		host, path string
		h          Header
	}{
		"CRLF in the path":     {addr, "/akam/v1/info HTTP/1.1\r\nX-Evil: 1\r\n\r\nGET /", nil},
		"LF in the path":       {addr, "/x\nX-Evil: 1", nil},
		"blank in the path":    {addr, "/x HTTP/1.0", nil},
		"CRLF in the host":     {addr + "\r\nX-Evil: 1", "/x", nil},
		"CR in a header value": {addr, "/x", Header{"X-Akam-Key": "a\r\nX-Evil: 1"}},
		"LF in a header name":  {addr, "/x", Header{"X-A\nB": "v"}},
		"empty path":           {addr, "", nil},
	} {
		t.Run(name, func(t *testing.T) {
			if _, _, err := rawGet(ctx, c.host, c.path, c.h); err == nil {
				t.Fatal("accepted")
			}
		})
	}
	time.Sleep(700 * time.Millisecond)
	if n := got.Load(); n != 0 {
		t.Fatalf("%d bytes reached the server for refused requests", n)
	}
}
