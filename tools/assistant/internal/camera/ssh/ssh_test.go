package ssh_test

import (
	"bytes"
	"context"
	"errors"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	gossh "golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

type fixture struct {
	cam      *testfake.Camera
	key      string
	known    string
	userKnow string
}

func newFixture(t *testing.T) *fixture { return newFixtureWith(t, testfake.Options{}) }

func newFixtureWith(t *testing.T, o testfake.Options) *fixture {
	t.Helper()
	dir := t.TempDir()
	key := filepath.Join(dir, "akam")
	if err := camssh.GenerateKey(key, "akam", nil); err != nil {
		t.Fatal(err)
	}
	pubBytes, _ := os.ReadFile(key + ".pub")
	pub, _, _, _, err := gossh.ParseAuthorizedKey(pubBytes)
	if err != nil {
		t.Fatal(err)
	}
	o.AuthorizedKey = pub
	cam := testfake.New(t, o)
	return &fixture{cam: cam, key: key, known: filepath.Join(dir, "state", "known_hosts"), userKnow: filepath.Join(dir, "user_known_hosts")}
}

func (f *fixture) cfg(confirm func(camssh.NewHostKey) (bool, error)) camssh.Config {
	return camssh.Config{Host: "127.0.0.1", Alias: "192.0.2.50", Port: f.cam.SSHPort(), KeyPath: f.key,
		KnownHosts: f.known, UserKnownHosts: f.userKnow, Confirm: confirm}
}

func yes(camssh.NewHostKey) (bool, error) { return true, nil }

func TestKeyGenerationIsOpenSSHCompatible(t *testing.T) {
	dir := t.TempDir()
	key := filepath.Join(dir, "k")
	if err := camssh.GenerateKey(key, "akam", nil); err != nil {
		t.Fatal(err)
	}
	st, _ := os.Stat(key)
	if st.Mode().Perm() != 0o600 {
		t.Errorf("private key mode %v", st.Mode().Perm())
	}
	pub, _ := os.ReadFile(key + ".pub")
	if !bytes.HasPrefix(pub, []byte("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5")) || !bytes.HasSuffix(pub, []byte(" akam\n")) || bytes.Count(pub, []byte("\n")) != 1 {
		t.Errorf("public key line: %q", pub)
	}
	if err := camssh.GenerateKey(key, "akam", nil); err == nil {
		t.Error("an existing key was overwritten")
	}
	if p, err := exec.LookPath("ssh-keygen"); err == nil { // local tool, no network: key usable by ssh -i
		out, err := exec.Command(p, "-y", "-f", key).Output()
		if err != nil {
			t.Fatalf("ssh-keygen cannot read the key: %v", err)
		}
		if !bytes.HasPrefix(pub, bytes.TrimSpace(out)) {
			t.Errorf("ssh-keygen derives another public key:\n%s\n%s", out, pub)
		}
	}
	// a key with a passphrase needs it
	pk := filepath.Join(dir, "p")
	if err := camssh.GenerateKey(pk, "akam", []byte("pass phrase")); err != nil {
		t.Fatal(err)
	}
	t.Setenv("SSH_AUTH_SOCK", "")
	if _, _, err := camssh.LoadSigner(pk, nil); err == nil {
		t.Error("passphrase key loaded without a passphrase")
	}
	if _, _, err := camssh.LoadSigner(pk, func() (string, error) { return "wrong", nil }); err == nil {
		t.Error("wrong passphrase accepted")
	}
	if _, _, err := camssh.LoadSigner(pk, func() (string, error) { return "pass phrase", nil }); err != nil {
		t.Errorf("right passphrase refused: %v", err)
	}
}

func TestKeyPathFor(t *testing.T) {
	if got := camssh.KeyPathFor("/id", "/a.pub", "/b.pub"); got != "/id" {
		t.Error(got)
	}
	if got := camssh.KeyPathFor("", "/a.pub", "/b.pub"); got != "/a" {
		t.Error(got)
	}
	if got := camssh.KeyPathFor("", "", "/b.pub"); got != "/b" {
		t.Error(got)
	}
	if got := camssh.KeyPathFor("", "", ""); !strings.HasSuffix(got, "/.ssh/akam") {
		t.Error(got)
	}
}

func TestFirstContactPinsAndSecondContactIsSilent(t *testing.T) {
	f := newFixture(t)
	asked := 0
	var seen camssh.NewHostKey
	c, err := camssh.Dial(f.cfg(func(nh camssh.NewHostKey) (bool, error) { asked++; seen = nh; return true, nil }))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	if asked != 1 || seen.Fingerprint != f.cam.HostFingerprint() || seen.Alias != "192.0.2.50" {
		t.Fatalf("asked=%d seen=%+v", asked, seen)
	}
	b, _ := os.ReadFile(f.known)
	if !strings.HasPrefix(string(b), "192.0.2.50 ssh-ed25519 ") {
		t.Errorf("pin file: %q", b)
	}
	if st, _ := os.Stat(f.known); st.Mode().Perm() != 0o600 {
		t.Errorf("pin file mode %v", st.Mode().Perm())
	}
	c, err = camssh.Dial(f.cfg(func(camssh.NewHostKey) (bool, error) { t.Error("asked again"); return false, nil }))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
}

func TestChangedHostKeyIsAHardError(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	f.cam.RotateHostKey()
	_, err = camssh.Dial(f.cfg(func(camssh.NewHostKey) (bool, error) {
		t.Error("a changed key must never be offered for acceptance")
		return true, nil
	}))
	var hk *camssh.HostKeyChangedError
	if !errors.As(err, &hk) {
		t.Fatalf("err = %v", err)
	}
	if hk.Fingerprint != f.cam.HostFingerprint() {
		t.Error("the error does not carry the new fingerprint")
	}
	// even with --trust-new-host-key for the new key
	cfg := f.cfg(nil)
	cfg.TrustFingerprint = f.cam.HostFingerprint()
	if _, err = camssh.Dial(cfg); !errors.As(err, &hk) {
		t.Fatalf("trust flag bypassed the pin: %v", err)
	}
	// forgetting is explicit, then the new key is a first contact again
	n, err := camssh.ForgetHostKey(f.known, "192.0.2.50")
	if err != nil || n != 1 {
		t.Fatalf("forget: %d %v", n, err)
	}
	c, err = camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
}

func TestRefusedFirstContactWritesNothing(t *testing.T) {
	f := newFixture(t)
	for name, confirm := range map[string]func(camssh.NewHostKey) (bool, error){
		"no":       func(camssh.NewHostKey) (bool, error) { return false, nil },
		"no tty":   func(camssh.NewHostKey) (bool, error) { return false, errors.New("no terminal") },
		"callback": nil,
	} {
		_, err := camssh.Dial(f.cfg(confirm))
		if !errors.Is(err, camssh.ErrHostKeyRefused) {
			t.Errorf("%s: err = %v", name, err)
		}
		if _, err := os.Stat(f.known); err == nil {
			t.Errorf("%s: a pin was written", name)
		}
	}
}

func TestTrustNewHostKeyFlag(t *testing.T) {
	f := newFixture(t)
	cfg := f.cfg(nil)
	cfg.TrustFingerprint = "SHA256:wrong"
	if _, err := camssh.Dial(cfg); !errors.Is(err, camssh.ErrHostKeyRefused) {
		t.Fatalf("wrong fingerprint: %v", err)
	}
	cfg.TrustFingerprint = f.cam.HostFingerprint()
	c, err := camssh.Dial(cfg)
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
}

func TestImportFromUserKnownHosts(t *testing.T) {
	f := newFixture(t)
	// the user's own file already trusts this key (as the bash wizard's ssh would have recorded it)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	b, _ := os.ReadFile(f.known)
	if err := os.WriteFile(f.userKnow, b, 0o600); err != nil {
		t.Fatal(err)
	}
	os.Remove(f.known)
	var seen camssh.NewHostKey
	c, err = camssh.Dial(f.cfg(func(nh camssh.NewHostKey) (bool, error) { seen = nh; return true, nil }))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	if !seen.FromUserKnownHosts {
		t.Error("the entry in the user's known_hosts was not noticed")
	}
	// and the user's file is never written
	after, _ := os.ReadFile(f.userKnow)
	if !bytes.Equal(after, b) {
		t.Error("the user's known_hosts was modified")
	}
}

func TestWrongKeyIsRefused(t *testing.T) {
	f := newFixture(t)
	other := filepath.Join(t.TempDir(), "other")
	if err := camssh.GenerateKey(other, "x", nil); err != nil {
		t.Fatal(err)
	}
	cfg := f.cfg(yes)
	cfg.KeyPath = other
	if _, err := camssh.Dial(cfg); !errors.Is(err, camssh.ErrAuth) {
		t.Fatalf("err = %v", err)
	}
	cfg.KeyPath = filepath.Join(t.TempDir(), "missing")
	if _, err := camssh.Dial(cfg); err == nil {
		t.Fatal("a missing key file connected")
	}
}

func TestExecStdinControlPortAndUnknownCommand(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	ctx := context.Background()
	r, err := c.Run(ctx, "true", nil)
	if err != nil || r.Status != 0 {
		t.Fatalf("true: %+v %v", r, err)
	}
	r, err = c.Run(ctx, "frobnicate --now", nil)
	if err != nil || r.Status != 127 || !strings.Contains(r.Combined(), "not found") {
		t.Fatalf("unknown command: %+v %v", r, err)
	}
	if testfake.HelpersAvailable() {
		r, err = c.Run(ctx, "/usr/sbin/akam_rtsp_auth.sh set-creds", strings.NewReader("viewer\nsupersecret1\n"))
		if err != nil || r.Status != 0 {
			t.Fatalf("set-creds: %+v %v", r, err)
		}
		r, _ = c.Run(ctx, "/usr/sbin/akam_rtsp_auth.sh show", nil)
		if strings.TrimSpace(string(r.Stdout)) != "user: viewer" {
			t.Errorf("show: %q err=%q status=%d", r.Stdout, r.Stderr, r.Status)
		}
	}
	r, err = c.Run(ctx, ctl.NcCommand, strings.NewReader(ctl.Input([]string{"GET led.status", "GET night.ir_led"})))
	if err != nil || r.Status != 0 {
		t.Fatalf("nc: %+v %v", r, err)
	}
	out := ctl.Answers(string(r.Stdout))
	if ctl.Get("led.status", out) != "auto" || ctl.Get("night.ir_led", out) != "auto" || len(out) != 2 {
		t.Errorf("control answers: %v (raw %q)", out, r.Stdout)
	}
}

// The camera's Dropbear is built without TCP forwarding: the fake must refuse direct-tcpip as it does,
// so that nothing in the assistant can come to rely on a forward again.
func TestFakeRefusesForwardingLikeDropbear(t *testing.T) {
	f := newFixture(t)
	pem, err := os.ReadFile(f.key)
	if err != nil {
		t.Fatal(err)
	}
	signer, err := gossh.ParsePrivateKey(pem)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := gossh.Dial("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(f.cam.SSHPort())), &gossh.ClientConfig{
		User: "root", Auth: []gossh.AuthMethod{gossh.PublicKeys(signer)}, HostKeyCallback: gossh.InsecureIgnoreHostKey(),
	})
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	if conn, err := raw.Dial("tcp", "127.0.0.1:8091"); err == nil {
		conn.Close()
		t.Fatal("the fake accepted a direct-tcpip channel: the real camera refuses it")
	} else if !strings.Contains(err.Error(), "unknown channel type") {
		t.Errorf("err = %v", err)
	}
}

// A person may take longer than the connection timeout to read the fingerprint of a new camera.
func TestSlowHostKeyConfirmationDoesNotExpireTheHandshake(t *testing.T) {
	f := newFixture(t)
	cfg := f.cfg(func(camssh.NewHostKey) (bool, error) {
		time.Sleep(900 * time.Millisecond)
		return true, nil
	})
	cfg.Timeout = 300 * time.Millisecond
	c, err := camssh.Dial(cfg)
	if err != nil {
		t.Fatalf("a slow confirmation broke the handshake: %v", err)
	}
	defer c.Close()
	if r, err := c.Run(context.Background(), "true", nil); err != nil || r.Status != 0 {
		t.Fatalf("true: %+v %v", r, err)
	}
}

func TestKnownHostsLineFormatIsOpenSSH(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	cb, err := knownhosts.New(f.known) // the standard parser accepts what we wrote
	if err != nil {
		t.Fatal(err)
	}
	_ = cb
}

// ProbeHostKey reads the key of the handshake without logging in; the pin helpers agree with it.
func TestProbeHostKeyAndPins(t *testing.T) {
	f := newFixture(t)
	key, err := camssh.ProbeHostKey(context.Background(), "127.0.0.1", f.cam.SSHPort(), nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	if camssh.Fingerprint(key) != f.cam.HostFingerprint() {
		t.Errorf("fingerprint %s, camera %s", camssh.Fingerprint(key), f.cam.HostFingerprint())
	}
	if camssh.PinnedFingerprint(f.known, "192.0.2.50") != "" {
		t.Error("a pin before any was written")
	}
	if err := camssh.PinHostKey(f.known, "192.0.2.50", key); err != nil {
		t.Fatal(err)
	}
	if got := camssh.PinnedFingerprint(f.known, "192.0.2.50"); got != f.cam.HostFingerprint() {
		t.Errorf("pinned %q", got)
	}
	if camssh.PinnedFingerprint(f.known, "192.0.2.51") != "" {
		t.Error("another alias matched")
	}
	// the pin written this way is the one Dial checks
	c, err := camssh.Dial(f.cfg(func(camssh.NewHostKey) (bool, error) { t.Error("asked"); return false, nil }))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	// a closed port is an error, not a hang
	if _, err := camssh.ProbeHostKey(context.Background(), "127.0.0.1", 1, nil, time.Second); err == nil {
		t.Error("probe of a closed port succeeded")
	}
}
