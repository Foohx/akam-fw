package kit_test

import (
	"bufio"
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
	if _, err := fetchString(t, c, srv.URL+"/down"); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("redirect to http: %v", err)
	}
	if _, err := fetchString(t, c, srv.URL+"/loop"); err == nil || !strings.Contains(err.Error(), "too many redirects") {
		t.Errorf("redirect loop: %v", err)
	}
	// FetchLimit retries a failed download 4 times in all; each attempt makes 10 requests.
	if n := hops.Load(); n != 40 {
		t.Errorf("%d requests before giving up, want 40 (4 attempts of 10)", n)
	}
}

func TestClientStatusAndTruncation(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/missing", http.NotFound)
	mux.HandleFunc("/short", func(w http.ResponseWriter, r *http.Request) {
		hj := w.(http.Hijacker)
		conn, buf, _ := hj.Hijack()
		_, _ = buf.WriteString("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a little")
		_ = buf.Flush()
		conn.Close()
	})
	srv := httptest.NewTLSServer(mux)
	defer srv.Close()
	c := kittest.ClientFor(srv)
	_, err := fetchString(t, c, srv.URL+"/missing")
	var he *kit.HTTPError
	if err == nil || !errors.As(err, &he) || he.Code != 404 {
		t.Errorf("404: %v", err)
	}
	if _, err := fetchString(t, c, srv.URL+"/short"); err == nil || !strings.Contains(err.Error(), "unexpected EOF") {
		t.Errorf("a truncated body must be an error, got %v", err)
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
	if _, err := fetchString(t, c, srv.URL+"/slow"); err == nil {
		t.Fatal("a stalled server must time out")
	}
	if time.Since(start) > 5*time.Second {
		t.Errorf("timeout took %v", time.Since(start))
	}
}

// A fake HTTP proxy: answers CONNECT, then relays the bytes to the real address.
func TestClientUsesAConnectProxy(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, "via proxy") }))
	defer srv.Close()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var target, auth atomic.Value
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
				target.Store(strings.Fields(line)[1])
				for {
					h, err := br.ReadString('\n')
					if err != nil || h == "\r\n" {
						break
					}
					if strings.HasPrefix(h, "Proxy-Authorization:") {
						auth.Store(strings.TrimSpace(h))
					}
				}
				up, err := net.Dial("tcp", srv.Listener.Addr().String())
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

func TestClientRefusesAProxyThatRefuses(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go func() { // the download retries a failed tunnel: answer every attempt
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			_, _ = bufio.NewReader(c).ReadString('\n')
			_, _ = io.WriteString(c, "HTTP/1.1 403 Forbidden\r\n\r\n")
			c.Close()
		}
	}()
	c := &kit.Client{Timeout: 5 * time.Second, TLSConfig: &tls.Config{}}
	c.Proxy = func(*url.URL) (*url.URL, error) { return url.Parse("http://" + ln.Addr().String()) }
	if _, err := fetchString(t, c, "https://example.invalid/x"); err == nil || !strings.Contains(err.Error(), "refused the tunnel") {
		t.Errorf("got %v", err)
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
	t.Setenv("HTTPS_PROXY", "socks5://p:1080")
	t.Setenv("NO_PROXY", "")
	if _, err := get("https://github.com/x"); err == nil {
		t.Error("a SOCKS proxy must be reported, not silently ignored")
	}
}
