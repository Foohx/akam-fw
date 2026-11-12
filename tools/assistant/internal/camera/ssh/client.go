// Package ssh is the built-in SSH client of the assistant (replacing the ssh binary): user root,
// public-key login with the dedicated key, host-key pinning in Akam's own known_hosts file, command
// execution with optional stdin. It uses no forwarding (the camera's Dropbear has none).
package ssh

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	gossh "golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
)

// Errors a caller may want to tell apart.
var (
	ErrHostKeyRefused = errors.New("the camera's host key was not accepted")
	ErrAuth           = errors.New("SSH login refused")
)

// HostKeyChangedError is the hard error of a pinned camera that now shows another key.
type HostKeyChangedError struct {
	Alias       string
	Fingerprint string // what the camera showed now
	KnownHosts  string
}

func (e *HostKeyChangedError) Error() string {
	return fmt.Sprintf("the host key of %s CHANGED (it now shows %s)", e.Alias, e.Fingerprint)
}

// NewHostKey describes a first contact, passed to Config.Confirm.
type NewHostKey struct {
	Alias       string
	Fingerprint string // SHA256:...
	// FromUserKnownHosts is true when the same key is already trusted in ~/.ssh/known_hosts:
	// accepting then imports it into Akam's pin file.
	FromUserKnownHosts bool
	PinFile            string
}

// Config describes one connection.
type Config struct {
	Host    string
	Port    int
	KeyPath string
	// KnownHosts is Akam's own pin file (default: <state dir>/known_hosts, set by the caller).
	KnownHosts string
	// UserKnownHosts is the file whose entries may be imported (normally ~/.ssh/known_hosts).
	UserKnownHosts string
	// Alias is the name under which the key is pinned (HostKeyAlias): the IP, so one pin also
	// covers the RAM Dropbear on port 2222. Default: Host.
	Alias string
	// Confirm is asked on first contact; nil refuses (no terminal).
	Confirm func(NewHostKey) (bool, error)
	// TrustFingerprint accepts a first contact whose fingerprint equals it (--trust-new-host-key).
	TrustFingerprint string
	// Passphrase asks for the key passphrase without echo (nil: refuse).
	Passphrase func() (string, error)
	Timeout    time.Duration
	Keepalive  time.Duration
}

// Client is a connected camera.
type Client struct {
	conn    *gossh.Client
	cleanup []func()
	stop    chan struct{}
	Host    string
}

func (c *Config) alias() string {
	if c.Alias != "" {
		return c.Alias
	}
	return c.Host
}

// Dial connects and logs in. The only host-key algorithm accepted is ssh-ed25519, the one
// akam_ssh.sh generates on the camera.
func Dial(cfg Config) (*Client, error) {
	if cfg.Port == 0 {
		cfg.Port = 22
	}
	if cfg.Timeout == 0 {
		cfg.Timeout = 10 * time.Second
	}
	if cfg.Keepalive == 0 {
		cfg.Keepalive = 15 * time.Second
	}
	signer, closeSigner, err := LoadSigner(cfg.KeyPath, cfg.Passphrase)
	if err != nil {
		return nil, err
	}
	addr := net.JoinHostPort(cfg.Host, strconv.Itoa(cfg.Port))
	raw, err := net.DialTimeout("tcp", addr, cfg.Timeout)
	if err != nil {
		closeSigner()
		return nil, err
	}
	// The handshake has a deadline, but not while a person reads the host-key fingerprint: the
	// deadline is suspended while Confirm waits and restarted afterwards.
	// Only during that first handshake: once it returned, the flag stops the wrapper from touching the
	// deadline of a live connection (a later callback must never leave a deadline on it).
	var handshaking atomic.Bool
	handshaking.Store(true)
	if confirm := cfg.Confirm; confirm != nil {
		cfg.Confirm = func(nh NewHostKey) (bool, error) {
			if !handshaking.Load() {
				return confirm(nh)
			}
			_ = raw.SetDeadline(time.Time{})
			defer func() {
				if handshaking.Load() {
					_ = raw.SetDeadline(time.Now().Add(cfg.Timeout))
				}
			}()
			return confirm(nh)
		}
	}
	sc := &gossh.ClientConfig{
		User:              "root",
		Auth:              []gossh.AuthMethod{gossh.PublicKeys(signer)},
		HostKeyCallback:   cfg.hostKeyCallback(),
		HostKeyAlgorithms: []string{gossh.KeyAlgoED25519},
		Timeout:           cfg.Timeout,
		ClientVersion:     "SSH-2.0-akam",
	}
	_ = raw.SetDeadline(time.Now().Add(cfg.Timeout))
	cc, chans, reqs, err := gossh.NewClientConn(raw, addr, sc)
	handshaking.Store(false)
	if err != nil {
		raw.Close()
		closeSigner()
		return nil, classify(err)
	}
	_ = raw.SetDeadline(time.Time{})
	c := &Client{conn: gossh.NewClient(cc, chans, reqs), stop: make(chan struct{}), Host: cfg.Host}
	c.cleanup = append(c.cleanup, closeSigner)
	go c.keepalive(cfg.Keepalive)
	return c, nil
}

// classify turns the library's text errors into the typed ones.
func classify(err error) error {
	var hk *HostKeyChangedError
	if errors.As(err, &hk) || errors.Is(err, ErrHostKeyRefused) {
		return err
	}
	if s := err.Error(); containsAny(s, "unable to authenticate", "no supported methods remain") {
		return fmt.Errorf("%w: %v", ErrAuth, err)
	}
	return err
}

func containsAny(s string, subs ...string) bool {
	for _, x := range subs {
		if bytes.Contains([]byte(s), []byte(x)) {
			return true
		}
	}
	return false
}

func (c *Client) keepalive(every time.Duration) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-c.stop:
			return
		case <-t.C:
			if _, _, err := c.conn.SendRequest("keepalive@openssh.com", true, nil); err != nil {
				return
			}
		}
	}
}

// Close ends the connection.
func (c *Client) Close() {
	if c == nil || c.conn == nil {
		return
	}
	select {
	case <-c.stop:
	default:
		close(c.stop)
	}
	c.conn.Close()
	for _, f := range c.cleanup {
		f()
	}
	c.cleanup = nil
}

// Result of a command.
type Result struct {
	Stdout, Stderr []byte
	Status         int
}

// Combined returns stdout then stderr as ssh prints them in a terminal-less `2>&1`.
func (r Result) Combined() string { return string(r.Stdout) + string(r.Stderr) }

// Run executes cmd on the camera with optional stdin. A non-zero exit status is not an error:
// it is Result.Status. Context cancellation closes the session; the output received so far is
// returned with the context error.
func (c *Client) Run(ctx context.Context, cmd string, stdin io.Reader) (Result, error) {
	s, err := c.conn.NewSession()
	if err != nil {
		return Result{}, err
	}
	defer s.Close()
	// A camera (or anything answering as one) can send for ever: only the first bytes are kept.
	out, errb := &limitBuf{max: maxRunStdout}, &limitBuf{max: maxRunStderr}
	s.Stdout, s.Stderr = out, errb
	if stdin != nil {
		s.Stdin = stdin
	}
	done := make(chan error, 1)
	go func() { done <- s.Run(cmd) }()
	select {
	case <-ctx.Done():
		s.Close()
		// What arrived before the deadline is kept: a caller may use a partial answer.
		return Result{Stdout: out.bytes(), Stderr: errb.bytes()}, ctx.Err()
	case err := <-done:
		res := Result{Stdout: out.bytes(), Stderr: errb.bytes()}
		if err != nil {
			var ee *gossh.ExitError
			if errors.As(err, &ee) {
				res.Status = ee.ExitStatus()
				return res, nil
			}
			var em *gossh.ExitMissingError
			if errors.As(err, &em) {
				res.Status = 255
				return res, nil
			}
			return res, err
		}
		return res, nil
	}
}

// RunStream executes cmd and sends its standard output to out as it arrives (nothing is buffered
// here), for a command that prints a large read-only stream. When out returns an error the session
// is closed at once and that error is returned. Stderr is returned (the first 4 KiB). A non-zero
// exit status is Result.Status, not an error; a dropped connection is an error.
func (c *Client) RunStream(ctx context.Context, cmd string, out io.Writer) (Result, error) {
	s, err := c.conn.NewSession()
	if err != nil {
		return Result{}, err
	}
	defer s.Close()
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	var werr error
	var errb limitBuf
	s.Stdout = writerFunc(func(p []byte) (int, error) {
		n, err := out.Write(p)
		if err != nil {
			werr = err
			cancel()
		}
		return n, err
	})
	s.Stderr = &errb
	done := make(chan error, 1)
	go func() { done <- s.Run(cmd) }()
	select {
	case <-ctx.Done():
		s.Close()
		// The writer must not be called once we return, so wait for the session; a peer that never
		// acknowledges the close would block that wait, so after a short grace the connection goes.
		select {
		case <-done:
		case <-time.After(3 * time.Second):
			c.conn.Close()
			<-done
		}
		if werr != nil {
			return Result{}, werr
		}
		return Result{}, ctx.Err()
	case err := <-done:
		res := Result{Stderr: errb.bytes()}
		if err != nil {
			if werr != nil {
				return res, werr
			}
			var ee *gossh.ExitError
			if errors.As(err, &ee) {
				res.Status = ee.ExitStatus()
				return res, nil
			}
			var em *gossh.ExitMissingError
			if errors.As(err, &em) {
				return res, fmt.Errorf("the connection closed before the command ended")
			}
			return res, err
		}
		return res, nil
	}
}

type writerFunc func([]byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

// Limits of what Run keeps of a command's output (the helpers print a few lines).
const (
	maxRunStdout = 1 << 20
	maxRunStderr = 64 << 10
)

// limitBuf keeps the first max bytes written to it (4 KiB when max is 0) and drops the rest; the
// writer always reports success, so the command is not stopped.
type limitBuf struct {
	mu  sync.Mutex
	b   []byte
	max int
}

// bytes returns a copy of what was kept so far (safe while the session still writes).
func (l *limitBuf) bytes() []byte {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]byte(nil), l.b...)
}

func (l *limitBuf) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	limit := l.max
	if limit == 0 {
		limit = 4096
	}
	if room := limit - len(l.b); room > 0 {
		if len(p) < room {
			room = len(p)
		}
		l.b = append(l.b, p[:room]...)
	}
	return len(p), nil
}

// ---- host key pinning ----

func (c *Config) hostKeyCallback() gossh.HostKeyCallback {
	return func(hostname string, remote net.Addr, key gossh.PublicKey) error {
		alias := c.alias()
		fp := gossh.FingerprintSHA256(key)
		if key.Type() != gossh.KeyAlgoED25519 {
			return fmt.Errorf("the camera offered a %s host key; Akam cameras use ed25519 only", key.Type())
		}
		target := net.JoinHostPort(alias, "22") // normalised to the bare alias
		if fileExists(c.KnownHosts) {
			cb, err := knownhosts.New(c.KnownHosts)
			if err != nil {
				return fmt.Errorf("cannot read the pin file %s: %w", c.KnownHosts, err)
			}
			err = cb(target, remote, key)
			if err == nil {
				return nil
			}
			var ke *knownhosts.KeyError
			if !errors.As(err, &ke) {
				return err
			}
			if len(ke.Want) > 0 {
				return &HostKeyChangedError{Alias: alias, Fingerprint: fp, KnownHosts: c.KnownHosts}
			}
		}
		// First contact.
		nh := NewHostKey{Alias: alias, Fingerprint: fp, PinFile: c.KnownHosts}
		if c.UserKnownHosts != "" && fileExists(c.UserKnownHosts) {
			if ucb, err := knownhosts.New(c.UserKnownHosts); err == nil {
				uerr := ucb(target, remote, key)
				var uke *knownhosts.KeyError
				switch {
				case uerr == nil:
					nh.FromUserKnownHosts = true
				case errors.As(uerr, &uke) && len(uke.Want) > 0:
					// The wizard's ssh pinned another key for this camera: same hard error.
					return &HostKeyChangedError{Alias: alias, Fingerprint: fp, KnownHosts: c.UserKnownHosts}
				}
			}
		}
		ok := false
		switch {
		case c.TrustFingerprint != "" && strings.TrimSpace(c.TrustFingerprint) == fp:
			ok = true
		case c.TrustFingerprint != "":
			return fmt.Errorf("%w: --trust-new-host-key %s does not match the fingerprint shown by the camera (%s)", ErrHostKeyRefused, c.TrustFingerprint, fp)
		case c.Confirm != nil:
			var err error
			if ok, err = c.Confirm(nh); err != nil {
				return fmt.Errorf("%w: %v", ErrHostKeyRefused, err)
			}
		}
		if !ok {
			return ErrHostKeyRefused
		}
		return appendPin(c.KnownHosts, alias, key)
	}
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	_, err := os.Stat(p)
	return err == nil
}

func appendPin(file, alias string, key gossh.PublicKey) error {
	if err := fsutil.PrivateMkdir(filepath.Dir(file)); err != nil {
		return err
	}
	f, err := os.OpenFile(file, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = fmt.Fprintln(f, knownhosts.Line([]string{alias}, key))
	return err
}

// ForgetHostKey removes every pin of alias from file and reports how many lines were dropped.
func ForgetHostKey(file, alias string) (int, error) {
	b, err := os.ReadFile(file)
	if err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}
	var keep []string
	n := 0
	for _, l := range bytes.Split(bytes.TrimRight(b, "\n"), []byte("\n")) {
		fields := bytes.Fields(l)
		if len(fields) > 0 && !bytes.HasPrefix(fields[0], []byte("#")) && hostsContain(string(fields[0]), alias) {
			n++
			continue
		}
		keep = append(keep, string(l))
	}
	if n == 0 {
		return 0, nil
	}
	out := ""
	for _, l := range keep {
		out += l + "\n"
	}
	return n, fsutil.AtomicWrite(file, []byte(out), 0o600)
}

func hostsContain(list, alias string) bool {
	for _, h := range bytes.Split([]byte(list), []byte(",")) {
		if string(h) == alias {
			return true
		}
	}
	return false
}
