package cli

// Which SSH key opens a camera: the choice at the first install (sshChoosePubkey, in sdcmds.go), what a
// camera uses today (keyShow, the options row, status), and the change of the key of a camera that
// already runs Akam (keyRotate): add the new key, prove it with a second login, only then remember it,
// and offer (default No) to take the old one out. The camera side is internal/assets
// akam_authkeys.sh, run from the camera's RAM.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	gossh "golang.org/x/crypto/ssh"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The three fixed command lines of the key change (see remote.go: the guard allows exactly these).
const (
	remoteAuthkeysPut    = "cat > /tmp/akam_authkeys.sh"
	remoteAuthkeysAdd    = "sh /tmp/akam_authkeys.sh add"
	remoteAuthkeysRemove = "sh /tmp/akam_authkeys.sh remove"
)

// tilde writes a path under the home folder as ~/...: it is what the menus and the docs show.
func tilde(p string) string {
	h, _ := os.UserHomeDir()
	if h != "" && (p == h || strings.HasPrefix(p, h+string(filepath.Separator))) {
		return "~" + filepath.ToSlash(strings.TrimPrefix(p, h))
	}
	return p
}

// keyKindText is how a key reads on a screen: "shared (~/.ssh/akam)", "this camera only (~/.ssh/akam-NAME)"
// or "custom (PATH)". pub is the .pub file.
func keyKindText(kind, pub string) string {
	priv := tilde(strings.TrimSuffix(pub, ".pub"))
	switch kind {
	case camssh.KindShared:
		return "shared (" + priv + ")"
	case camssh.KindOwn:
		return "this camera only (" + priv + ")"
	}
	return "custom (" + priv + ")"
}

// keyPub is the public key file the current camera uses: --pubkey, the state, or the default.
func (a *App) keyPub() string { return a.keyPath() + ".pub" }

// keyRowText is the "SSH key" line of the options screen and of status (no fingerprint: a row is short).
func (a *App) keyRowText() string {
	pub := a.keyPub()
	return keyKindText(camssh.KindOf(pub, a.Cams.Name), pub)
}

// keyBrief is the kind of key of another camera, read from its own state ("" when none was chosen).
func (a *App) keyBrief(camera string) string {
	pub := a.Cams.ReadKey(camera, "ssh_pubkey")
	if pub == "" {
		return ""
	}
	return camssh.KindOf(pub, camera)
}

// keyShow prints which key this camera uses. It reads only this PC.
func (a *App) keyShow() error {
	a.UI.Bold(fmt.Sprintf("SSH key of camera %s", a.Cams.Name))
	chosen := a.St.Get("ssh_pubkey")
	pub := a.keyPub()
	priv := strings.TrimSuffix(pub, ".pub")
	switch {
	case a.Opt.Identity != "":
		a.UI.Say("  --identity " + tilde(a.Opt.Identity) + " is used for this run (it overrides the key below).")
	case chosen == "":
		a.UI.Say("  No key was chosen for this camera yet: the shared key is used (the card step asks).")
	}
	a.UI.Say("  Key: " + keyKindText(camssh.KindOf(pub, a.Cams.Name), pub))
	if fp := camssh.PubFingerprint(pub); fp != "" {
		a.UI.Say("  Fingerprint: " + fp)
	} else {
		a.UI.Say("  Fingerprint: not available (" + tilde(pub) + " is missing or not a public key)")
	}
	if exists(priv) {
		a.UI.Say("  Private key: " + tilde(priv) + " (stays on this computer)")
	} else {
		a.UI.Warn("the private key " + tilde(priv) + " is not on this computer: this camera cannot be reached with it.")
	}
	a.UI.Say(fmt.Sprintf("  Change it: %s key use-shared | use-new | use FILE.pub   (other cameras are never touched)", Prog))
	return nil
}

// cmdKey is the "key" command: show, use-shared, use-new, use FILE.pub.
func (a *App) cmdKey(args []string) error {
	act := "show"
	if len(args) > 0 {
		act = args[0]
	}
	bad := usageErr("akam: key needs one of: show | use-shared | use-new | use FILE.pub")
	var ch keyChoice
	switch act {
	case "show":
		if len(args) > 1 {
			return bad
		}
		return a.keyShow()
	case "use-shared":
		if len(args) != 1 {
			return bad
		}
		ch = keyChoice{kind: camssh.KindShared, pub: camssh.SharedKeyPath() + ".pub"}
	case "use-new":
		if len(args) != 1 {
			return bad
		}
		ch = keyChoice{kind: camssh.KindOwn, pub: camssh.OwnKeyPath(a.Cams.Name) + ".pub"}
	case "use":
		if len(args) != 2 || args[1] == "" || strings.HasPrefix(args[1], "-") {
			return bad
		}
		ch = keyChoice{kind: camssh.KindCustom, pub: args[1]}
	default:
		return bad
	}
	return a.keyChange(&ch, "", nil)
}

// keyChoice is the key a camera is to use from now on. pub is its .pub file; kind is shared, own or
// custom. A zero choice means "ask".
type keyChoice struct{ kind, pub string }

// keyCommentFor is the comment of a key made for kind.
func (a *App) keyCommentFor(kind string) string {
	if kind == camssh.KindOwn {
		return "akam-" + a.Cams.Name
	}
	return "akam"
}

// askKeyChoice asks which key the camera should use from now on (the options screen, "k").
func (a *App) askKeyChoice() (keyChoice, bool) {
	cur := camssh.KindOf(a.keyPub(), a.Cams.Name)
	mark := func(kind string) string {
		if kind == cur {
			return "  (in use now)"
		}
		return ""
	}
	items := []string{
		fmt.Sprintf("Shared key %s (all your cameras)%s", tilde(camssh.SharedKeyPath()), mark(camssh.KindShared)),
		fmt.Sprintf("A key for this camera only (%s)%s", tilde(camssh.OwnKeyPath(a.Cams.Name)), mark(camssh.KindOwn)),
		"Another key: type the path of its .pub file",
		"Cancel",
	}
	i, err := a.UI.Pick("Which SSH key should open this camera from now on?", items)
	if err != nil || i == 3 {
		return keyChoice{}, false
	}
	switch i {
	case 0:
		return keyChoice{kind: camssh.KindShared, pub: camssh.SharedKeyPath() + ".pub"}, true
	case 1:
		return keyChoice{kind: camssh.KindOwn, pub: camssh.OwnKeyPath(a.Cams.Name) + ".pub"}, true
	}
	p, err := a.UI.Prompt("  Path of the public key (.pub, ed25519; empty cancels):", "")
	if err != nil || strings.TrimSpace(p) == "" {
		return keyChoice{}, false
	}
	return keyChoice{kind: camssh.KindCustom, pub: strings.TrimSpace(p)}, true
}

// keyEnsure makes sure the key pair behind pub exists: it uses it when it does, and creates it (asking
// first, then the passphrase) when it does not and mayCreate (a key the user named is never created). The passphrase of a key made here is returned, so the
// check login of a key change need not ask it again.
func (a *App) keyEnsure(pub, comment string, mayCreate bool) (pass string, err error) {
	priv := strings.TrimSuffix(pub, ".pub")
	if exists(pub) {
		if err := pubkeyCheck(a, pub); err != nil {
			return "", err
		}
		a.UI.Say(fmt.Sprintf("  Using %s (its private key stays on this PC; only the .pub goes to the camera).", tilde(pub)))
		if !exists(priv) {
			a.UI.Warn("the private key " + tilde(priv) + " is not on this computer: you could not log in with this key.")
		}
		return "", nil
	}
	if !mayCreate {
		return "", a.UI.Fail("public key not found: "+pub, "give the .pub file of an existing ed25519 key.")
	}
	if exists(priv) {
		return "", a.UI.Fail(fmt.Sprintf("%s exists but %s does not.", tilde(priv), tilde(pub)),
			fmt.Sprintf("recreate the public key (ssh-keygen -y -f %s > %s), or choose another key.", tilde(priv), tilde(pub)))
	}
	a.UI.Say(fmt.Sprintf("  There is no %s. Akam creates an ed25519 key pair for it:", tilde(priv)))
	if !a.confirm("  Create it now (you will be asked for a passphrase)?") {
		return "", a.UI.Fail("no SSH key.", "create one, then pass --pubkey FILE.")
	}
	return a.keyCreate(priv, comment)
}

// keyCreate generates the key pair at priv: the passphrase is asked twice, without echo. An empty
// one is accepted only after an explicit warning and a yes (default No). Nothing is overwritten.
func (a *App) keyCreate(priv, comment string) (string, error) {
	var pass string
	for try := 0; ; try++ {
		p1, err := a.UI.Secret("  Passphrase for the new key (recommended; not shown):")
		if err != nil {
			return "", ui.ErrFailed
		}
		if p1 == "" {
			a.UI.Warn("a key without a passphrase works for anyone, or any program, that can read your files.")
			if ok, _ := a.UI.AskYN("  Create the key WITHOUT a passphrase?"); ok {
				break
			}
			if try >= 2 {
				return "", a.UI.Fail("no passphrase was chosen.", "run this step again.")
			}
			continue
		}
		p2, err := a.UI.Secret("  Same passphrase again:")
		if err != nil || p2 != p1 {
			return "", a.UI.Fail("the passphrases differ.", "run this step again.")
		}
		a.Secrets.Add(p1)
		pass = p1
		break
	}
	var pp []byte
	if pass != "" {
		pp = []byte(pass)
	}
	if err := camssh.GenerateKey(priv, comment, pp); err != nil {
		return "", a.UI.Fail("creating the key failed: "+err.Error(), "nothing was overwritten.")
	}
	a.UI.Say(fmt.Sprintf("  Created %s (private, mode 600) and %s.pub; fingerprint %s.", tilde(priv), tilde(priv), camssh.PubFingerprint(priv+".pub")))
	return pass, nil
}

// authLine is the line sent to akam_authkeys.sh for a public key: type, key and a comment the script accepts.
func authLine(k gossh.PublicKey, comment string) string {
	if !regexp.MustCompile(`^[A-Za-z0-9._@+-]{1,40}$`).MatchString(comment) {
		comment = "akam"
	}
	return camssh.AuthorizedLine(k, comment)
}

func sameKey(a, b gossh.PublicKey) bool { return bytes.Equal(a.Marshal(), b.Marshal()) }

// pubComment reads the comment of a public key file ("" when unreadable).
func pubComment(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	_, c, _, _, err := gossh.ParseAuthorizedKey(b)
	if err != nil {
		return ""
	}
	return c
}

// authkeysRun sends one add or remove of akam_authkeys.sh over c and returns the result word
// (AUTHKEYS=...). The key line goes through standard input, never on the command line or in a log.
func (a *App) authkeysRun(c *camssh.Client, script []byte, mode, line, fp string) (string, error) {
	if res, err := a.run(c, remoteAuthkeysPut, bytes.NewReader(script), "'cat > /tmp/akam_authkeys.sh' < akam_authkeys.sh"); err != nil || res.Status != 0 {
		if err == nil {
			err = fmt.Errorf("the copy of the helper to the camera's RAM failed (status %d)", res.Status)
		}
		return "", err
	}
	cmd := remoteAuthkeysAdd
	if mode == "remove" {
		cmd = remoteAuthkeysRemove
	}
	res, err := a.run(c, cmd, strings.NewReader(line+"\n"), fmt.Sprintf("'%s' < key line (%s)", cmd, fp))
	if err != nil {
		return "", err
	}
	word := ""
	for _, l := range strings.Split(string(res.Stdout), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "AUTHKEYS="); ok {
			word = v
		}
	}
	if res.Status != 0 && word == "" {
		word = "FAILED"
	}
	if res.Status != 0 {
		a.Log.Line("authkeys %s status=%d result=%s", mode, res.Status, word)
		return word, fmt.Errorf("the camera answered %s", word)
	}
	return word, nil
}

// verifyKeyLogin opens a NEW login with the key at priv (the host key must already be pinned and
// must match) and runs a trivial command.
func (a *App) verifyKeyLogin(ip, priv string, pass *string) error {
	nc, err := a.dialKey(ip, priv, pass)
	if err != nil {
		return err
	}
	defer nc.Close()
	res, err := a.run(nc, remoteTrue(), nil, "")
	if err != nil {
		return err
	}
	if res.Status != 0 {
		return fmt.Errorf("the check command ended with status %d", res.Status)
	}
	return nil
}

// keyChange changes the key of the current camera: ch (nil or zero: ask), the camera address, and an
// SSH session already open with the current key (nil: it is opened here). It is the whole flow
// of "key use-*" and of the "k" key of the options screen.
func (a *App) keyChange(ch *keyChoice, ip string, c *camssh.Client) error {
	var err error
	if ip == "" {
		if ip, err = a.camNeedIP(); err != nil {
			return err
		}
	}
	if a.UI.Dry() {
		a.banner("SSH key - " + a.Cams.Name)
		a.UI.Say("  WOULD connect with the current key, create the new key if it does not exist (passphrase asked twice),")
		a.UI.Say("  ask a yes/no (default No), then over SSH: copy akam_authkeys.sh to the camera's RAM and run it to ADD the new")
		a.UI.Say("  public key to /etc/config/dropbear/authorized_keys (a temp file in the same folder, synced, moved into place).")
		a.UI.Say("  WOULD then log in again with the NEW key; only if that works the choice is saved for this camera.")
		a.UI.Say("  WOULD finally ask (default No) whether to remove the old key from the camera. Other cameras are never touched.")
		return nil
	}
	if c == nil {
		if c, err = a.connectTools(ip); err != nil {
			return err
		}
		defer c.Close()
	}
	if ch == nil || ch.pub == "" {
		got, ok := a.askKeyChoice()
		if !ok {
			a.UI.Say("  Nothing was changed.")
			return nil
		}
		ch = &got
	}
	return a.keyRotate(ip, c, *ch)
}

func (a *App) keyRotate(ip string, c *camssh.Client, ch keyChoice) error {
	script, err := a.embeddedScript("akam_authkeys.sh")
	if err != nil {
		return a.UI.Fail("akam_authkeys.sh is not available: "+err.Error(), "")
	}
	oldPriv := a.keyPath()
	oldPub := oldPriv + ".pub"
	newPass, err := a.keyEnsure(ch.pub, a.keyCommentFor(ch.kind), ch.kind != camssh.KindCustom)
	if err != nil {
		return err
	}
	if err := pubkeyCheck(a, ch.pub); err != nil {
		return err
	}
	newPriv := strings.TrimSuffix(ch.pub, ".pub")
	if !exists(newPriv) {
		return a.UI.Fail("the private key "+tilde(newPriv)+" is not on this computer.",
			"a key that cannot log in is never put on the camera; nothing was changed.")
	}
	newKey, err := camssh.ParsePubFile(ch.pub)
	if err != nil {
		return a.UI.Fail(err.Error(), "")
	}
	newFP := gossh.FingerprintSHA256(newKey)
	oldKey, oldErr := camssh.ParsePubFile(oldPub)
	if oldErr == nil && sameKey(oldKey, newKey) {
		a.UI.Say(fmt.Sprintf("  Camera %s already uses this key (%s): nothing to do.", a.Cams.Name, newFP))
		if a.St.Get("ssh_pubkey") != ch.pub {
			a.setState("ssh_pubkey", ch.pub)
		}
		return nil
	}

	// What will change, and the question (default No).
	a.UI.Blank()
	a.UI.Bold(fmt.Sprintf("Change the SSH key of camera %s", a.Cams.Name))
	a.UI.Say("  New key:     " + keyKindText(ch.kind, ch.pub) + "  " + newFP)
	if oldErr == nil {
		a.UI.Say("  Current key: " + keyKindText(camssh.KindOf(oldPub, a.Cams.Name), oldPub) + "  " + gossh.FingerprintSHA256(oldKey))
	}
	a.UI.Say("  1. The new public key is ADDED to the camera's key list (the current key keeps working).")
	a.UI.Say("  2. Akam logs in again with the new key. Only if that works is it remembered for this camera.")
	a.UI.Say("  3. You are then asked whether to remove the current key from the camera (default No).")
	a.UI.Say("  If anything fails the camera still accepts the current key. Your other cameras are not touched.")
	if !a.confirm("  Add the new key to this camera now?") {
		a.UI.Say("  Nothing was changed on the camera.")
		return nil
	}
	if a.stopRequested() {
		a.UI.Warn("Stopped. Nothing was changed on the camera.")
		return ui.ErrFailed
	}

	op := a.op()
	defer op.end()
	op.plan("Add the new key on the camera", "Log in with the new key", "Remember the key for this camera")
	op.start(0)
	op.status("copies akam_authkeys.sh to the camera's RAM and adds the public key to /etc/config/dropbear/authorized_keys")
	op.writing(true)
	newLine := authLine(newKey, pubComment(ch.pub))
	word, err := a.authkeysRun(c, script, "add", newLine, newFP)
	if err != nil || (word != "added" && word != "present") {
		if err == nil {
			err = fmt.Errorf("unexpected answer %q", word)
		}
		if word != "" { // the helper ran and failed: a half-done add (the copy Dropbear reads) is undone too
			a.keyRollback(ip, c, script, newLine, newFP)
		}
		op.writing(false)
		return a.UI.Fail("the camera did not take the new key: "+err.Error()+".", "the current key still works; nothing was saved here.")
	}
	op.done(0, word)
	op.start(1)
	op.status("a new SSH login with the new key; the camera's host key must match the remembered one")
	a.UI.Say("  Key added (" + word + "). Logging in again with the new key ...")
	if verr := a.verifyKeyLogin(ip, newPriv, &newPass); verr != nil {
		a.UI.Err("the new key does not work: " + verr.Error())
		if word == "added" {
			a.keyRollback(ip, c, script, newLine, newFP)
		}
		op.writing(false)
		return a.UI.Fail("the key was not changed.", "the camera still accepts your current key; nothing was saved here.")
	}
	op.done(1, "works")
	op.start(2)
	if err := a.St.Set("ssh_pubkey", ch.pub); err != nil {
		op.writing(false)
		return a.UI.Fail(fmt.Sprintf("the camera accepts the new key, but it could not be saved here: %v.", err),
			fmt.Sprintf("both keys work on the camera; run %s key use ... again.", Prog))
	}
	a.passphrase = nil // the cached passphrase was the one of the old key
	op.writing(false)
	op.done(2, keyKindText(ch.kind, ch.pub))
	a.UI.Good(fmt.Sprintf("  The new key works and is now used for camera %s: %s (%s).", a.Cams.Name, keyKindText(ch.kind, ch.pub), newFP))
	if a.Opt.Pubkey != "" || a.Opt.Identity != "" {
		a.UI.Say("  (--pubkey / --identity still overrides it for runs that pass them.)")
	}

	// The old key: only offered when it is known, and never the one just verified.
	if oldErr != nil {
		a.UI.Say("  The old key's public file was not found, so its removal is not offered; both keys keep working on the camera.")
		return nil
	}
	oldFP := gossh.FingerprintSHA256(oldKey)
	a.UI.Blank()
	a.UI.Say(fmt.Sprintf("  The camera still accepts the old key (%s).", oldFP))
	a.UI.Say("  No keeps both keys working. Yes removes only that key from THIS camera; other cameras keep their lists.")
	if !a.confirm("  Remove the old key from this camera?") {
		a.UI.Say("  The old key was left on the camera.")
		return nil
	}
	return a.keyRemoveOld(ip, script, oldKey, newPriv, &newPass, oldFP)
}

// keyRollback takes a key we just added out of the camera's list again (the check login failed). The
// session c still holds the current key; if it is gone a new login with the current key is tried.
func (a *App) keyRollback(ip string, c *camssh.Client, script []byte, line, fp string) {
	word, err := a.authkeysRun(c, script, "remove", line, fp)
	if err != nil {
		if nc, derr := a.dial(ip); derr == nil {
			defer nc.Close()
			word, err = a.authkeysRun(nc, script, "remove", line, fp)
		}
	}
	if err != nil || (word != "removed" && word != "absent") {
		a.UI.Warn(fmt.Sprintf("could not take the new key (%s) out of the camera again; it does no harm, and the current key still works. Remove it later with the card route if you want.", fp))
		return
	}
	if word == "removed" {
		a.UI.Say("  The new key was taken out of the camera again.")
	}
}

// keyRemoveOld removes the old key over a login made with the NEW key, then proves the new key still
// logs in. If that proof fails the old key is put back.
func (a *App) keyRemoveOld(ip string, script []byte, oldKey gossh.PublicKey, newPriv string, newPass *string, oldFP string) error {
	nc, err := a.dialKey(ip, newPriv, newPass)
	if err != nil {
		return a.UI.Fail("could not log in with the new key to remove the old one: "+err.Error()+".", "the old key was left on the camera.")
	}
	defer nc.Close()
	op := a.op()
	defer op.end()
	op.plan("Remove the old key from the camera", "Log in with the new key again")
	op.start(0)
	op.writing(true)
	defer op.writing(false)
	oldLine := authLine(oldKey, "akam")
	word, err := a.authkeysRun(nc, script, "remove", oldLine, oldFP)
	switch {
	case err != nil && word == "REFUSED_LAST_KEY":
		return a.UI.Fail("the old key is the only key on the camera, so it was not removed.", "nothing was changed.")
	case err != nil:
		return a.UI.Fail("the old key could not be removed: "+err.Error()+".", "both keys may still work; nothing was saved wrongly.")
	case word == "absent":
		a.UI.Say("  The old key was not in the camera's list any more.")
		return nil
	}
	op.done(0, word)
	op.start(1)
	if verr := a.verifyKeyLogin(ip, newPriv, newPass); verr != nil {
		a.UI.Err("the new key no longer logs in after the removal: " + verr.Error())
		if w, rerr := a.authkeysRun(nc, script, "add", oldLine, oldFP); rerr != nil || (w != "added" && w != "present") {
			return a.UI.Fail("the old key could NOT be put back automatically.", "use the flash card route (a new akam/authorized_keys) if you are locked out.")
		}
		return a.UI.Fail("the old key was put back.", "both keys work again; the camera was not locked out.")
	}
	op.done(1, "works")
	a.UI.Good(fmt.Sprintf("  The old key (%s) was removed from this camera; the new key still works.", oldFP))
	return nil
}

// keyCandidates lists the ed25519 public keys of ~/.ssh (one line each) other than skip (private paths).
func keyCandidates(skip ...string) []string {
	h, _ := os.UserHomeDir()
	var out []string
	ms, _ := filepath.Glob(filepath.Join(h, ".ssh", "*.pub"))
next:
	for _, f := range ms {
		for _, s := range skip {
			if filepath.Clean(f) == filepath.Clean(s) {
				continue next
			}
		}
		if b, err := os.ReadFile(f); err == nil && sdcard.CheckPubKey(b) == nil {
			out = append(out, f)
		}
	}
	return out
}
