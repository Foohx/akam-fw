// Package akamapi is the client of the signed local control API of an Akam camera (specs/control-api.md, section 5
// and 12.1): HMAC-SHA256 request signing and the read-only calls the installer needs (GET /hello, GET /info) to
// recognise the model and the running version before an update.
//
// The signer is tested against tests/fixtures/control_api_vectors.txt, the file the C server and the shell signer
// are tested with. Requests are signed in challenge mode (X-Akam-Challenge, one extra round trip to GET /hello): it
// needs no clock on either side, so a camera whose clock is unset, or a PC whose clock is off, works the same.
// The secret never goes in a URL, a log line or an error text.
package akamapi

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Port is the TCP port of the API.
const Port = 8092

const (
	prefix  = "/akam/v1"
	maxBody = 64 * 1024
)

var (
	// ErrClosed: nothing answers as an Akam API (no listener, 404): the camera has no API key yet, or runs stock.
	ErrClosed = errors.New("the camera does not answer the control API")
	// ErrAuth: the camera refused the key (revoked, wrong secret, scope).
	ErrAuth = errors.New("the camera refused the API key")

	keyIDRe  = regexp.MustCompile(`^[0-9a-f]{8}$`)
	secretRe = regexp.MustCompile(`^[0-9a-f]{64}$`)
)

// Key is one API key of a camera. String never shows the secret.
type Key struct {
	ID     string
	Secret []byte
}

func (k Key) String() string { return "key " + k.ID }

// ParseKey reads "key_id:secret_hex" (the form the assistant stores) or "key_id" and "secret" as two lines.
func ParseKey(text string) (Key, error) {
	t := strings.TrimSpace(text)
	t = strings.ReplaceAll(t, "\r", "")
	t = strings.ReplaceAll(t, "\n", ":")
	id, sec, ok := strings.Cut(t, ":")
	if !ok || !keyIDRe.MatchString(id) || !secretRe.MatchString(sec) {
		return Key{}, errors.New("an API key is 'KEYID:SECRET' (8 and 64 lower-case hex digits)")
	}
	b, _ := hex.DecodeString(sec)
	return Key{ID: id, Secret: b}, nil
}

// LoadKey reads a key file. On a system with permission bits the file must not be readable by others.
func LoadKey(path string) (Key, error) {
	st, err := os.Stat(path)
	if err != nil {
		return Key{}, err
	}
	if runtime.GOOS != "windows" && st.Mode().Perm()&0o077 != 0 {
		return Key{}, fmt.Errorf("%s is readable by other users: run chmod 600 on it", path)
	}
	if st.Size() > 4096 {
		return Key{}, errors.New("the key file is too large")
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return Key{}, err
	}
	return ParseKey(string(b))
}

// Sign returns the four signing headers of one request (specs/control-api.md 5.2). ts and nonce are the timestamp
// and nonce, or both "challenge:<hex>" in challenge mode (then X-Akam-Challenge replaces the two headers).
func Sign(k Key, method, path, rawQuery string, body []byte, ts, nonce string) (Header, error) {
	if err := checkPath(path); err != nil {
		return nil, err
	}
	cq, err := CanonicalQuery(rawQuery)
	if err != nil {
		return nil, err
	}
	bh := sha256.Sum256(body)
	canon := strings.Join([]string{"AKAM1-HMAC-SHA256", strings.ToUpper(method), path, cq, hex.EncodeToString(bh[:]), ts, nonce, k.ID}, "\n")
	m := hmac.New(sha256.New, k.Secret)
	m.Write([]byte(canon))
	h := Header{}
	h["X-Akam-Key"] = k.ID
	if c, ok := strings.CutPrefix(ts, "challenge:"); ok {
		h["X-Akam-Challenge"] = c
	} else {
		h["X-Akam-Timestamp"] = ts
		h["X-Akam-Nonce"] = nonce
	}
	h["X-Akam-Signature"] = hex.EncodeToString(m.Sum(nil))
	return h, nil
}

// checkPath refuses what the server answers 400 to: the path is signed as sent, never normalised.
func checkPath(p string) error {
	if len(p) < 2 || p[0] != '/' || p[len(p)-1] == '/' || strings.Contains(p, "//") || strings.Contains(p, "%") {
		return errors.New("path refused: it must start with '/', have no '//', '%' or trailing '/'")
	}
	for _, seg := range strings.Split(p[1:], "/") {
		if seg == "." || seg == ".." {
			return errors.New("path refused: '.' and '..' segments")
		}
	}
	for i := 0; i < len(p); i++ {
		if p[i] <= ' ' || p[i] == 0x7f {
			return errors.New("path refused: control character or space")
		}
	}
	return nil
}

// CanonicalQuery is the query as the server canonicalises it: each name and value percent-decoded, re-encoded with
// the unreserved set and upper-case hex, "name" alone becomes "name=", sorted by name then value, joined with '&'.
func CanonicalQuery(raw string) (string, error) {
	if raw == "" {
		return "", nil
	}
	type kv struct{ k, v string }
	var l []kv
	for _, seg := range strings.Split(raw, "&") {
		if seg == "" {
			return "", errors.New("malformed query: empty segment")
		}
		k, v, _ := strings.Cut(seg, "=")
		dk, err := pctDecode(k)
		if err != nil {
			return "", err
		}
		dv, err := pctDecode(v)
		if err != nil {
			return "", err
		}
		l = append(l, kv{pctEncode(dk), pctEncode(dv)})
	}
	sort.Slice(l, func(i, j int) bool {
		if l[i].k != l[j].k {
			return l[i].k < l[j].k
		}
		return l[i].v < l[j].v
	})
	parts := make([]string, len(l))
	for i, e := range l {
		parts[i] = e.k + "=" + e.v
	}
	return strings.Join(parts, "&"), nil
}

func pctDecode(s string) (string, error) {
	var b []byte
	for i := 0; i < len(s); i++ {
		if s[i] != '%' {
			b = append(b, s[i])
			continue
		}
		if i+2 >= len(s) {
			return "", errors.New("malformed query: bad escape")
		}
		x, err := hex.DecodeString(s[i+1 : i+3])
		if err != nil || x[0] == 0 {
			return "", errors.New("malformed query: bad escape")
		}
		b = append(b, x[0])
		i += 2
	}
	return string(b), nil
}

func pctEncode(s string) string {
	const hexu = "0123456789ABCDEF"
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '.' || c == '_' || c == '~' {
			b.WriteByte(c)
		} else {
			b.WriteByte('%')
			b.WriteByte(hexu[c>>4])
			b.WriteByte(hexu[c&15])
		}
	}
	return b.String()
}

// Header is the set of signing headers (canonical names, one value each).
type Header map[string]string

// Get returns a header value.
func (h Header) Get(k string) string { return h[k] }

// Transport does one plain HTTP/1.1 GET of path on hostport with the given headers and returns the status and the
// body (at most maxBody bytes). The default is a small raw-socket client: net/http is not linked into the program.
type Transport func(ctx context.Context, hostport, path string, h Header) (int, []byte, error)

// Client talks to one camera.
type Client struct {
	HostPort string // 192.0.2.10:8092
	Key      Key
	Do       Transport        // nil: the raw client
	DoReq    RequestTransport // PATCH and POST; nil: the raw client
	Timeout  time.Duration
}

// New is a client of the camera at host (IP or name) with the default port and a 5 s timeout.
func New(host string, k Key) *Client {
	return &Client{HostPort: net.JoinHostPort(host, strconv.Itoa(Port)), Key: k, Timeout: 5 * time.Second}
}

func rawGet(ctx context.Context, hostport, path string, h Header) (int, []byte, error) {
	return rawRequest(ctx, hostport, "GET", path, nil, h)
}

// RequestTransport does one plain HTTP/1.1 request (GET, PATCH or POST, with a JSON body for the last two) like Transport.
type RequestTransport func(ctx context.Context, hostport, method, path string, body []byte, h Header) (int, []byte, error)

func rawRequest(ctx context.Context, hostport, method, path string, reqBody []byte, h Header) (int, []byte, error) {
	// the request line and the Host header are built by concatenation: no CR, LF or blank may get into them
	if strings.ContainsAny(hostport+path, "\r\n ") || path == "" || hostport == "" || (method != "GET" && method != "PATCH" && method != "POST") {
		return 0, nil, errors.New("bad request target")
	}
	var d net.Dialer
	conn, err := d.DialContext(ctx, "tcp", hostport)
	if err != nil {
		return 0, nil, err
	}
	defer conn.Close()
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}
	names := make([]string, 0, len(h))
	for k := range h {
		names = append(names, k)
	}
	sort.Strings(names)
	var req strings.Builder
	req.WriteString(method + " " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\nAccept: application/json\r\n")
	if method == "PATCH" || method == "POST" {
		req.WriteString("Content-Type: application/json\r\nContent-Length: " + strconv.Itoa(len(reqBody)) + "\r\n")
	}
	for _, k := range names {
		if strings.ContainsAny(k+h[k], "\r\n") {
			return 0, nil, errors.New("bad header")
		}
		req.WriteString(k + ": " + h[k] + "\r\n")
	}
	req.WriteString("\r\n")
	req.Write(reqBody)
	if _, err := io.WriteString(conn, req.String()); err != nil {
		return 0, nil, err
	}
	// Connection: close and no chunked bodies (the server sends Content-Length): read to EOF, bounded.
	raw, err := io.ReadAll(io.LimitReader(conn, maxBody+8192))
	if len(raw) == 0 && err != nil {
		return 0, nil, err
	}
	head, body, ok := strings.Cut(string(raw), "\r\n\r\n")
	if !ok {
		return 0, nil, errors.New("no HTTP answer")
	}
	line, _, _ := strings.Cut(head, "\r\n")
	f := strings.Fields(line)
	if len(f) < 2 || !strings.HasPrefix(f[0], "HTTP/1.") {
		return 0, nil, errors.New("not an HTTP answer")
	}
	code, err := strconv.Atoi(f[1])
	if err != nil {
		return 0, nil, errors.New("not an HTTP answer")
	}
	if strings.Contains(strings.ToLower(head), "transfer-encoding: chunked") {
		return code, nil, errors.New("chunked answer not supported")
	}
	if len(body) > maxBody {
		body = body[:maxBody]
	}
	return code, []byte(body), nil
}

func (c *Client) get(ctx context.Context, path string, signed bool, ch string) ([]byte, int, error) {
	to := c.Timeout
	if to == 0 {
		to = 5 * time.Second
	}
	ctx, cancel := context.WithTimeout(ctx, to)
	defer cancel()
	var h Header
	if signed {
		var err error
		if h, err = Sign(c.Key, "GET", prefix+path, "", nil, "challenge:"+ch, "challenge:"+ch); err != nil {
			return nil, 0, err
		}
	}
	do := c.Do
	if do == nil {
		do = rawGet
	}
	code, b, err := do(ctx, c.HostPort, prefix+path, h)
	if err != nil {
		return nil, 0, fmt.Errorf("%w: %v", ErrClosed, redactErr(err))
	}
	return b, code, nil
}

// redactErr keeps the error text short and free of URLs.
func redactErr(err error) string {
	var ue interface{ Unwrap() error }
	if errors.As(err, &ue) && ue.Unwrap() != nil {
		err = ue.Unwrap()
	}
	return err.Error()
}

// Info is what GET /info says about the camera.
type Info struct {
	Model    string `json:"model"`
	App      string `json:"app"` // the running Akam version string
	UptimeS  int64  `json:"app_uptime_s"`
	ClockSet bool   `json:"clock_set"`
	KeyID    string `json:"key_id"`
	Scope    string `json:"scope"`
}

// Info does the signed GET /info: one GET /hello for a challenge, then the signed request.
func (c *Client) Info(ctx context.Context) (Info, error) {
	hb, code, err := c.get(ctx, "/hello", false, "")
	if err != nil {
		return Info{}, err
	}
	if code == 404 {
		return Info{}, fmt.Errorf("%w (HTTP 404)", ErrClosed)
	}
	if code != 200 {
		return Info{}, fmt.Errorf("GET /hello: HTTP %d", code)
	}
	var hello struct {
		API       int    `json:"api"`
		Challenge string `json:"challenge"`
	}
	if json.Unmarshal(hb, &hello) != nil || hello.API != 1 || !regexp.MustCompile(`^[0-9a-f]{32}$`).MatchString(hello.Challenge) {
		return Info{}, errors.New("GET /hello: not the Akam control API")
	}
	b, code, err := c.get(ctx, "/info", true, hello.Challenge)
	if err != nil {
		return Info{}, err
	}
	switch {
	case code == 401 || code == 403:
		return Info{}, ErrAuth
	case code != 200:
		return Info{}, fmt.Errorf("GET /info: HTTP %d", code)
	}
	var in Info
	if err := json.Unmarshal(b, &in); err != nil {
		return Info{}, errors.New("GET /info: unreadable answer")
	}
	return in, nil
}
