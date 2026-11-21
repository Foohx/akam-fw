package sdcard

import (
	"bytes"
	"errors"
	"io"
	"io/fs"
	"net"
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

var argvRe = regexp.MustCompile(`'([^']*)' sd-helper --socket '([^']*)' --uid (\d+) --nonce ([0-9a-f]+)`)

func fakeOwner(uid uint32) func(fs.FileInfo) (uint32, bool) {
	return func(fs.FileInfo) (uint32, bool) { return uid, true }
}

// sockHarness plays osascript: launch parses the script it is given and runs the helper side in a goroutine.
type sockHarness struct {
	t        *testing.T
	uid      uint32 // the user who runs the program
	helperIn func(args []string) []string
	hPeer    peerUIDFunc
	owner    func(fs.FileInfo) (uint32, bool)
	rec      *recorder
	argv     []string
	exit     int
	dirMode  os.FileMode
}

func (h *sockHarness) transport(parentPeer peerUIDFunc) *osascriptTransport {
	if h.hPeer == nil {
		h.hPeer = func(*net.UnixConn) (uint32, error) { return h.uid, nil }
	}
	if h.owner == nil {
		h.owner = fakeOwner(h.uid)
	}
	return &osascriptTransport{uid: h.uid, tmpBase: h.t.TempDir(), peer: parentPeer, accept: 2 * time.Second, launch: func(argv []string) (<-chan error, error) {
		h.argv = argv
		m := argvRe.FindStringSubmatch(argv[2])
		if m == nil {
			h.t.Fatalf("script not understood: %s", argv[2])
		}
		args := []string{"--socket", m[2], "--uid", m[3], "--nonce", m[4]}
		if h.helperIn != nil {
			args = h.helperIn(args)
		}
		// the socket lives in a private directory with a random name
		dir := m[2][:strings.LastIndex(m[2], "/")]
		if st, err := os.Stat(dir); err != nil || st.Mode().Perm() != 0o700 || !strings.Contains(dir, "akam-") {
			h.t.Errorf("socket dir %s: %v %v", dir, st, err)
		}
		if h.dirMode != 0 {
			_ = os.Chmod(dir, h.dirMode)
		}
		ch := make(chan error, 1)
		go func() {
			mk := func() Helper { return Helper{Inv: h.rec, Opener: h.rec} }
			code := socketHelper(args, mk, h.hPeer, h.owner)
			h.exit = code
			if code != 0 {
				ch <- errors.New("exit " + strconv.Itoa(code))
				return
			}
			ch <- nil
		}()
		return ch, nil
	}}
}

func linuxCardFacts() Facts {
	return Facts{OS: "linux", Path: "/dev/sdz", Name: "sdz", Whole: true, Size: 128 << 20, Bus: "usb", RemovableRaw: "1", Model: "Card", DevNum: "8:48"}
}

func TestSocketTransportOneRequestOneAnswer(t *testing.T) {
	facts := linuxCardFacts()
	h := &sockHarness{t: t, uid: 501, rec: &recorder{facts: facts}}
	tr := h.transport(func(*net.UnixConn) (uint32, error) { return 0, nil }) // fake peer: root
	var progress bytes.Buffer
	resp, err := tr.run("/Applications/Akam Tools/akam", Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts)}, &progress)
	if err != nil || !resp.OK {
		t.Fatalf("%v %+v", err, resp)
	}
	if !strings.Contains(progress.String(), "wipe: 0%") {
		t.Errorf("progress not forwarded: %q", progress.String())
	}
	if got := strings.Join(h.rec.log, ","); !strings.Contains(got, "open ") || !strings.Contains(got, "write") {
		t.Errorf("helper did not run: %s", got)
	}
	// the command line: fixed words, the program path, a hex socket path, the uid, a hex nonce; no request data
	if h.argv[0] != "/usr/bin/osascript" || h.argv[1] != "-e" || len(h.argv) != 3 {
		t.Fatalf("argv %q", h.argv)
	}
	m := argvRe.FindStringSubmatch(h.argv[2])
	if m == nil || m[1] != "/Applications/Akam Tools/akam" || m[3] != "501" || len(m[4]) != 32 {
		t.Fatalf("script %q", h.argv[2])
	}
	if !strings.HasSuffix(h.argv[2], `" with administrator privileges`) || strings.Contains(h.argv[2], "/dev/sdz") || strings.Contains(h.argv[2], "wipe") {
		t.Errorf("script carries request data: %s", h.argv[2])
	}
	if !regexp.MustCompile(`/akam-[0-9a-f]{16}/s\.sock`).MatchString(m[2]) {
		t.Errorf("socket path %q", m[2])
	}
	// the directory is gone afterwards
	if _, err := os.Stat(m[2][:strings.LastIndex(m[2], "/")]); err == nil {
		t.Error("socket directory left behind")
	}
}

func TestSocketTransportRefusesANonRootPeer(t *testing.T) {
	facts := linuxCardFacts()
	h := &sockHarness{t: t, uid: 501, rec: &recorder{facts: facts}}
	tr := h.transport(func(*net.UnixConn) (uint32, error) { return 501, nil }) // the user's own process, not root
	_, err := tr.run("/usr/local/bin/akam", Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts)}, io.Discard)
	if err == nil {
		t.Fatal("accepted a peer that is not root")
	}
	for _, l := range h.rec.log {
		if l == "write" || strings.HasPrefix(l, "open") {
			t.Fatalf("the helper ran: %v", h.rec.log)
		}
	}
}

func TestSocketTransportPeerCredentialError(t *testing.T) {
	facts := linuxCardFacts()
	h := &sockHarness{t: t, uid: 501, rec: &recorder{facts: facts}}
	tr := h.transport(func(*net.UnixConn) (uint32, error) { return 0, errors.New("no cred") })
	if _, err := tr.run("/usr/local/bin/akam", Request{Op: OpList}, io.Discard); err == nil {
		t.Fatal("accepted a peer whose credentials cannot be read")
	}
}

func TestSocketTransportRefusesAWrongNonce(t *testing.T) {
	facts := linuxCardFacts()
	h := &sockHarness{t: t, uid: 501, rec: &recorder{facts: facts}}
	h.helperIn = func(a []string) []string { a[5] = strings.Repeat("0", 32); return a }
	tr := h.transport(func(*net.UnixConn) (uint32, error) { return 0, nil })
	if _, err := tr.run("/usr/local/bin/akam", Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts)}, io.Discard); err == nil {
		t.Fatal("accepted a wrong nonce")
	}
	if len(h.rec.log) != 0 {
		t.Fatalf("the helper ran: %v", h.rec.log)
	}
}

func TestSocketHelperChecksItsServer(t *testing.T) {
	facts := linuxCardFacts()
	for name, mod := range map[string]func(h *sockHarness){
		"server is another uid": func(h *sockHarness) { h.hPeer = func(*net.UnixConn) (uint32, error) { return 777, nil } },
		"peer unreadable":       func(h *sockHarness) { h.hPeer = func(*net.UnixConn) (uint32, error) { return 0, errors.New("x") } },
		"dir not owned by user": func(h *sockHarness) { h.owner = fakeOwner(999) },
		"dir not private":       func(h *sockHarness) { h.dirMode = 0o755 },
		"bad socket name":       func(h *sockHarness) { h.helperIn = func(a []string) []string { a[1] += "x"; return a } },
		"relative socket":       func(h *sockHarness) { h.helperIn = func(a []string) []string { a[1] = "s.sock"; return a } },
		"extra argument":        func(h *sockHarness) { h.helperIn = func(a []string) []string { return append(a, "--x") } },
		"short nonce":           func(h *sockHarness) { h.helperIn = func(a []string) []string { a[5] = "abc"; return a } },
	} {
		h := &sockHarness{t: t, uid: 501, rec: &recorder{facts: facts}}
		mod(h)
		tr := h.transport(func(*net.UnixConn) (uint32, error) { return 0, nil })
		tr.accept = 500 * time.Millisecond
		if _, err := tr.run("/usr/local/bin/akam", Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts)}, io.Discard); err == nil {
			t.Errorf("%s: accepted", name)
		}
		if len(h.rec.log) != 0 {
			t.Errorf("%s: the helper ran: %v", name, h.rec.log)
		}
	}
}

func TestSocketTransportDeclinedPrompt(t *testing.T) {
	tr := &osascriptTransport{uid: 501, tmpBase: t.TempDir(), peer: func(*net.UnixConn) (uint32, error) { return 0, nil }, accept: 5 * time.Second,
		launch: func([]string) (<-chan error, error) {
			ch := make(chan error, 1)
			ch <- errors.New("execution error: User canceled. (-128)")
			return ch, nil
		}}
	_, err := tr.run("/usr/local/bin/akam", Request{Op: OpList}, io.Discard)
	if err == nil || !strings.Contains(err.Error(), "did not run") {
		t.Fatalf("%v", err)
	}
}

func TestOsascriptArgvRefusesUnsafePaths(t *testing.T) {
	nonce := strings.Repeat("a", 32)
	for _, self := range []string{"", "akam", "/tmp/a'b", `/tmp/a"b`, "/tmp/a\nb", "/tmp/a$(id)", "/tmp/a;b", "/tmp/a\\b", "/tmp/a`b`"} {
		if _, err := osascriptArgv(self, "/tmp/akam-0123456789abcdef/s.sock", 501, nonce); err == nil {
			t.Errorf("accepted self %q", self)
		}
	}
	if _, err := osascriptArgv("/usr/local/bin/akam", "/tmp/x y'/s.sock", 501, nonce); err == nil {
		t.Error("accepted a socket path with a quote")
	}
	if _, err := osascriptArgv("/usr/local/bin/akam", "/tmp/akam-0123456789abcdef/s.sock", 501, "xyz"); err == nil {
		t.Error("accepted a non-hex nonce")
	}
}

func TestPrivateSockDirIsPrivateAndRandom(t *testing.T) {
	base := t.TempDir()
	a, err := privateSockDir(base)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := privateSockDir(base)
	if a == b {
		t.Error("same name twice")
	}
	if st, err := os.Stat(a); err != nil || st.Mode().Perm() != 0o700 {
		t.Errorf("%v %v", st, err)
	}
	if len(a+"/"+sockName) > sockMaxPath+len(base) {
		t.Error("path too long")
	}
}
