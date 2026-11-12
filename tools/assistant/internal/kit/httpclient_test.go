package kit_test

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func fetchString(t *testing.T, c *kit.Client, u string) (string, error) {
	t.Helper()
	out := filepath.Join(t.TempDir(), "f")
	if err := kit.FetchLimit(context.Background(), c, u, out, 1<<20); err != nil {
		return "", err
	}
	b, _ := os.ReadFile(out)
	return string(b), nil
}

// get is one GET without the retries of FetchLimit (errors come back at once).
func get(t *testing.T, c *kit.Client, u string) (int, string, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	code, b, err := kit.Get(ctx, c, u, 1<<20)
	return code, string(b), err
}

type countConn struct {
	net.Conn
	n *atomic.Int64
}

func (c countConn) Read(p []byte) (int, error) {
	n, err := c.Conn.Read(p)
	c.n.Add(int64(n))
	return n, err
}

// readHead consumes a request head up to the blank line.
func readHead(br *bufio.Reader) {
	for {
		l, err := br.ReadString('\n')
		if err != nil || l == "\r\n" {
			return
		}
	}
}

// rawServer is a TLS server (the certificate of srv) that answers with whatever handler writes. The client
// counts the bytes it reads from the network in n.
func rawServer(t *testing.T, srv *httptest.Server, handler func(c net.Conn)) (c *kit.Client, u string, n *atomic.Int64) {
	t.Helper()
	ln, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{Certificates: srv.TLS.Certificates})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				readHead(bufio.NewReader(conn))
				handler(conn)
			}()
		}
	}()
	n = new(atomic.Int64)
	c = kittest.ClientFor(srv)
	c.DialContext = func(ctx context.Context, nw, addr string) (net.Conn, error) {
		raw, err := (&net.Dialer{}).DialContext(ctx, nw, addr)
		return countConn{raw, n}, err
	}
	return c, "https://" + ln.Addr().String() + "/x", n
}

// rawProxy is a plain TCP "proxy": handler runs after the CONNECT head was read.
func rawProxy(t *testing.T, handler func(c net.Conn)) *url.URL {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				readHead(bufio.NewReader(conn))
				handler(conn)
			}()
		}
	}()
	return &url.URL{Scheme: "http", Host: ln.Addr().String()}
}

func writeAll(c net.Conn, s string) { _, _ = io.WriteString(c, s) }

// endless writes unit forever (in blocks of about 8 KiB, so that the TLS overhead stays small) until the
// client closes the connection.
func endless(c net.Conn, unit string) {
	block := strings.Repeat(unit, 8192/len(unit)+1)
	for {
		if _, err := io.WriteString(c, block); err != nil {
			return
		}
	}
}

func TestClientDecodesChunkedAndGzip(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Accept-Encoding") != "identity" || r.Header.Get("Connection") != "close" {
			t.Errorf("unexpected request headers %v", r.Header)
		}
		fl := w.(http.Flusher)
		_, _ = io.WriteString(w, "chunk one, ")
		fl.Flush()
		_, _ = io.WriteString(w, "chunk two")
		fl.Flush()
	}))
	defer srv.Close()
	got, err := fetchString(t, kittest.ClientFor(srv), srv.URL+"/a?b=c")
	if err != nil || got != "chunk one, chunk two" {
		t.Fatalf("got %q, %v", got, err)
	}
}

func gz(t *testing.T, b []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	_, _ = zw.Write(b)
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func TestClientGzipAndDecompressionBomb(t *testing.T) {
	good := gz(t, []byte("hello gzip"))
	bomb := gz(t, make([]byte, 64<<20))
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Encoding", "gzip")
		if r.URL.Path == "/bomb" {
			_, _ = w.Write(bomb)
			return
		}
		_, _ = w.Write(good)
	}))
	defer srv.Close()
	c := kittest.ClientFor(srv)
	if got, err := fetchString(t, c, srv.URL+"/good"); err != nil || got != "hello gzip" {
		t.Fatalf("gzip body: %q, %v", got, err)
	}
	out := filepath.Join(t.TempDir(), "f")
	err := kit.FetchLimit(context.Background(), c, srv.URL+"/bomb", out, 1<<20)
	if !errors.Is(err, kit.ErrTooLarge) {
		t.Errorf("a decompression bomb must hit the size cap, got %v", err)
	}
	if _, err := os.Stat(out); err == nil {
		t.Error("the bomb was kept")
	}
}

func TestClientFollowsHTTPSRedirectsOnly(t *testing.T) {
	var hops atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/final", func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "arrived") })
	mux.HandleFunc("/start", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/final", http.StatusFound) })
	mux.HandleFunc("/loop", func(w http.ResponseWriter, r *http.Request) {
		hops.Add(1)
		http.Redirect(w, r, "/loop", http.StatusFound)
	})
	mux.HandleFunc("/down", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://127.0.0.1:1/x", http.StatusFound)
	})
	srv := httptest.NewTLSServer(mux)
	defer srv.Close()
	c := kittest.ClientFor(srv)
	if got, err := fetchString(t, c, srv.URL+"/start"); err != nil || got != "arrived" {
		t.Fatalf("relative redirect: %q, %v", got, err)
	}
	start := time.Now()
	if _, err := fetchString(t, c, srv.URL+"/down"); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("redirect to http: %v", err)
	}
	if _, err := fetchString(t, c, srv.URL+"/loop"); err == nil || !strings.Contains(err.Error(), "too many redirects") {
		t.Errorf("redirect loop: %v", err)
	}
	// Policy refusals are not retried: 10 requests for the loop, and no back-off wait.
	if n := hops.Load(); n != 10 {
		t.Errorf("%d requests, want 10 (no retry of a policy refusal)", n)
	}
	if d := time.Since(start); d > 2*time.Second {
		t.Errorf("policy refusals were retried (%v)", d)
	}
}

func TestClientStatusAndTruncation(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/missing", http.NotFound)
	srv := httptest.NewTLSServer(mux)
	defer srv.Close()
	c := kittest.ClientFor(srv)
	_, err := fetchString(t, c, srv.URL+"/missing")
	var he *kit.HTTPError
	if err == nil || !errors.As(err, &he) || he.Code != 404 {
		t.Errorf("404: %v", err)
	}
	c, u, _ := rawServer(t, srv, func(conn net.Conn) {
		writeAll(conn, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a little")
	})
	if _, _, err := get(t, c, u); err == nil || !strings.Contains(err.Error(), "unexpected EOF") {
		t.Errorf("a truncated body must be an error, got %v", err)
	}
}

// Only a 200 is the file: anything else is an HTTPError carrying the code.
func TestFetchAcceptsOnly200(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/302":
			w.WriteHeader(http.StatusFound) // no Location
		case "/204":
			w.WriteHeader(http.StatusNoContent)
		case "/206":
			w.WriteHeader(http.StatusPartialContent)
			_, _ = io.WriteString(w, "part")
		default:
			_, _ = io.WriteString(w, "whole")
		}
	}))
	defer srv.Close()
	c := kittest.ClientFor(srv)
	for path, code := range map[string]int{"/302": 302, "/204": 204, "/206": 206} {
		out := filepath.Join(t.TempDir(), "f")
		err := kit.FetchLimit(context.Background(), c, srv.URL+path, out, 1<<20)
		var he *kit.HTTPError
		if !errors.As(err, &he) || he.Code != code {
			t.Errorf("%s: want HTTPError %d, got %v", path, code, err)
		}
		if _, err := os.Stat(out); err == nil {
			t.Errorf("%s: a file was kept", path)
		}
	}
	if got, err := fetchString(t, c, srv.URL+"/ok"); err != nil || got != "whole" {
		t.Errorf("200: %q, %v", got, err)
	}
}

func TestClientTimeout(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-r.Context().Done():
		case <-time.After(10 * time.Second):
		}
	}))
	defer srv.Close()
	c := kittest.ClientFor(srv)
	c.Timeout = 300 * time.Millisecond
	start := time.Now()
	if _, _, err := get(t, c, srv.URL+"/slow"); err == nil {
		t.Fatal("a stalled server must time out")
	}
	if time.Since(start) > 5*time.Second {
		t.Errorf("timeout took %v", time.Since(start))
	}
}

// M1: nothing read before the body is unbounded, and the client reads at most about 1 MiB of such a head.
func TestClientBoundsTheAnswerHead(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	defer srv.Close()
	big := strings.Repeat("x", 2<<20)
	cases := map[string]func(c net.Conn){
		"huge status line":    func(c net.Conn) { writeAll(c, "HTTP/1.1 200 "+big) },
		"huge header":         func(c net.Conn) { writeAll(c, "HTTP/1.1 200 OK\r\nX: "+big+"\r\n\r\n") },
		"many headers":        func(c net.Conn) { writeAll(c, "HTTP/1.1 200 OK\r\n"+strings.Repeat("X-A: b\r\n", 400000)+"\r\n") },
		"endless 100":         func(c net.Conn) { endless(c, "HTTP/1.1 100 Continue\r\n\r\n") },
		"endless header line": func(c net.Conn) { writeAll(c, "HTTP/1.1 200 OK\r\nX: "); endless(c, "aaaaaaaaaaaaaaaa") },
		"huge chunk extension": func(c net.Conn) {
			writeAll(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;"+strings.Repeat("a", 100000)+"\r\nhello\r\n0\r\n\r\n")
		},
		"endless chunk size line": func(c net.Conn) {
			writeAll(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;")
			endless(c, "aaaaaaaaaaaaaaaa")
		},
		"huge trailer": func(c net.Conn) {
			writeAll(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n"+strings.Repeat("X: "+strings.Repeat("a", 1000)+"\r\n", 100)+"\r\n")
		},
		"endless small trailers": func(c net.Conn) {
			writeAll(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n")
			endless(c, "X: y\r\n")
		},
	}
	for name, h := range cases {
		t.Run(name, func(t *testing.T) {
			c, u, n := rawServer(t, srv, h)
			start := time.Now()
			_, _, err := get(t, c, u)
			if err == nil {
				t.Fatal("an oversized answer was accepted")
			}
			if d := time.Since(start); d > 10*time.Second {
				t.Errorf("took %v", d)
			}
			if got := n.Load(); got > 1<<20+256<<10 {
				t.Errorf("the client read %d bytes of the answer (err %v)", got, err)
			}
		})
	}
	t.Run("huge CONNECT answer", func(t *testing.T) {
		c := kittest.ClientFor(srv)
		n := new(atomic.Int64)
		px := rawProxy(t, func(conn net.Conn) { writeAll(conn, "HTTP/1.1 200 "+big) })
		c.Proxy = func(*url.URL) (*url.URL, error) { return px, nil }
		c.DialContext = func(ctx context.Context, nw, addr string) (net.Conn, error) {
			raw, err := (&net.Dialer{}).DialContext(ctx, nw, addr)
			return countConn{raw, n}, err
		}
		_, _, err := get(t, c, "https://example.com/x")
		if err == nil || !strings.HasPrefix(strings.TrimPrefix(err.Error(), `GET "https://example.com/x": `), "proxy:") {
			t.Errorf("got %v", err)
		}
		if got := n.Load(); got > 1<<20+4096 {
			t.Errorf("the client read %d bytes of the CONNECT answer", got)
		}
	})
	t.Run("endless CONNECT 100", func(t *testing.T) {
		c := kittest.ClientFor(srv)
		px := rawProxy(t, func(conn net.Conn) { endless(conn, "HTTP/1.1 100 Continue\r\n\r\n") })
		c.Proxy = func(*url.URL) (*url.URL, error) { return px, nil }
		if _, _, err := get(t, c, "https://example.com/x"); err == nil {
			t.Error("accepted")
		}
	})
}

// L5: strict parsing.
func TestClientParsesStrictly(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	defer srv.Close()
	ok := map[string]string{
		"HTTP/1.0 200 OK\r\n\r\nbody until close":                                                    "body until close",
		"HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nhi":                                                "hi",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n0\r\nX: t\r\n\r\n": "hello",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2 \r\nde\r\n0\r\n\r\n":     "abcde",
	}
	for resp, want := range ok {
		c, u, _ := rawServer(t, srv, func(conn net.Conn) { writeAll(conn, resp) })
		if code, got, err := get(t, c, u); err != nil || code != 200 || got != want {
			t.Errorf("%q: %d %q %v", resp, code, got, err)
		}
	}
	bad := map[string]string{
		"HTTP/2 200 OK\r\n\r\n":                                                  "version",
		"HTTP/1.2 200 OK\r\n\r\n":                                                "version",
		"HTTP/1.1 20 OK\r\n\r\n":                                                 "two digits",
		"HTTP/1.1 2000 OK\r\n\r\n":                                               "four digits",
		"HTTP/1.1 +20 OK\r\n\r\n":                                                "sign",
		"HTTP/1.1 200OK\r\n\r\n":                                                 "no space",
		"HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello":                     "CL sign",
		"HTTP/1.1 200 OK\r\nContent-Length: 0x5\r\n\r\nhello":                    "CL hex",
		"HTTP/1.1 200 OK\r\nContent-Length: 5 5\r\n\r\nhello":                    "CL space",
		"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n":                          "CL negative",
		"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello": "CL conflict",
		"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n": "CL and TE",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n":                                       "TE list",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n":                     "bad size",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-5\r\nhello\r\n0\r\n\r\n":                     "negative size",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\nhello\r\n0\r\n\r\n":                       "empty size",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n0\r\n\r\n":                        "bare LF size line",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n":                        "no CRLF after data",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n":                               "missing last chunk",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX: y\n\n":                  "bare LF trailer",
	}
	for resp, why := range bad {
		c, u, _ := rawServer(t, srv, func(conn net.Conn) { writeAll(conn, resp) })
		if code, got, err := get(t, c, u); err == nil {
			t.Errorf("%s: %q accepted (%d %q)", why, resp, code, got)
		}
	}
}

func fakeProxy(t *testing.T, ln net.Listener, upstream string, target, auth *atomic.Value) {
	t.Helper()
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				br := bufio.NewReader(c)
				line, _ := br.ReadString('\n')
				if f := strings.Fields(line); len(f) > 1 {
					target.Store(f[1])
				}
				for {
					h, err := br.ReadString('\n')
					if err != nil || h == "\r\n" {
						break
					}
					if strings.HasPrefix(h, "Proxy-Authorization:") {
						auth.Store(strings.TrimSpace(h))
					}
				}
				up, err := net.Dial("tcp", upstream)
				if err != nil {
					return
				}
				defer up.Close()
				_, _ = io.WriteString(c, "HTTP/1.1 200 Connection established\r\n\r\n")
				go func() { _, _ = io.Copy(up, br) }()
				_, _ = io.Copy(c, up)
			}()
		}
	}()
}

func TestClientUsesAConnectProxy(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "via proxy") }))
	defer srv.Close()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var target, auth atomic.Value
	fakeProxy(t, ln, srv.Listener.Addr().String(), &target, &auth)
	c := kittest.ClientFor(srv)
	c.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("http://user:pw@" + ln.Addr().String()) }
	got, err := fetchString(t, c, srv.URL+"/x")
	if err != nil || got != "via proxy" {
		t.Fatalf("got %q, %v", got, err)
	}
	if target.Load() != srv.Listener.Addr().String() {
		t.Errorf("CONNECT target %v", target.Load())
	}
	if a, _ := auth.Load().(string); !strings.HasSuffix(a, "dXNlcjpwdw==") {
		t.Errorf("proxy auth header %q", a)
	}
}

// The https:// proxy leg verifies the proxy's certificate against its host name with the client's TLS config.
func TestClientHTTPSProxyLeg(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "via tls proxy") }))
	defer srv.Close()
	ln, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{Certificates: srv.TLS.Certificates})
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var target, auth atomic.Value
	fakeProxy(t, ln, srv.Listener.Addr().String(), &target, &auth)
	_, port, _ := net.SplitHostPort(ln.Addr().String())
	c := kittest.ClientFor(srv)
	c.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("https://127.0.0.1:" + port) }
	if got, err := fetchString(t, c, srv.URL+"/x"); err != nil || got != "via tls proxy" {
		t.Fatalf("got %q, %v", got, err)
	}
	// "localhost" is not in the test certificate: the proxy's name must be checked.
	c.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("https://localhost:" + port) }
	if _, _, err := get(t, c, srv.URL+"/x"); err == nil || !strings.HasPrefix(strings.TrimPrefix(err.Error(), `GET "`+srv.URL+`/x": `), "proxy:") {
		t.Errorf("a proxy certificate for another name was accepted: %v", err)
	}
	// A client that does not trust the certificate refuses the proxy too.
	d := kit.NewHTTPClient()
	d.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("https://127.0.0.1:" + port) }
	if _, _, err := get(t, d, "https://example.com/x"); err == nil {
		t.Error("an untrusted proxy certificate was accepted")
	}
}

func TestClientRefusesAProxyThatRefuses(t *testing.T) {
	var tries atomic.Int32
	px := rawProxy(t, func(c net.Conn) { tries.Add(1); writeAll(c, "HTTP/1.1 403 Forbidden\r\n\r\n") })
	px.User = url.UserPassword("user", "SECRET")
	c := &kit.Client{Timeout: 5 * time.Second, TLSConfig: &tls.Config{}}
	c.Proxy = func(*url.URL) (*url.URL, error) { return px, nil }
	_, _, err := get(t, c, "https://example.invalid/x")
	if err == nil || !strings.Contains(err.Error(), "proxy: the tunnel was refused (HTTP 403)") {
		t.Errorf("got %v", err)
	}
	if err != nil && strings.Contains(err.Error(), "SECRET") {
		t.Errorf("the proxy password is in the error: %v", err)
	}
}

func TestProxyCredentialsNeverInErrors(t *testing.T) {
	c := &kit.Client{Timeout: 5 * time.Second}
	// A proxy address nobody listens on.
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	addr := ln.Addr().String()
	ln.Close()
	c.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("http://user:SECRET@" + addr) }
	if _, _, err := get(t, c, "https://example.invalid/x"); err == nil || strings.Contains(err.Error(), "SECRET") || !strings.Contains(err.Error(), "proxy:") {
		t.Errorf("dial error: %v", err)
	}
	for _, v := range []string{"http://user:SECRET@:8080", "socks5://user:SECRET@h:1080", "http://user:SECRET@[bad:8080", "user:SECRET@"} {
		t.Setenv("HTTPS_PROXY", v)
		_, err := kit.ProxyFromEnvironment(&url.URL{Scheme: "https", Host: "github.com"})
		if err == nil || strings.Contains(err.Error(), "SECRET") || !strings.HasPrefix(err.Error(), "proxy:") {
			t.Errorf("%q: %v", v, err)
		}
	}
	// A malformed proxy is reported at once, not retried.
	t.Setenv("HTTPS_PROXY", "socks5://h:1080")
	start := time.Now()
	out := filepath.Join(t.TempDir(), "f")
	err := kit.FetchLimit(context.Background(), &kit.Client{Proxy: kit.ProxyFromEnvironment}, "https://example.invalid/x", out, 1<<10)
	if err == nil || !strings.Contains(err.Error(), "SOCKS proxies are not supported") || time.Since(start) > 2*time.Second {
		t.Errorf("SOCKS setting: %v after %v", err, time.Since(start))
	}
}

// L1: cancelling reaches a proxy that accepts and never answers.
func TestClientCancelsWhileWaitingForTheProxy(t *testing.T) {
	stall := make(chan struct{})
	t.Cleanup(func() { close(stall) })
	px := rawProxy(t, func(net.Conn) { <-stall })
	c := &kit.Client{TLSConfig: &tls.Config{}, Proxy: func(*url.URL) (*url.URL, error) { return px, nil }}
	ctx, cancel := context.WithCancel(context.Background())
	go func() { time.Sleep(200 * time.Millisecond); cancel() }()
	start := time.Now()
	out := filepath.Join(t.TempDir(), "f")
	err := kit.FetchLimit(ctx, c, "https://example.invalid/x", out, 1<<10)
	if !errors.Is(err, context.Canceled) {
		t.Errorf("want context.Canceled, got %v", err)
	}
	if d := time.Since(start); d > 3*time.Second {
		t.Errorf("cancellation took %v", d)
	}
}

func TestClientCancelsWhileWaitingForTheServer(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { <-r.Context().Done() }))
	defer srv.Close()
	ctx, cancel := context.WithCancel(context.Background())
	go func() { time.Sleep(200 * time.Millisecond); cancel() }()
	start := time.Now()
	err := kit.FetchLimit(ctx, kittest.ClientFor(srv), srv.URL+"/x", filepath.Join(t.TempDir(), "f"), 1<<10)
	if !errors.Is(err, context.Canceled) || time.Since(start) > 3*time.Second {
		t.Errorf("got %v after %v", err, time.Since(start))
	}
}

// L4: TLS verification failures.
func TestClientVerifiesTheCertificate(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/hop", func(w http.ResponseWriter, r *http.Request) {
		_, p, _ := net.SplitHostPort(r.Host)
		http.Redirect(w, r, "https://wrong.test:"+p+"/f", http.StatusFound)
	})
	mux.HandleFunc("/f", func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "secret") })
	srv := httptest.NewTLSServer(mux)
	defer srv.Close()
	addr := srv.Listener.Addr().String()
	_, port, _ := net.SplitHostPort(addr)
	toSrv := func(ctx context.Context, nw, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, nw, addr)
	}
	c := kittest.ClientFor(srv)
	c.DialContext = toSrv

	// Right name (example.com is in the test certificate), then a redirect to a wrong one.
	if code, body, err := get(t, c, "https://example.com:"+port+"/f"); err != nil || code != 200 || body != "secret" {
		t.Fatalf("control: %d %q %v", code, body, err)
	}
	for name, u := range map[string]string{
		"wrong name":                "https://wrong.test:" + port + "/f",
		"wrong name after redirect": "https://example.com:" + port + "/hop",
	} {
		start := time.Now()
		out := filepath.Join(t.TempDir(), "f")
		err := kit.FetchLimit(context.Background(), c, u, out, 1<<10)
		if err == nil || !strings.Contains(err.Error(), "certificate") {
			t.Errorf("%s: got %v", name, err)
		}
		if _, e := os.Stat(out); e == nil {
			t.Errorf("%s: a file was written", name)
		}
		if time.Since(start) > 2*time.Second {
			t.Errorf("%s: a certificate failure was retried", name)
		}
	}

	// Through a proxy the origin certificate is still checked against the URL's name.
	pln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer pln.Close()
	var target, auth atomic.Value
	fakeProxy(t, pln, addr, &target, &auth)
	p := kittest.ClientFor(srv)
	p.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("http://" + pln.Addr().String()) }
	if _, _, err := get(t, p, "https://wrong.test:"+port+"/f"); err == nil || !strings.Contains(err.Error(), "certificate") {
		t.Errorf("through a proxy: %v", err)
	}
	if code, _, err := get(t, p, "https://example.com:"+port+"/f"); err != nil || code != 200 {
		t.Errorf("through a proxy, right name: %d %v", code, err)
	}
	if got := target.Load(); got != "example.com:"+port {
		t.Errorf("CONNECT target %v", got)
	}

	// A certificate the client does not trust.
	if _, _, err := get(t, kit.NewHTTPClient(), srv.URL+"/f"); err == nil {
		t.Error("an untrusted certificate was accepted")
	}
}

func TestClientRefusesTLS11(t *testing.T) {
	srv := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "old") }))
	srv.TLS = &tls.Config{MinVersion: tls.VersionTLS10, MaxVersion: tls.VersionTLS11}
	srv.StartTLS()
	defer srv.Close()
	if _, _, err := get(t, kittest.ClientFor(srv), srv.URL+"/x"); err == nil {
		t.Error("a TLS 1.1 server was accepted")
	}
}

func TestNewHTTPClientDefaults(t *testing.T) {
	c := kit.NewHTTPClient()
	if c.TLSConfig == nil || c.TLSConfig.MinVersion != tls.VersionTLS12 || c.TLSConfig.InsecureSkipVerify || c.TLSConfig.RootCAs != nil {
		t.Errorf("TLS config %+v", c.TLSConfig)
	}
	if c.Timeout != 15*time.Minute || c.Proxy == nil {
		t.Errorf("timeout %v, proxy set: %v", c.Timeout, c.Proxy != nil)
	}
	if _, _, err := get(t, &kit.Client{}, "http://127.0.0.1:1/x"); err == nil {
		t.Error("plain HTTP accepted")
	}
}

func TestProxyFromEnvironment(t *testing.T) {
	for _, k := range []string{"HTTPS_PROXY", "https_proxy", "NO_PROXY", "no_proxy"} {
		t.Setenv(k, "")
	}
	get := func(raw string) (string, error) {
		u, _ := url.Parse(raw)
		p, err := kit.ProxyFromEnvironment(u)
		if p == nil {
			return "", err
		}
		return p.String(), err
	}
	if p, _ := get("https://github.com/x"); p != "" {
		t.Errorf("no variable, proxy %q", p)
	}
	t.Setenv("HTTPS_PROXY", "proxy.corp:3128")
	if p, _ := get("https://github.com/x"); p != "http://proxy.corp:3128" {
		t.Errorf("scheme-less proxy: %q", p)
	}
	for _, loopback := range []string{"https://localhost/x", "https://127.0.0.1:8443/x", "https://[::1]/x"} {
		if p, _ := get(loopback); p != "" {
			t.Errorf("%s must not use the proxy", loopback)
		}
	}
	t.Setenv("NO_PROXY", ".corp.example, 10.0.0.0/8, internal.test:8443, other.test")
	for raw, want := range map[string]bool{
		"https://a.corp.example/x":   false,
		"https://corp.example/x":     false,
		"https://10.1.2.3/x":         false,
		"https://11.1.2.3/x":         true,
		"https://internal.test:8443": false,
		"https://internal.test":      true,
		"https://sub.other.test/x":   false,
		"https://notother.test/x":    true,
		"https://github.com/x":       true,
	} {
		if p, _ := get(raw); (p != "") != want {
			t.Errorf("%s: proxied=%v, want %v", raw, p != "", want)
		}
	}
	// A URL without a port is on 443.
	t.Setenv("NO_PROXY", "github.com:443")
	if p, _ := get("https://github.com/x"); p != "" {
		t.Errorf("NO_PROXY=github.com:443 did not exclude https://github.com: %q", p)
	}
	if p, _ := get("https://github.com:8443/x"); p == "" {
		t.Error("NO_PROXY=github.com:443 excluded port 8443")
	}
	t.Setenv("HTTPS_PROXY", "socks5://p:1080")
	t.Setenv("NO_PROXY", "")
	if _, err := get("https://github.com/x"); err == nil {
		t.Error("a SOCKS proxy must be reported, not silently ignored")
	}
}
