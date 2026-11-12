package kit

import (
	"bufio"
	"compress/gzip"
	"context"
	"crypto/tls"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"net/textproto"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

// Client is the minimal HTTPS GET client of the downloads. It replaces net/http, which links about 1.6 MiB
// (HTTP/1 and proxy machinery, IDNA and Unicode normalisation tables) for what is one GET with a redirect
// rule. HTTP/1.1 only, one connection per request, no cookies, no cache. A proxy (HTTPS_PROXY, NO_PROXY)
// is reached through an HTTP CONNECT tunnel; SOCKS proxies are not supported.
type Client struct {
	// Timeout bounds a whole download (connection, headers and body); 0: none.
	Timeout time.Duration
	// TLSConfig is cloned for each connection; ServerName is set from the URL.
	TLSConfig *tls.Config
	// Proxy returns the proxy to use for u, or nil for a direct connection.
	Proxy func(u *url.URL) (*url.URL, error)
	// DialContext opens the TCP connection (to the host or to the proxy); nil: a plain net.Dialer.
	DialContext func(ctx context.Context, network, addr string) (net.Conn, error)
}

// NewHTTPClient is the download client: HTTPS only (also for redirects), TLS 1.2 or newer, and a
// generous total timeout (the kit is a few tens of MB).
func NewHTTPClient() *Client {
	return &Client{
		Timeout:   15 * time.Minute,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12},
		Proxy:     ProxyFromEnvironment,
	}
}

const maxRedirects = 10

// httpResponse is one answer: Body is the decoded entity, ContentLength -1 when unknown.
type httpResponse struct {
	StatusCode    int
	ContentLength int64
	Body          io.ReadCloser
}

// get fetches rawURL with GET, following redirects under the HTTPS-only rule.
func (c *Client) get(ctx context.Context, rawURL string) (*httpResponse, error) {
	cancel := context.CancelFunc(func() {})
	if c.Timeout > 0 {
		ctx, cancel = context.WithTimeout(ctx, c.Timeout)
	}
	u, err := url.Parse(rawURL)
	if err != nil {
		cancel()
		return nil, err
	}
	for n := 1; ; n++ {
		resp, loc, err := c.once(ctx, u)
		if err != nil {
			cancel()
			return nil, fmt.Errorf("GET %q: %w", u.Redacted(), err)
		}
		if loc == "" {
			resp.Body = &cancelOnClose{resp.Body, cancel}
			return resp, nil
		}
		resp.Body.Close()
		next, err := u.Parse(loc)
		if err == nil {
			err = checkRedirect(next, n)
		}
		if err != nil {
			cancel()
			return nil, fmt.Errorf("GET %q: %w", u.Redacted(), err)
		}
		u = next
	}
}

func checkRedirect(next *url.URL, requests int) error {
	if next.Scheme != "https" {
		return errors.New("refusing a redirect to a non-HTTPS address")
	}
	if requests >= maxRedirects {
		return errors.New("too many redirects")
	}
	return nil
}

type cancelOnClose struct {
	io.ReadCloser
	cancel context.CancelFunc
}

func (c *cancelOnClose) Close() error {
	err := c.ReadCloser.Close()
	c.cancel()
	return err
}

type connBody struct {
	io.Reader
	closeConn func() error
}

func (b connBody) Close() error { return b.closeConn() }

// once sends one request; for a redirect it returns the Location (the body is still to be closed).
func (c *Client) once(ctx context.Context, u *url.URL) (*httpResponse, string, error) {
	if u.Scheme != "https" || u.Hostname() == "" {
		return nil, "", errors.New("only HTTPS addresses are supported")
	}
	conn, err := c.connect(ctx, u)
	if err != nil {
		return nil, "", err
	}
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	closeConn := func() error { stop(); return conn.Close() }
	fail := func(err error) (*httpResponse, string, error) {
		closeConn()
		if cerr := ctx.Err(); cerr != nil {
			err = cerr
		}
		return nil, "", err
	}
	if _, err := fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: akam\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", u.RequestURI(), u.Host); err != nil {
		return fail(err)
	}
	br := bufio.NewReaderSize(conn, 32<<10)
	code, hdr, err := readHead(br)
	if err != nil {
		return fail(err)
	}
	if loc := hdr.Get("Location"); loc != "" && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
		return &httpResponse{StatusCode: code, Body: connBody{strings.NewReader(""), closeConn}}, loc, nil
	}
	body, n, err := entity(br, code, hdr)
	if err != nil {
		return fail(err)
	}
	if strings.EqualFold(strings.TrimSpace(hdr.Get("Content-Encoding")), "gzip") {
		zr, err := gzip.NewReader(body)
		if err != nil {
			return fail(err)
		}
		body, n = zr, -1
	}
	return &httpResponse{StatusCode: code, ContentLength: n, Body: connBody{body, closeConn}}, "", nil
}

// connect opens the TLS connection to u, directly or through the proxy chosen for it.
func (c *Client) connect(ctx context.Context, u *url.URL) (net.Conn, error) {
	var proxy *url.URL
	if c.Proxy != nil {
		var err error
		if proxy, err = c.Proxy(u); err != nil {
			return nil, err
		}
	}
	port := u.Port()
	if port == "" {
		port = "443"
	}
	hostport := net.JoinHostPort(u.Hostname(), port)
	var raw net.Conn
	var err error
	if proxy == nil {
		raw, err = c.dial(ctx, hostport)
	} else {
		raw, err = c.tunnel(ctx, proxy, hostport)
	}
	if err != nil {
		return nil, err
	}
	cfg := &tls.Config{MinVersion: tls.VersionTLS12}
	if c.TLSConfig != nil {
		cfg = c.TLSConfig.Clone()
	}
	cfg.ServerName = u.Hostname()
	conn := tls.Client(raw, cfg)
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}
	if err := conn.HandshakeContext(ctx); err != nil {
		raw.Close()
		return nil, err
	}
	return conn, nil
}

func (c *Client) dial(ctx context.Context, hostport string) (net.Conn, error) {
	if c.DialContext != nil {
		return c.DialContext(ctx, "tcp", hostport)
	}
	d := net.Dialer{Timeout: 30 * time.Second, KeepAlive: 30 * time.Second}
	return d.DialContext(ctx, "tcp", hostport)
}

// tunnel opens an HTTP CONNECT tunnel to hostport through proxy.
func (c *Client) tunnel(ctx context.Context, proxy *url.URL, hostport string) (net.Conn, error) {
	pport := proxy.Port()
	if pport == "" {
		pport = map[string]string{"http": "80", "https": "443"}[proxy.Scheme]
	}
	conn, err := c.dial(ctx, net.JoinHostPort(proxy.Hostname(), pport))
	if err != nil {
		return nil, err
	}
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}
	if proxy.Scheme == "https" {
		tc := tls.Client(conn, &tls.Config{MinVersion: tls.VersionTLS12, ServerName: proxy.Hostname()})
		if err := tc.HandshakeContext(ctx); err != nil {
			conn.Close()
			return nil, err
		}
		conn = tc
	}
	req := "CONNECT " + hostport + " HTTP/1.1\r\nHost: " + hostport + "\r\nUser-Agent: akam\r\n"
	if ui := proxy.User; ui != nil {
		pw, _ := ui.Password()
		req += "Proxy-Authorization: Basic " + base64.StdEncoding.EncodeToString([]byte(ui.Username()+":"+pw)) + "\r\n"
	}
	if _, err := io.WriteString(conn, req+"\r\n"); err != nil {
		conn.Close()
		return nil, err
	}
	// The origin's TLS handshake starts only after the proxy's blank line, and the origin speaks only after
	// the client hello, so the reader cannot swallow tunnel bytes.
	code, _, err := readHead(bufio.NewReader(conn))
	if err != nil {
		conn.Close()
		return nil, err
	}
	if code != 200 {
		conn.Close()
		return nil, fmt.Errorf("the proxy refused the tunnel: HTTP %d", code)
	}
	return conn, nil
}

// readHead reads a status line and the header block (1xx answers are skipped).
func readHead(br *bufio.Reader) (int, textproto.MIMEHeader, error) {
	tp := textproto.NewReader(br)
	for {
		line, err := tp.ReadLine()
		if err != nil {
			return 0, nil, eofToUnexpected(err)
		}
		if !strings.HasPrefix(line, "HTTP/1.") || len(line) < 12 || line[8] != ' ' {
			return 0, nil, fmt.Errorf("malformed HTTP response %q", truncate(line, 40))
		}
		code, err := strconv.Atoi(line[9:12])
		if err != nil || code < 100 {
			return 0, nil, fmt.Errorf("malformed HTTP status %q", truncate(line, 40))
		}
		hdr, err := tp.ReadMIMEHeader()
		if err != nil {
			return 0, nil, eofToUnexpected(err)
		}
		if code >= 200 || code == 101 {
			return code, hdr, nil
		}
	}
}

func eofToUnexpected(err error) error {
	if err == io.EOF {
		return io.ErrUnexpectedEOF
	}
	return err
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// entity returns the message body reader and its length (-1: unknown). A body cut short is io.ErrUnexpectedEOF.
func entity(br *bufio.Reader, code int, hdr textproto.MIMEHeader) (io.Reader, int64, error) {
	if code == 204 || code == 304 || code/100 == 1 {
		return strings.NewReader(""), 0, nil
	}
	if te := hdr.Values("Transfer-Encoding"); len(te) > 0 {
		if len(te) != 1 || !strings.EqualFold(strings.TrimSpace(te[0]), "chunked") {
			return nil, 0, fmt.Errorf("unsupported Transfer-Encoding %q", truncate(strings.Join(te, ","), 40))
		}
		return &chunkedReader{br: br}, -1, nil
	}
	if cl := hdr.Values("Content-Length"); len(cl) > 0 {
		n, err := strconv.ParseInt(strings.TrimSpace(cl[0]), 10, 64)
		if err != nil || n < 0 {
			return nil, 0, fmt.Errorf("bad Content-Length %q", truncate(cl[0], 20))
		}
		for _, v := range cl[1:] {
			if strings.TrimSpace(v) != strings.TrimSpace(cl[0]) {
				return nil, 0, errors.New("conflicting Content-Length headers")
			}
		}
		return &exactReader{r: br, left: n}, n, nil
	}
	return br, -1, nil // the end of the connection ends the body
}

// exactReader yields exactly left bytes; a shorter stream is io.ErrUnexpectedEOF.
type exactReader struct {
	r    io.Reader
	left int64
}

func (e *exactReader) Read(p []byte) (int, error) {
	if e.left <= 0 {
		return 0, io.EOF
	}
	if int64(len(p)) > e.left {
		p = p[:e.left]
	}
	n, err := e.r.Read(p)
	e.left -= int64(n)
	if err == io.EOF && e.left > 0 {
		err = io.ErrUnexpectedEOF
	}
	if e.left == 0 && err == io.EOF {
		err = nil
	}
	return n, err
}

// chunkedReader decodes Transfer-Encoding: chunked (chunk extensions and trailers are ignored).
type chunkedReader struct {
	br   *bufio.Reader
	left int64
	done bool
	err  error
}

func (c *chunkedReader) fail(err error) error {
	c.err = eofToUnexpected(err)
	return c.err
}

func (c *chunkedReader) Read(p []byte) (int, error) {
	if c.err != nil {
		return 0, c.err
	}
	if c.done {
		return 0, io.EOF
	}
	if c.left == 0 {
		line, err := c.br.ReadString('\n')
		if err != nil {
			return 0, c.fail(err)
		}
		line = strings.TrimSpace(line)
		if i := strings.IndexByte(line, ';'); i >= 0 {
			line = strings.TrimSpace(line[:i])
		}
		n, perr := strconv.ParseUint(line, 16, 62)
		if perr != nil {
			return 0, c.fail(errors.New("malformed chunk size"))
		}
		if n == 0 {
			for { // trailers, up to the blank line
				t, err := c.br.ReadString('\n')
				if err != nil {
					return 0, c.fail(err)
				}
				if strings.TrimSpace(t) == "" {
					break
				}
			}
			c.done = true
			return 0, io.EOF
		}
		c.left = int64(n)
	}
	if int64(len(p)) > c.left {
		p = p[:c.left]
	}
	n, err := c.br.Read(p)
	c.left -= int64(n)
	if err != nil {
		return n, c.fail(err)
	}
	if c.left == 0 { // the CRLF after the chunk data
		var crlf [2]byte
		if _, err := io.ReadFull(c.br, crlf[:]); err != nil {
			return n, c.fail(err)
		}
		if crlf[0] != '\r' || crlf[1] != '\n' {
			return n, c.fail(errors.New("malformed chunk"))
		}
	}
	return n, nil
}

// ProxyFromEnvironment is the proxy of HTTPS_PROXY (or https_proxy) for u, unless NO_PROXY (or no_proxy) or
// the address (localhost, loopback) excludes it. A value without a scheme is an http:// proxy.
func ProxyFromEnvironment(u *url.URL) (*url.URL, error) {
	v := os.Getenv("HTTPS_PROXY")
	if v == "" {
		v = os.Getenv("https_proxy")
	}
	if v == "" {
		return nil, nil
	}
	np := os.Getenv("NO_PROXY")
	if np == "" {
		np = os.Getenv("no_proxy")
	}
	if !proxyApplies(u.Hostname(), u.Port(), np) {
		return nil, nil
	}
	p, err := url.Parse(v)
	if err != nil || p.Scheme == "" || p.Host == "" {
		p, err = url.Parse("http://" + v)
	}
	if err != nil || p.Host == "" {
		return nil, errors.New("invalid proxy address in HTTPS_PROXY")
	}
	if p.Scheme != "http" && p.Scheme != "https" {
		return nil, fmt.Errorf("unsupported proxy scheme %q (use an http:// or https:// proxy)", p.Scheme)
	}
	return p, nil
}

// proxyApplies reports whether a request to host:port goes through the proxy given the NO_PROXY list.
func proxyApplies(host, port, noProxy string) bool {
	host = strings.ToLower(strings.TrimSuffix(host, "."))
	if host == "localhost" {
		return false
	}
	ip, _ := netip.ParseAddr(host)
	if ip.IsValid() && ip.IsLoopback() {
		return false
	}
	for _, e := range strings.Split(strings.ToLower(noProxy), ",") {
		e = strings.TrimSpace(e)
		if e == "" {
			continue
		}
		if e == "*" {
			return false
		}
		if pfx, err := netip.ParsePrefix(e); err == nil {
			if ip.IsValid() && pfx.Contains(ip) {
				return false
			}
			continue
		}
		eh, ep := e, ""
		if h, p, err := net.SplitHostPort(e); err == nil {
			eh, ep = h, p
		}
		if ep != "" && ep != port {
			continue
		}
		eh = strings.TrimSuffix(strings.Trim(eh, "[]"), ".")
		if eip, err := netip.ParseAddr(eh); err == nil {
			if ip.IsValid() && eip == ip {
				return false
			}
			continue
		}
		eh = strings.TrimPrefix(eh, "*")
		if strings.HasPrefix(eh, ".") {
			if strings.HasSuffix(host, eh) || host == eh[1:] {
				return false
			}
		} else if host == eh || strings.HasSuffix(host, "."+eh) {
			return false
		}
	}
	return true
}
