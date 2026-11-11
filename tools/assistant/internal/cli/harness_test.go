package cli_test

import (
	"crypto/sha256"
	"encoding/hex"
	"bytes"
	"context"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/audio"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// env is one test world: a fake camera, a private HOME and state directory, a fresh SSH key.
type env struct {
	t     *testing.T
	cam   *testfake.Camera
	state string
	home  string
	key   string
	// capture is the fake microphone/converter; nil means "no tool installed" (nothing is run).
	capture *fakeCapture
	pass    string // AKAM_RTSP_PASSWORD for the next run (the program removes it from its environment)
}

type fakeCapture struct {
	mu        sync.Mutex
	micPCM    []byte // raw s16le the "microphone" delivers
	convPCM   []byte
	closed    int
	micCalls  int
	convCalls int
}

type closer struct {
	io.Reader
	f *fakeCapture
}

func (c closer) Close() error { c.f.mu.Lock(); c.f.closed++; c.f.mu.Unlock(); return nil }

func (f *fakeCapture) stats() (mic, closed, conv int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.micCalls, f.closed, f.convCalls
}

func (f *fakeCapture) Mic(_ context.Context, _ int) (io.ReadCloser, error) {
	f.mu.Lock()
	f.micCalls++
	f.mu.Unlock()
	return closer{bytes.NewReader(f.micPCM), f}, nil
}

func (f *fakeCapture) Convert(_ context.Context, _ string) (io.ReadCloser, error) {
	f.mu.Lock()
	f.convCalls++
	f.mu.Unlock()
	return closer{bytes.NewReader(f.convPCM), f}, nil
}

var _ audio.Capturer = (*fakeCapture)(nil)

func freeClosedPort(t *testing.T) string {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	p := ln.Addr().(*net.TCPAddr).Port
	ln.Close()
	return itoa(p)
}

func itoa(n int) string {
	var b [12]byte
	i := len(b)
	for {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
		if n == 0 {
			break
		}
	}
	return string(b[i:])
}

func newEnv(t *testing.T, opts ...func(*testfake.Options)) *env {
	t.Helper()
	root := t.TempDir()
	e := &env{t: t, state: filepath.Join(root, "state"), home: filepath.Join(root, "home")}
	if err := os.MkdirAll(e.home, 0o700); err != nil {
		t.Fatal(err)
	}
	e.key = filepath.Join(e.home, ".ssh", "akam")
	if err := camssh.GenerateKey(e.key, "akam", nil); err != nil {
		t.Fatal(err)
	}
	pubBytes, _ := os.ReadFile(e.key + ".pub")
	pub, _, _, _, err := gossh.ParseAuthorizedKey(pubBytes)
	if err != nil {
		t.Fatal(err)
	}
	fo := testfake.Options{AuthorizedKey: pub}
	for _, o := range opts {
		o(&fo)
	}
	e.cam = testfake.New(t, fo)
	t.Setenv("HOME", e.home)
	t.Setenv("XDG_STATE_HOME", "")
	t.Setenv("AKAM_STATE_DIR", "")
	t.Setenv("SSH_AUTH_SOCK", "")
	t.Setenv("NO_COLOR", "1")
	t.Setenv("AKAM_FORCE_TTY", "1")
	t.Setenv("AKAM_PORT_SSH", itoa(e.cam.SSHPort()))
	t.Setenv("AKAM_PORT_RTSP", itoa(e.cam.RTSPPort()))
	t.Setenv("AKAM_PORT_CTL", itoa(e.cam.CtlPort()))
	t.Setenv("AKAM_PORT_TELNET", freeClosedPort(t))
	t.Setenv("AKAM_RTSP_PASSWORD", "")
	t.Setenv("AKAM_TALK_CAPTURE_CMD", "")
	t.Setenv("AKAM_TALK_UNPACED", "1")
	t.Cleanup(func() {
		// Safety rule on every command the camera ever received: no reboot, power-off, flash write.
		bad := regexp.MustCompile(`(?i)\b(reboot|poweroff|halt|shutdown|init\s+[0-6])\b|watchdog|flash_tool|of=/dev/`)
		for _, c := range e.cam.Commands() {
			if bad.MatchString(c) {
				t.Errorf("the camera received a forbidden command: %s", c)
			}
		}
	})
	return e
}

// base are the options every run gets: no colour, this state, this key, this camera, its pinned key.
func (e *env) base() []string {
	return []string{"--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "--identity", e.key,
		"--trust-new-host-key", e.cam.HostFingerprint()}
}

type result struct {
	out, err string
	rc       int
}

func (e *env) hooks() cli.Hooks {
	h := cli.Hooks{
		// nothing from the host: no ffmpeg, no arecord, no pactl
		LookPath: func(string) (string, error) { return "", exec.ErrNotFound },
	}
	if e.capture != nil {
		h.Capture = e.capture
	}
	return h
}

// run runs the program with the base options followed by args; stdin is the scripted input.
func (e *env) run(stdin string, args ...string) result {
	return e.runRaw(stdin, append(e.base(), args...)...)
}

func (e *env) runRaw(stdin string, argv ...string) result {
	if e.pass != "" {
		os.Setenv("AKAM_RTSP_PASSWORD", e.pass)
	}
	var out, errb bytes.Buffer
	rc := cli.Run(argv, cli.Streams{In: strings.NewReader(stdin), Out: &out, Err: &errb}, "test", e.hooks())
	return result{out.String(), errb.String(), rc}
}

func (e *env) readState(key string) string {
	b, _ := os.ReadFile(filepath.Join(e.state, "state"))
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, key+"="); ok {
			return v
		}
	}
	return ""
}

func (e *env) writeState(content string) {
	if err := os.MkdirAll(e.state, 0o700); err != nil {
		e.t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(e.state, "state"), []byte(content), 0o600); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) logText() string {
	b, _ := os.ReadFile(filepath.Join(e.state, "akam.log"))
	return string(b)
}

func (e *env) needHelpers() {
	if !testfake.HelpersAvailable() {
		e.t.Skip("skip: the camera helper scripts cannot run here (no POSIX sh)")
	}
}

func contains(t *testing.T, s string, subs ...string) {
	t.Helper()
	for _, sub := range subs {
		if !strings.Contains(s, sub) {
			t.Errorf("output lacks %q:\n%s", sub, s)
		}
	}
}

func notContains(t *testing.T, s string, subs ...string) {
	t.Helper()
	for _, sub := range subs {
		if sub != "" && strings.Contains(s, sub) {
			t.Errorf("output must not contain %q:\n%s", sub, s)
		}
	}
}

func expectRC(t *testing.T, r result, rc int) {
	t.Helper()
	if r.rc != rc {
		t.Fatalf("exit status %d, want %d\nstdout:\n%s\nstderr:\n%s", r.rc, rc, r.out, r.err)
	}
}

func hashOf(p string) (string, error) {
	b, err := os.ReadFile(p)
	if err != nil {
		return "", err
	}
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:]), nil
}

// listen opens a loopback TCP port that accepts and drops connections, until the test ends.
func listen(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			c.Close()
		}
	}()
	t.Cleanup(func() { ln.Close(); <-done })
	return ln.Addr().(*net.TCPAddr).Port
}
