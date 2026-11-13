//go:build akamtest

package cli_test

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// One key per camera: the choice at the first install, the key row, and the change of the key of a
// camera that runs Akam. The camera is the fake one, whose Dropbear reads the key list the way the
// real one does (the tmpfs copy that akam_authkeys.sh must refresh).

// keyArgs are the options of a run that uses the key of the STATE (no --identity: that would override it).
func keyArgs(e *env, extra ...string) []string {
	return append([]string{"--no-color", "--state-dir", e.state, "--ip", "127.0.0.1",
		"--trust-new-host-key", e.cam.HostFingerprint()}, extra...)
}

func (e *env) keyRun(stdin string, extra ...string) result {
	return e.runRaw(stdin, keyArgs(e, extra...)...)
}

// canLogin says whether the camera accepts the private key at priv (passphrase pass, "" for none).
func canLogin(t *testing.T, e *env, priv, pass string) bool {
	t.Helper()
	c, err := camssh.Dial(camssh.Config{
		Host: "127.0.0.1", Port: e.cam.SSHPort(), KeyPath: priv, Alias: "127.0.0.1",
		KnownHosts: filepath.Join(t.TempDir(), "kh"), TrustFingerprint: e.cam.HostFingerprint(),
		Passphrase: func() (string, error) { return pass, nil }, Timeout: 5 * time.Second,
	})
	if err != nil {
		if errors.Is(err, camssh.ErrAuth) {
			return false
		}
		t.Fatalf("dial: %v", err)
	}
	c.Close()
	return true
}

func sharedPub(e *env) string { return e.key + ".pub" }
func ownPriv(e *env, camera string) string {
	return filepath.Join(e.home, ".ssh", "akam-"+camera)
}

func (e *env) useSharedKey() { e.writeState("ssh_pubkey=" + sharedPub(e) + "\n") }

func keyLinesOf(t *testing.T, list string) []string {
	t.Helper()
	var out []string
	for _, l := range strings.Split(list, "\n") {
		if f := strings.Fields(l); len(f) >= 2 {
			out = append(out, f[0]+" "+f[1])
		}
	}
	return out
}

func pubLine(t *testing.T, pubFile string) string {
	t.Helper()
	b, err := os.ReadFile(pubFile)
	if err != nil {
		t.Fatal(err)
	}
	f := strings.Fields(string(b))
	return f[0] + " " + f[1]
}

// ---------------------------------------------------------------- show

func TestKeyShowSharedOwnAndCustom(t *testing.T) {
	e := newEnv(t)
	e.useSharedKey()
	r := e.keyRun("", "key", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "SSH key of camera default", "  Key: shared (~/.ssh/akam)",
		"  Fingerprint: "+camssh.PubFingerprint(sharedPub(e)), "  Private key: ~/.ssh/akam (stays on this computer)")
	notContains(t, r.out, "BEGIN")
	// no subcommand means show
	r = e.keyRun("", "key")
	expectRC(t, r, 0)
	contains(t, r.out, "  Key: shared (~/.ssh/akam)")

	own := ownPriv(e, "default")
	if err := camssh.GenerateKey(own, "akam-default", nil); err != nil {
		t.Fatal(err)
	}
	e.writeState("ssh_pubkey=" + own + ".pub\n")
	r = e.keyRun("", "key", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "  Key: this camera only (~/.ssh/akam-default)", "  Fingerprint: "+camssh.PubFingerprint(own+".pub"))

	other := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(other, "work", nil); err != nil {
		t.Fatal(err)
	}
	e.writeState("ssh_pubkey=" + other + ".pub\n")
	r = e.keyRun("", "key", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "  Key: custom (~/.ssh/id_work)")
}

func TestKeyUsageAndDryRun(t *testing.T) {
	e := newEnv(t)
	for _, args := range [][]string{{"key", "frob"}, {"key", "use"}, {"key", "use-new", "x"}, {"key", "show", "x"}} {
		r := e.keyRun("", args...)
		expectRC(t, r, 2)
		contains(t, r.err, "akam: key needs one of: show | use-shared | use-new | use FILE.pub")
	}
	r := e.keyRun("", "--dry-run", "key", "use-new")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD connect with the current key", "WOULD then log in again with the NEW key", "Other cameras are never touched")
	if n := len(e.cam.Commands()); n != 0 {
		t.Errorf("a rehearsal sent %d commands to the camera", n)
	}
	if _, err := os.Stat(ownPriv(e, "default")); err == nil {
		t.Error("a rehearsal created a key")
	}
}

func TestStatusShowsTheKeyOnceChosen(t *testing.T) {
	e := newEnv(t)
	r := e.keyRun("", "status")
	expectRC(t, r, 0)
	notContains(t, r.out, "SSH key:")
	e.useSharedKey()
	r = e.keyRun("", "status")
	expectRC(t, r, 0)
	contains(t, r.out, "  SSH key: shared (~/.ssh/akam)  "+camssh.PubFingerprint(sharedPub(e)))
}

// ---------------------------------------------------------------- the change

func TestKeyUseNewAddsVerifiesAndKeepsBothKeys(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	// other cameras are never touched
	otherDir := filepath.Join(e.state, "cameras", "garage")
	if err := os.MkdirAll(otherDir, 0o700); err != nil {
		t.Fatal(err)
	}
	otherState := "camera_ip=192.0.2.9\nssh_pubkey=" + sharedPub(e) + "\n"
	if err := os.WriteFile(filepath.Join(otherDir, "state"), []byte(otherState), 0o600); err != nil {
		t.Fatal(err)
	}
	oldLine := pubLine(t, sharedPub(e))

	// create? y   passphrase twice   add? y   remove the old key? (default No)
	r := e.keyRun("y\npw-new-1\npw-new-1\ny\n\n", "key", "use-new")
	expectRC(t, r, 0)
	contains(t, r.out, "Created ", "Change the SSH key of camera default",
		"New key:     this camera only (~/.ssh/akam-default)", "Current key: shared (~/.ssh/akam)",
		"Key added (added). Logging in again with the new key ...", "The new key works and is now used for camera default",
		"No keeps both keys working.", "The old key was left on the camera.")
	contains(t, r.err, "Add the new key to this camera now? [y/N]", "Remove the old key from this camera? [y/N]")

	own := ownPriv(e, "default")
	// format and mode of the new key
	b, err := os.ReadFile(own)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(b), "-----BEGIN OPENSSH "+"PRIVATE KEY-----") {
		t.Errorf("not an OpenSSH private key: %.40s", b)
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(own); st.Mode().Perm() != 0o600 {
			t.Errorf("private key mode %v", st.Mode().Perm())
		}
	}
	if _, err := gossh.ParsePrivateKey(b); err == nil {
		t.Error("the key was written without its passphrase")
	}
	s, closeFn, err := camssh.LoadSigner(own, func() (string, error) { return "pw-new-1", nil })
	if err != nil {
		t.Fatalf("LoadSigner with the passphrase: %v", err)
	}
	closeFn()
	if gossh.FingerprintSHA256(s.PublicKey()) != camssh.PubFingerprint(own+".pub") {
		t.Error("the .pub file does not match the private key")
	}
	if c := pubComment(t, own+".pub"); c != "akam-default" {
		t.Errorf("comment %q, want akam-default", c)
	}

	// the camera: both keys listed, file mode 600, both log in
	list, mode := e.cam.AuthorizedKeys()
	got := keyLinesOf(t, list)
	if len(got) != 2 || got[0] != oldLine || got[1] != pubLine(t, own+".pub") {
		t.Errorf("camera key list: %q", got)
	}
	if runtime.GOOS != "windows" && mode != 0o600 {
		t.Errorf("authorized_keys mode %v", mode)
	}
	if !canLogin(t, e, e.key, "") || !canLogin(t, e, own, "pw-new-1") {
		t.Error("both keys must log in")
	}
	// remembered for this camera only
	if v := e.readState("ssh_pubkey"); v != own+".pub" {
		t.Errorf("ssh_pubkey = %q", v)
	}
	if b, _ := os.ReadFile(filepath.Join(otherDir, "state")); string(b) != otherState {
		t.Errorf("another camera's state changed: %q", b)
	}
	// the log never holds the passphrase or the private key
	log := e.logText()
	notContains(t, log, "pw-new-1", "BEGIN OPENSSH", strings.Fields(pubLine(t, own+".pub"))[1])
	contains(t, log, "'sh /tmp/akam_authkeys.sh add' < key line (")
	// next runs use the new key, and show says so
	r = e.keyRun("", "key", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "  Key: this camera only (~/.ssh/akam-default)")
	// the same choice again changes nothing
	n := authkeysCommands(e)
	r = e.keyRun("pw-new-1\n", "key", "use-new")
	expectRC(t, r, 0)
	contains(t, r.out, "already uses this key")
	if authkeysCommands(e) != n {
		t.Error("an unchanged key sent commands to the camera")
	}
}

func TestKeyChangeToAnotherKeyAndRemoveOld(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	// add? y   remove the old key? y
	r := e.keyRun("y\ny\n", "key", "use", work+".pub")
	expectRC(t, r, 0)
	contains(t, r.out, "New key:     custom (~/.ssh/id_work)", "The new key works and is now used for camera default",
		"Yes removes only that key from THIS camera", "was removed from this camera; the new key still works.")
	list, _ := e.cam.AuthorizedKeys()
	if got := keyLinesOf(t, list); len(got) != 1 || got[0] != pubLine(t, work+".pub") {
		t.Errorf("camera key list: %q", got)
	}
	if canLogin(t, e, e.key, "") {
		t.Error("the removed key still logs in")
	}
	if !canLogin(t, e, work, "") {
		t.Error("the new key must log in")
	}
	if v := e.readState("ssh_pubkey"); v != work+".pub" {
		t.Errorf("ssh_pubkey = %q", v)
	}
	// back to the shared key: the camera now lists only id_work, so the current key logs in
	r = e.keyRun("y\nn\n", "key", "use-shared")
	expectRC(t, r, 0)
	list, _ = e.cam.AuthorizedKeys()
	if got := keyLinesOf(t, list); len(got) != 2 {
		t.Errorf("camera key list: %q", got)
	}
	if !canLogin(t, e, e.key, "") || !canLogin(t, e, work, "") {
		t.Error("both keys must log in")
	}
}

func TestKeyChangeRollsBackWhenTheNewKeyDoesNotLogIn(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.FreezeAuth = true })
	e.needHelpers()
	e.useSharedKey()
	before := e.readState("ssh_pubkey")
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	listBefore, _ := e.cam.AuthorizedKeys()
	r := e.keyRun("y\n", "key", "use", work+".pub")
	expectRC(t, r, 1)
	contains(t, r.out, "Key added (added).", "The new key was taken out of the camera again.")
	contains(t, r.err, "the new key does not work")
	contains(t, r.err+r.out, "the key was not changed.", "the camera still accepts your current key; nothing was saved here.")
	if list, _ := e.cam.AuthorizedKeys(); list != listBefore {
		t.Errorf("rollback left the list changed:\n%s", list)
	}
	if !canLogin(t, e, e.key, "") {
		t.Error("the old key must still log in after the failure")
	}
	if v := e.readState("ssh_pubkey"); v != before {
		t.Errorf("state changed to %q", v)
	}
	notContains(t, r.out, "The new key works")
}

func TestKeyChangeRefusesAKeyWithoutItsPrivateFile(t *testing.T) {
	e := newEnv(t)
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_lost")
	if err := camssh.GenerateKey(work, "lost", nil); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(work); err != nil {
		t.Fatal(err)
	}
	r := e.keyRun("y\ny\n", "key", "use", work+".pub")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not on this computer")
	if list, _ := e.cam.AuthorizedKeys(); len(keyLinesOf(t, list)) != 1 {
		t.Errorf("the camera list changed: %q", list)
	}
}

func TestKeyChangeRefusesAPrivateKeyAndAnRSAKey(t *testing.T) {
	e := newEnv(t)
	e.useSharedKey()
	r := e.keyRun("y\n", "key", "use", e.key) // the PRIVATE file
	expectRC(t, r, 1)
	contains(t, r.err, "is a PRIVATE key")
	r = e.keyRun("y\n", "key", "use", filepath.Join(e.home, "nope.pub"))
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "public key not found")
}

func TestKeyChangeIsIdempotentWhenTheKeyIsAlreadyOnTheCamera(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	// put the key on the camera by hand, as a first run that died after the add would have
	c, err := camssh.Dial(camssh.Config{Host: "127.0.0.1", Port: e.cam.SSHPort(), KeyPath: e.key, Alias: "127.0.0.1",
		KnownHosts: filepath.Join(t.TempDir(), "kh"), TrustFingerprint: e.cam.HostFingerprint()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	script, _ := assets.Script("akam_authkeys.sh")
	line, _ := os.ReadFile(work + ".pub")
	runKeys(t, c, script, "add", string(line), "added")
	runKeys(t, c, script, "add", string(line), "present")
	r := e.keyRun("y\n\n", "key", "use", work+".pub")
	expectRC(t, r, 0)
	contains(t, r.out, "Key added (present).", "The new key works")
	list, _ := e.cam.AuthorizedKeys()
	if got := keyLinesOf(t, list); len(got) != 2 {
		t.Errorf("a key was listed twice: %q", got)
	}
}

// runKeys runs akam_authkeys.sh on the camera and checks its AUTHKEYS= word.
func runKeys(t *testing.T, c *camssh.Client, script []byte, mode, line, want string) {
	t.Helper()
	ctx := context.Background()
	if _, err := c.Run(ctx, "cat > /tmp/akam_authkeys.sh", strings.NewReader(string(script))); err != nil {
		t.Fatal(err)
	}
	res, err := c.Run(ctx, "sh /tmp/akam_authkeys.sh "+mode, strings.NewReader(line))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(res.Stdout), "AUTHKEYS="+want) {
		t.Fatalf("%s: stdout %q stderr %q status %d, want AUTHKEYS=%s", mode, res.Stdout, res.Stderr, res.Status, want)
	}
}

func TestCameraScriptNeverEmptiesTheKeyList(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	c, err := camssh.Dial(camssh.Config{Host: "127.0.0.1", Port: e.cam.SSHPort(), KeyPath: e.key, Alias: "127.0.0.1",
		KnownHosts: filepath.Join(t.TempDir(), "kh"), TrustFingerprint: e.cam.HostFingerprint()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	script, _ := assets.Script("akam_authkeys.sh")
	only, _ := os.ReadFile(sharedPub(e))
	ctx := context.Background()
	if _, err := c.Run(ctx, "cat > /tmp/akam_authkeys.sh", strings.NewReader(string(script))); err != nil {
		t.Fatal(err)
	}
	res, err := c.Run(ctx, "sh /tmp/akam_authkeys.sh remove", strings.NewReader(string(only)))
	if err != nil {
		t.Fatal(err)
	}
	if res.Status != 3 || !strings.Contains(string(res.Stdout), "AUTHKEYS=REFUSED_LAST_KEY") {
		t.Errorf("status %d stdout %q", res.Status, res.Stdout)
	}
	list, _ := e.cam.AuthorizedKeys()
	if got := keyLinesOf(t, list); len(got) != 1 || got[0] != pubLine(t, sharedPub(e)) {
		t.Errorf("the only key was removed: %q", got)
	}
	if !canLogin(t, e, e.key, "") {
		t.Error("the only key stopped working")
	}
	// a line that is not an ed25519 key is refused before anything is read
	res, _ = c.Run(ctx, "sh /tmp/akam_authkeys.sh add", strings.NewReader("ssh-rsa AAAAB3Nza x\n"))
	if res.Status != 2 || !strings.Contains(string(res.Stdout), "AUTHKEYS=BAD_KEY") {
		t.Errorf("rsa: status %d stdout %q", res.Status, res.Stdout)
	}
}

// The write lock: it goes on before the key list is changed and off once the key is checked or rolled back.
type lockEventsUI struct {
	*ui.Plain
	mu     sync.Mutex
	events []string
	cmds   func() []string
}

func (u *lockEventsUI) OpPlan([]string)      {}
func (u *lockEventsUI) OpStart(int)          {}
func (u *lockEventsUI) OpDone(int, string)   {}
func (u *lockEventsUI) OpStatus(string)      {}
func (u *lockEventsUI) OpBytes(int64, int64) {}
func (u *lockEventsUI) OpEnd()               {}
func (u *lockEventsUI) OpWriting(on bool) {
	adds := 0
	for _, c := range u.cmds() {
		if c == "sh /tmp/akam_authkeys.sh add" {
			adds++
		}
	}
	u.mu.Lock()
	u.events = append(u.events, map[bool]string{true: "lock", false: "unlock"}[on]+"@adds="+string(rune('0'+adds)))
	u.mu.Unlock()
}

func TestKeyChangeTakesTheWriteLockAroundTheWrite(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	var lu *lockEventsUI
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, sec *state.Secrets, dry bool) ui.UI {
		lu = &lockEventsUI{Plain: cli.NewPlainUI(o, st, log, sec, dry), cmds: e.cam.Commands}
		return lu
	}
	r := runWith(e, h, "y\n\n", keyArgs(e, "key", "use", work+".pub")...)
	expectRC(t, r, 0)
	lu.mu.Lock()
	defer lu.mu.Unlock()
	if len(lu.events) < 2 || lu.events[0] != "lock@adds=0" || lu.events[1] != "unlock@adds=1" {
		t.Errorf("write lock events: %v", lu.events)
	}
}

// ---------------------------------------------------------------- first install

// newKeyCardWorld is the SD world with every step before prepare-flash-sd done (valid image, stock backup).
func newKeyCardWorld(t *testing.T) *sdWorld {
	t.Helper()
	w := newSDWorld(t)
	img := validImage(t)
	seedFlashCard(t, w, img, img)
	return w
}

func TestFirstInstallKeyChoiceSharedIsTheDefault(t *testing.T) {
	w := newKeyCardWorld(t)
	r := w.run("\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "  Which SSH key should open this camera?",
		"    1) Shared key ~/.ssh/akam (all your cameras)",
		"    2) A key for this camera only (~/.ssh/akam-default)",
		"Using ~/.ssh/akam.pub")
	if v := w.readState("ssh_pubkey"); v != sharedPub(w.env) {
		t.Errorf("ssh_pubkey = %q", v)
	}
	if _, err := os.Stat(ownPriv(w.env, "default")); err == nil {
		t.Error("a per-camera key was created without being chosen")
	}
}

func TestFirstInstallCreatesAKeyForThisCameraOnly(t *testing.T) {
	w := newKeyCardWorld(t)
	// choose 2, create it, passphrase twice, then the erase question
	r := w.run("2\ny\nhunter2-pass\nhunter2-pass\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "    2) A key for this camera only (~/.ssh/akam-default)", "Created ~/.ssh/akam-default")
	contains(t, r.err, "Passphrase for the new key (recommended; not shown):", "Same passphrase again:")
	own := ownPriv(w.env, "default")
	if st, err := os.Stat(own); err != nil || (runtime.GOOS != "windows" && st.Mode().Perm() != 0o600) {
		t.Errorf("private key: %v %v", st, err)
	}
	if _, closeFn, err := camssh.LoadSigner(own, func() (string, error) { return "hunter2-pass", nil }); err != nil {
		t.Errorf("LoadSigner: %v", err)
	} else {
		closeFn()
	}
	if _, _, err := camssh.LoadSigner(own, func() (string, error) { return "wrong", nil }); err == nil {
		t.Error("a wrong passphrase must be refused")
	}
	if c := pubComment(t, own+".pub"); c != "akam-default" {
		t.Errorf("comment %q", c)
	}
	if v := w.readState("ssh_pubkey"); v != own+".pub" {
		t.Errorf("ssh_pubkey = %q", v)
	}
	// the card carries that key, and the shared key stays untouched
	notContains(t, w.allText(t), "hunter2-pass")
	notContains(t, r.out+r.err+w.logText(), "hunter2-pass")
}

func readStateOf(t *testing.T, e *env, camera, key string) string {
	t.Helper()
	b, _ := os.ReadFile(filepath.Join(e.state, "cameras", camera, "state"))
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, key+"="); ok {
			return v
		}
	}
	return ""
}

func TestFirstInstallCreatesTheSharedKeyWhenMissing(t *testing.T) {
	w := newKeyCardWorld(t)
	for _, f := range []string{w.key, w.key + ".pub"} {
		if err := os.Remove(f); err != nil {
			t.Fatal(err)
		}
	}
	// empty passphrase is refused once (default No), then given twice
	r := w.run("1\ny\n\n\nsecret-pass-2\nsecret-pass-2\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "There is no ~/.ssh/akam. Akam creates an ed25519 key pair for it:",
		"WARNING: a key without a passphrase works for anyone, or any program, that can read your files.")
	contains(t, r.err, "Create the key WITHOUT a passphrase? [y/N]")
	if _, closeFn, err := camssh.LoadSigner(w.key, func() (string, error) { return "secret-pass-2", nil }); err != nil {
		t.Errorf("LoadSigner: %v", err)
	} else {
		closeFn()
	}
	if c := pubComment(t, w.key+".pub"); c != "akam" {
		t.Errorf("comment %q", c)
	}
}

func TestFirstInstallEmptyPassphraseOnlyAfterAYes(t *testing.T) {
	w := newKeyCardWorld(t)
	for _, f := range []string{w.key, w.key + ".pub"} {
		os.Remove(f)
	}
	r := w.run("1\ny\n\ny\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	if s, closeFn, err := camssh.LoadSigner(w.key, nil); err != nil || s == nil {
		t.Errorf("a key made without a passphrase must load without one: %v", err)
	} else {
		closeFn()
	}
}

func TestFirstInstallPassphrasesThatDifferStopBeforeAnyFileIsWritten(t *testing.T) {
	w := newKeyCardWorld(t)
	r := w.run("2\ny\none\ntwo\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "the passphrases differ.")
	if _, err := os.Stat(ownPriv(w.env, "default")); err == nil {
		t.Error("a key was written")
	}
	w.untouched(t)
}

func TestFirstInstallNeverOverwritesAnExistingKey(t *testing.T) {
	w := newKeyCardWorld(t)
	own := ownPriv(w.env, "default")
	if err := camssh.GenerateKey(own, "mine", nil); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(own)
	r := w.run("2\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "Using ~/.ssh/akam-default.pub")
	if after, _ := os.ReadFile(own); string(after) != string(before) {
		t.Error("an existing key was overwritten")
	}
	if err := camssh.GenerateKey(own, "x", nil); err == nil {
		t.Error("GenerateKey overwrote a key")
	}
}

func TestFirstInstallListsOtherEd25519KeysAfterTheTwoChoices(t *testing.T) {
	w := newKeyCardWorld(t)
	work := filepath.Join(w.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	r := w.run("3\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "    3) "+work+".pub", "Using "+work+".pub (its private key stays on this PC")
	if v := w.readState("ssh_pubkey"); v != work+".pub" {
		t.Errorf("ssh_pubkey = %q", v)
	}
}

func TestFirstInstallRehearsalAsksNothingAndCreatesNothing(t *testing.T) {
	w := newKeyCardWorld(t)
	for _, f := range []string{w.key, w.key + ".pub"} {
		os.Remove(f)
	}
	if b, err := os.ReadFile(filepath.Join(w.state, "state")); err == nil { // the rehearsal has its own state file
		os.WriteFile(filepath.Join(w.state, "state.dry"), b, 0o600)
	}
	r := w.run("", "--dry-run", "prepare-flash-sd", "--device", "/dev/sdb")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD ask: Which SSH key should open this camera?", "WOULD create an ed25519 key ~/.ssh/akam")
	if _, err := os.Stat(w.key); err == nil {
		t.Error("a rehearsal created a key")
	}
}

// ---------------------------------------------------------------- options form

func TestOptionsFormShowsAndChangesTheKey(t *testing.T) {
	e := formEnv(t)
	e.needHelpers()
	fs := &formSession{}
	fs.script = func(n int, spec ui.FormSpec) ui.FormResult {
		switch n {
		case 0:
			row := rowOf(spec, "ro.Security.SSH key")
			if row.Group != "Security" || row.Kind != ui.FormReadonly || row.Value != "shared (~/.ssh/akam)" {
				t.Errorf("SSH key row: %+v", row)
			}
			if !strings.Contains(row.Note, "k changes the SSH key") {
				t.Errorf("note %q", row.Note)
			}
			if !hasKey(spec, "k") {
				t.Error("no k key")
			}
			return ui.FormResult{Key: "k", Group: "Security"}
		}
		return ui.FormResult{}
	}
	// pick "A key for this camera only", create it, passphrase twice, confirm, keep the old key
	r := runForm(t, e, fs, "2\ny\npw-form\npw-form\ny\n\n")
	expectRC(t, r, 0)
	contains(t, r.out, "New key:     this camera only (~/.ssh/akam-salon)", "The new key works and is now used for camera salon")
	if got := readStateOf(t, e, "salon", "ssh_pubkey"); got != ownPriv(e, "salon")+".pub" {
		t.Errorf("ssh_pubkey = %q", got)
	}
	if len(fs.specs) < 2 {
		t.Fatalf("the form was not opened again: %d", len(fs.specs))
	}
}

func TestOptionsFormKeyCancelChangesNothing(t *testing.T) {
	e := formEnv(t)
	e.needHelpers()
	fs := &formSession{}
	fs.script = func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "k", Group: "Security"}
		}
		return ui.FormResult{}
	}
	r := runForm(t, e, fs, "4\n")
	expectRC(t, r, 0)
	contains(t, r.out, "Nothing was changed.")
	if list, _ := e.cam.AuthorizedKeys(); len(keyLinesOf(t, list)) != 1 {
		t.Errorf("list: %q", list)
	}
}

// authkeysCommands counts the commands of the key helper the camera received.
func authkeysCommands(e *env) int {
	n := 0
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "akam_authkeys.sh") {
			n++
		}
	}
	return n
}

func pubComment(t *testing.T, pubFile string) string {
	t.Helper()
	b, err := os.ReadFile(pubFile)
	if err != nil {
		t.Fatal(err)
	}
	_, c, _, _, err := gossh.ParseAuthorizedKey(b)
	if err != nil {
		t.Fatal(err)
	}
	return c
}
