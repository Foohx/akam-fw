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
	"math"
	"net"
	"net/netip"
	"net/textproto"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

// Client is the minimal HTTPS GET client of the downloads. It replaces net/http, which links about 0.55 MiB more
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

const (
	maxRedirects = 10
	maxHead      = 1 << 20 // status line, headers and 1xx answers together (also a proxy's CONNECT answer)
	maxLine      = 4 << 10 // a chunk-size or trailer line
	maxTrailers  = 16 << 10
)

// policyError is a refusal that a retry cannot change (non-HTTPS redirect, redirect loop, unsupported or
// malformed proxy setting, a certificate that does not verify): FetchLimit fails at once.
type policyError struct{ error }

func (e *policyError) Unwrap() error { return e.error }

func policyf(format string, a ...any) error { return &policyError{fmt.Errorf(format, a...)} }

// capReader limits the bytes read from the connection while the head of a message is parsed.
type capReader struct {
	r io.Reader
	n int64
}

var errHeadTooLarge = errors.New("the answer head is too large")

func (c *capReader) Read(p []byte) (int, error) {
	if c.n <= 0 {
		return 0, errHeadTooLarge
	}
	if int64(len(p)) > c.n {
		p = p[:c.n]
	}
	n, err := c.r.Read(p)
	c.n -= int64(n)
	return n, err
}

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
		return policyf("refusing a redirect to a non-HTTPS address")
	}
	if requests >= maxRedirects {
		return policyf("too many redirects")
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
		return nil, "", policyf("only HTTPS addresses are supported")
	}
	conn, err := c.connect(ctx, u)
	if err != nil {
		return nil, "", err
	}
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	closeConn := func() error { stop(); return conn.Close() }
	fail := func(err error) (*httpResponse, string, error) {
		closeConn()
		return nil, "", ctxOr(ctx, err)
	}
	if _, err := fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: akam\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", u.RequestURI(), u.Host); err != nil {
		return fail(err)
	}
	cr := &capReader{r: conn, n: maxHead}
	br := bufio.NewReaderSize(cr, 32<<10)
	code, hdr, err := readHead(br)
	if err != nil {
		return fail(err)
	}
	cr.n = math.MaxInt64 // the head is read: the body is bounded by its own framing and by the caller's limit
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
		return nil, ctxOr(ctx, err)
	}
	conn := tls.Client(raw, c.tlsConfig(u.Hostname()))
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}
	if err := conn.HandshakeContext(ctx); err != nil {
		raw.Close()
		var cve *tls.CertificateVerificationError
		if errors.As(err, &cve) {
			err = &policyError{err} // a retry meets the same certificate
		}
		return nil, ctxOr(ctx, err)
	}
	return conn, nil
}

// tlsConfig is a clone of the client's configuration for serverName (TLS 1.2 or newer by default).
func (c *Client) tlsConfig(serverName string) *tls.Config {
	cfg := &tls.Config{MinVersion: tls.VersionTLS12}
	if c.TLSConfig != nil {
		cfg = c.TLSConfig.Clone()
	}
	cfg.ServerName = serverName
	return cfg
}

// ctxOr is the context's error when it is done (a closed connection is then only a consequence).
func ctxOr(ctx context.Context, err error) error {
	if cerr := ctx.Err(); cerr != nil {
		return cerr
	}
	return err
}

func (c *Client) dial(ctx context.Context, hostport string) (net.Conn, error) {
	if c.DialContext != nil {
		return c.DialContext(ctx, "tcp", hostport)
	}
	d := net.Dialer{Timeout: 30 * time.Second, KeepAlive: 30 * time.Second}
	return d.DialContext(ctx, "tcp", hostport)
}

// tunnel opens an HTTP CONNECT tunnel to hostport through proxy. Its errors start with "proxy:" and never
// carry the proxy's credentials.
func (c *Client) tunnel(ctx context.Context, proxy *url.URL, hostport string) (net.Conn, error) {
	pport := proxy.Port()
	if pport == "" {
		pport = map[string]string{"http": "80", "https": "443"}[proxy.Scheme]
	}
	conn, err := c.dial(ctx, net.JoinHostPort(proxy.Hostname(), pport))
	if err != nil {
		return nil, fmt.Errorf("proxy: %w", err)
	}
	// Cancellation must reach a proxy that accepts and never answers.
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	done := false
	defer func() {
		stop()
		if !done {
			conn.Close()
		}
	}()
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}
	if proxy.Scheme == "https" {
		tc := tls.Client(conn, c.tlsConfig(proxy.Hostname()))
		if err := tc.HandshakeContext(ctx); err != nil {
			return nil, fmt.Errorf("proxy: %w", ctxOr(ctx, err))
		}
		conn = tc
	}
	req := "CONNECT " + hostport + " HTTP/1.1\r\nHost: " + hostport + "\r\nUser-Agent: akam\r\n"
	if ui := proxy.User; ui != nil {
		pw, _ := ui.Password()
		req += "Proxy-Authorization: Basic " + base64.StdEncoding.EncodeToString([]byte(ui.Username()+":"+pw)) + "\r\n"
	}
	if _, err := io.WriteString(conn, req+"\r\n"); err != nil {
		return nil, fmt.Errorf("proxy: %w", ctxOr(ctx, err))
	}
	// The origin's TLS handshake starts only after the proxy's blank line, and the origin speaks only after
	// the client hello, so the reader cannot swallow tunnel bytes.
	code, _, err := readHead(bufio.NewReader(&capReader{r: conn, n: maxHead}))
	if err != nil {
		return nil, fmt.Errorf("proxy: %w", ctxOr(ctx, err))
	}
	if code != 200 {
		return nil, fmt.Errorf("proxy: the tunnel was refused (HTTP %d)", code)
	}
	done = true
	return conn, nil
}

// readHead reads a status line and the header block (1xx answers are skipped); the caller bounds the bytes.
// Only HTTP/1.0 and HTTP/1.1 with a 3-digit status are accepted. Lines are those of net/textproto, which
// tolerates a bare LF: headers carry no framing risk here (one request per connection, Connection: close);
// the chunk framing, where a sloppy parser could desynchronise, requires CRLF (see readLine).
func readHead(br *bufio.Reader) (int, textproto.MIMEHeader, error) {
	tp := textproto.NewReader(br)
	for {
		line, err := tp.ReadLine()
		if err != nil {
			return 0, nil, eofToUnexpected(err)
		}
		code, ok := parseStatus(line)
		if !ok {
			return 0, nil, fmt.Errorf("malformed HTTP status line %q", truncate(line, 40))
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

func parseStatus(line string) (int, bool) {
	if len(line) < 12 || (!strings.HasPrefix(line, "HTTP/1.1 ") && !strings.HasPrefix(line, "HTTP/1.0 ")) {
		return 0, false
	}
	if !allDigits(line[9:12]) || (len(line) > 12 && line[12] != ' ') {
		return 0, false
	}
	code, _ := strconv.Atoi(line[9:12])
	return code, code >= 100
}

func allDigits(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
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
	te, cl := hdr.Values("Transfer-Encoding"), hdr.Values("Content-Length")
	if len(te) > 0 && len(cl) > 0 {
		return nil, 0, errors.New("the answer has both Transfer-Encoding and Content-Length")
	}
	if len(te) > 0 {
		if len(te) != 1 || !strings.EqualFold(strings.TrimSpace(te[0]), "chunked") {
			return nil, 0, fmt.Errorf("unsupported Transfer-Encoding %q", truncate(strings.Join(te, ","), 40))
		}
		return &chunkedReader{br: br}, -1, nil
	}
	if len(cl) > 0 {
		v := strings.TrimSpace(cl[0])
		n, err := strconv.ParseInt(v, 10, 64)
		if !allDigits(v) || len(v) > 18 || err != nil {
			return nil, 0, fmt.Errorf("bad Content-Length %q", truncate(cl[0], 20))
		}
		for _, o := range cl[1:] {
			if strings.TrimSpace(o) != v {
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

// readLine reads one CRLF-terminated line of at most maxLine bytes (the line and its CRLF are bounded, so a
// server cannot make the client buffer an endless line). A bare LF is an error: chunk framing is strict.
func readLine(br *bufio.Reader) (string, error) {
	b, err := br.ReadSlice('\n')
	if err == bufio.ErrBufferFull {
		return "", errors.New("chunk framing line too long")
	}
	if err != nil {
		return "", eofToUnexpected(err)
	}
	if len(b) > maxLine {
		return "", errors.New("chunk framing line too long")
	}
	if len(b) < 2 || b[len(b)-2] != '\r' {
		return "", errors.New("malformed chunk framing (no CRLF)")
	}
	return string(b[:len(b)-2]), nil
}

// chunkedReader decodes Transfer-Encoding: chunked (chunk extensions and trailers are ignored, but bounded).
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
		line, err := readLine(c.br)
		if err != nil {
			return 0, c.fail(err)
		}
		size, _, _ := strings.Cut(line, ";")
		size = strings.TrimRight(size, " \t")
		n, perr := strconv.ParseUint(size, 16, 62)
		if perr != nil {
			return 0, c.fail(errors.New("malformed chunk size"))
		}
		if n == 0 {
			total := 0
			for { // trailers, up to the blank line
				t, err := readLine(c.br)
				if err != nil {
					return 0, c.fail(err)
				}
				if t == "" {
					break
				}
				if total += len(t) + 2; total > maxTrailers {
					return 0, c.fail(errors.New("chunked trailers too large"))
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
// the address (localhost, loopback) excludes it. A value without a scheme is an http:// proxy. Its errors
// never quote the variable (it may hold credentials).
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
	port := u.Port()
	if port == "" {
		port = "443"
	}
	if !proxyApplies(u.Hostname(), port, np) {
		return nil, nil
	}
	if !strings.Contains(v, "://") {
		v = "http://" + v
	}
	p, err := url.Parse(v)
	if err != nil {
		return nil, policyf("proxy: HTTPS_PROXY is not a valid address")
	}
	if p.Hostname() == "" {
		return nil, policyf("proxy: HTTPS_PROXY has no host name")
	}
	if p.Scheme != "http" && p.Scheme != "https" {
		return nil, policyf("proxy: unsupported scheme %q in HTTPS_PROXY (SOCKS proxies are not supported; use an HTTP CONNECT proxy)", p.Scheme)
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
