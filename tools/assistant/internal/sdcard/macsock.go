package sdcard

import (
	"bufio"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"
)

// The opt-in macOS elevation (AKAM_SD_ELEVATE=osascript, spec 11, 4.4.4). AppleScript cannot pass stdin, so the
// request travels over a Unix socket:
//
//	parent (user)                                   helper (root, started by osascript)
//	 private dir 0700, random name, listen
//	 osascript "<self> sd-helper --socket P --uid U --nonce N"  --->  checks the socket's directory (0700, owner U)
//	                                                  <--- connect, peer must be uid U (LOCAL_PEERCRED)
//	 peer must be uid 0 (LOCAL_PEERCRED)              <--- nonce line
//	 constant-time nonce check, then the request ---> exactly one request (HelperMain), then EOF
//	 progress and the final answer as JSON lines <---
//
// The command line carries the fixed executable path, the socket path, the uid and a random nonce: no user data,
// so there is no quoting hazard and the Wi-Fi password stays out of every command line and environment.

const (
	sockName         = "s.sock"
	sockMaxPath      = 100 // sun_path is 104 bytes on macOS
	sockHandshakeTTL = 15 * time.Second
	sockAcceptTTL    = 5 * time.Minute // the user types an administrator password
	sockMaxBadPeers  = 8
	sockMaxAnswer    = 8 << 20
)

var (
	nonceRe    = regexp.MustCompile(`^[0-9a-f]{32}$`)
	safeSelfRe = regexp.MustCompile(`^/[A-Za-z0-9 ._+/@,=-]+$`)
)

// peerUIDFunc returns the user id of the process at the other end of a Unix socket (LOCAL_PEERCRED on macOS).
type peerUIDFunc func(c *net.UnixConn) (uint32, error)

// osascriptTransport is the parent side. Everything that touches the OS is a field, so a test can play the helper.
type osascriptTransport struct {
	uid     uint32
	tmpBase string
	peer    peerUIDFunc
	// launch starts the osascript command and returns a channel that gets its exit status.
	launch func(argv []string) (<-chan error, error)
	accept time.Duration
}

func realOsascript() *osascriptTransport {
	return &osascriptTransport{uid: uint32(os.Getuid()), tmpBase: os.TempDir(), peer: peerUIDOS, launch: launchOsascript, accept: sockAcceptTTL}
}

func launchOsascript(argv []string) (<-chan error, error) {
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Env = []string{"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LANG=C"}
	var stderr strings.Builder
	cmd.Stderr = &limitedWriter{w: &stderr, n: 2048}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	ch := make(chan error, 1)
	go func() {
		err := cmd.Wait()
		if err != nil {
			err = fmt.Errorf("%v: %s", err, strings.TrimSpace(stderr.String()))
		}
		ch <- err
	}()
	return ch, nil
}

// osascriptArgv builds the command. self is checked for characters that could leave the quoting; nothing else in
// the script is variable but the socket path (a private directory under the temp dir: hex and fixed words), the
// uid and the nonce (hex).
func osascriptArgv(self, sock string, uid uint32, nonce string) ([]string, error) {
	if !safeSelfRe.MatchString(self) {
		return nil, fmt.Errorf("the program path %q cannot be passed to the administrator prompt", self)
	}
	if !safeSelfRe.MatchString(sock) || !nonceRe.MatchString(nonce) {
		return nil, errors.New("internal error: unsafe socket path or nonce")
	}
	cmdline := fmt.Sprintf("'%s' sd-helper --socket '%s' --uid %d --nonce %s", self, sock, uid, nonce)
	script := `do shell script "` + cmdline + `" with administrator privileges`
	return []string{"/usr/bin/osascript", "-e", script}, nil
}

func randHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// privateSockDir creates a 0700 directory with a random name and returns it.
func privateSockDir(base string) (string, error) {
	r, err := randHex(8)
	if err != nil {
		return "", err
	}
	for _, b := range []string{base, "/tmp"} {
		if len(filepath.Join(b, "akam-"+r, sockName)) > sockMaxPath {
			continue
		}
		dir := filepath.Join(b, "akam-"+r)
		if err := os.Mkdir(dir, 0o700); err != nil {
			return "", err
		}
		if err := os.Chmod(dir, 0o700); err != nil {
			os.Remove(dir)
			return "", err
		}
		return dir, nil
	}
	return "", errors.New("no short enough private directory for the socket")
}

// sockLine is one line of the helper's answer: progress text, or the final Response.
type sockLine struct {
	Progress *string `json:"progress"`
	Response
}

// run is RunElevated for osascript.
func (t *osascriptTransport) run(self string, req Request, stderr io.Writer) (Response, error) {
	var err error
	if self == "" {
		if self, err = os.Executable(); err != nil {
			return Response{}, err
		}
	}
	dir, err := privateSockDir(t.tmpBase)
	if err != nil {
		return Response{}, err
	}
	defer os.RemoveAll(dir)
	sock := filepath.Join(dir, sockName)
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: sock, Net: "unix"})
	if err != nil {
		return Response{}, err
	}
	defer ln.Close()
	_ = os.Chmod(sock, 0o600)
	nonce, err := randHex(16)
	if err != nil {
		return Response{}, err
	}
	argv, err := osascriptArgv(self, sock, t.uid, nonce)
	if err != nil {
		return Response{}, err
	}
	body, err := json.Marshal(req)
	if err != nil {
		return Response{}, err
	}
	exited, err := t.launch(argv)
	if err != nil {
		return Response{}, fmt.Errorf("the administrator prompt did not start: %v", err)
	}
	type accepted struct {
		c   *net.UnixConn
		err error
	}
	got := make(chan accepted, 1)
	go func() {
		c, err := t.acceptRoot(ln, nonce)
		got <- accepted{c, err}
	}()
	var conn *net.UnixConn
	select {
	case a := <-got:
		if a.err != nil {
			return Response{}, a.err
		}
		conn = a.c
	case err := <-exited:
		// the prompt ended without the helper connecting: declined, wrong password or a failure
		ln.Close()
		if a := <-got; a.c != nil {
			a.c.Close()
		}
		if err == nil {
			err = errors.New("the administrator step ended without connecting")
		}
		return Response{}, fmt.Errorf("the administrator step did not run (declined?): %v", err)
	}
	defer conn.Close()
	resp, err := t.converse(conn, body, stderr)
	select { // let osascript finish; its status no longer matters once the answer is in
	case <-exited:
	case <-time.After(5 * time.Second):
	}
	return resp, err
}

// acceptRoot accepts connections until one comes from uid 0 with the right nonce; others are closed.
func (t *osascriptTransport) acceptRoot(ln *net.UnixListener, nonce string) (*net.UnixConn, error) {
	_ = ln.SetDeadline(time.Now().Add(t.accept))
	bad := 0
	for {
		c, err := ln.AcceptUnix()
		if err != nil {
			return nil, errors.New("the administrator helper did not connect")
		}
		if err := t.authenticate(c, nonce); err != nil {
			c.Close()
			if bad++; bad >= sockMaxBadPeers {
				return nil, fmt.Errorf("refusing: too many bad connections (last: %v)", err)
			}
			continue
		}
		return c, nil
	}
}

func (t *osascriptTransport) authenticate(c *net.UnixConn, nonce string) error {
	uid, err := t.peer(c)
	if err != nil {
		return fmt.Errorf("cannot read the peer credentials: %v", err)
	}
	if uid != 0 {
		return fmt.Errorf("the peer is uid %d, not root", uid)
	}
	_ = c.SetReadDeadline(time.Now().Add(sockHandshakeTTL))
	line, err := bufio.NewReaderSize(io.LimitReader(c, 64), 64).ReadString('\n')
	if err != nil {
		return errors.New("no nonce")
	}
	if subtle.ConstantTimeCompare([]byte(strings.TrimSuffix(line, "\n")), []byte(nonce)) != 1 {
		return errors.New("wrong nonce")
	}
	_ = c.SetReadDeadline(time.Time{})
	return nil
}

func (t *osascriptTransport) converse(c *net.UnixConn, body []byte, stderr io.Writer) (Response, error) {
	if _, err := c.Write(body); err != nil {
		return Response{}, fmt.Errorf("sending the request failed: %v", err)
	}
	if err := c.CloseWrite(); err != nil {
		return Response{}, err
	}
	dec := json.NewDecoder(io.LimitReader(c, sockMaxAnswer))
	for {
		var l sockLine
		if err := dec.Decode(&l); err != nil {
			return Response{}, errors.New("the helper gave no usable answer")
		}
		if l.Progress != nil {
			if stderr != nil {
				fmt.Fprintln(stderr, sanitizeLine(*l.Progress))
			}
			continue
		}
		return l.Response, nil
	}
}

func sanitizeLine(s string) string {
	return strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, s)
}

// ttyAvailable reports whether sudo could ask for a password on a terminal.
func ttyAvailable() bool {
	f, err := os.OpenFile("/dev/tty", os.O_RDWR, 0)
	if err != nil {
		return false
	}
	f.Close()
	return true
}

// rawNodeFor is the node whose writability decides NeedsElevation: /dev/rdiskN on macOS, the path itself elsewhere.
func rawNodeFor(dev string) string {
	if runtime.GOOS == "darwin" && macDiskRe.MatchString(dev) {
		return rawPath(dev)
	}
	return dev
}

// ---- helper side ----

// progressWriter turns the lines a helper writes into {"progress": "..."} lines on the socket.
type progressWriter struct{ w io.Writer }

func (p progressWriter) Write(b []byte) (int, error) {
	for _, ln := range strings.Split(strings.TrimRight(string(b), "\n"), "\n") {
		j, _ := json.Marshal(map[string]string{"progress": ln})
		if _, err := p.w.Write(append(j, '\n')); err != nil {
			return 0, err
		}
	}
	return len(b), nil
}

// socketHelper is `sd-helper --socket P --uid N --nonce H` (root, started by osascript). It returns the exit status.
func socketHelper(args []string, mk func() Helper, peer peerUIDFunc, owner func(fs.FileInfo) (uint32, bool)) int {
	if len(args) != 6 || args[0] != "--socket" || args[2] != "--uid" || args[4] != "--nonce" {
		return 2
	}
	sock, nonce := args[1], args[5]
	uid, err := strconv.ParseUint(args[3], 10, 32)
	if err != nil || !nonceRe.MatchString(nonce) || !filepath.IsAbs(sock) || filepath.Base(sock) != sockName {
		return 2
	}
	if err := checkSockDir(filepath.Dir(sock), uint32(uid), owner); err != nil {
		fmt.Fprintln(os.Stderr, "sd-helper:", err)
		return 2
	}
	raddr := &net.UnixAddr{Name: sock, Net: "unix"}
	c, err := net.DialUnix("unix", nil, raddr)
	if err != nil {
		return 2
	}
	defer c.Close()
	if pu, err := peer(c); err != nil || pu != uint32(uid) {
		fmt.Fprintln(os.Stderr, "sd-helper: the socket is not owned by the user who started the program")
		return 2
	}
	if _, err := io.WriteString(c, nonce+"\n"); err != nil {
		return 2
	}
	h := mk()
	h.Progress = progressWriter{c}
	return HelperMain(c, c, h)
}

// checkSockDir requires a real directory, mode 0700, owned by uid (the user who started the program).
func checkSockDir(dir string, uid uint32, owner func(fs.FileInfo) (uint32, bool)) error {
	st, err := os.Lstat(dir)
	if err != nil || !st.IsDir() {
		return errors.New("the socket directory is not a directory")
	}
	if st.Mode().Perm() != 0o700 {
		return errors.New("the socket directory is not private (0700)")
	}
	if o, ok := owner(st); !ok || o != uid {
		return errors.New("the socket directory is not owned by the calling user")
	}
	return nil
}
