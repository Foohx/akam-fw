package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

// writePub writes the public key of the signer as a .pub file with the given comment.
func writePub(t *testing.T, b kittest.Built, comment string) string {
	t.Helper()
	line := strings.TrimSpace(string(gossh.MarshalAuthorizedKey(b.Signer.PublicKey())))
	if comment != "" {
		line += " " + comment
	}
	f := filepath.Join(t.TempDir(), "mine.pub")
	if err := os.WriteFile(f, []byte(line+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return f
}

// The test key is not the official one: it only works once the user trusts it, and then every success names it.
func TestTrustKeyAcceptsAndLabelsAUserKey(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, nil) // no AKAM_ALLOWED_SIGNERS: the embedded official key
	pub := writePub(t, b, "ci-builds")

	r := e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "INVALID signature")

	// the warning is shown and a plain No adds nothing
	r = e.run("n\n", "trust-key", "add", pub, "--name", "mine")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "Trust a signing key", "the key was not added.")
	r = e.run("", "trust-key", "list")
	contains(t, r.out, "Official key", "Your trusted keys: none")

	r = e.run("y\n", "trust-key", "add", pub, "--name", "mine")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "can sign firmware", "Trusted: mine SHA256:")
	r = e.run("", "trust-key", "list")
	contains(t, r.out, "mine", "SHA256:")
	if st, err := os.Stat(filepath.Join(e.state, "trusted_signers")); err != nil || (st.Mode().Perm()&0o077 != 0 && filepath.Separator == '/' && !strings.Contains(os.Getenv("OS"), "Windows")) {
		t.Errorf("trusted_signers: %v %v", err, st)
	}

	r = e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "ok: signature valid", `ok: signed by your key "mine"`, `Signed by your key "mine".`)
	notContains(t, r.out+r.err, "official Akam release key")

	// removed: refused again
	r = e.run("", "trust-key", "remove", "mine")
	expectRC(t, r, 0)
	r = e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "INVALID signature")
	r = e.run("", "trust-key", "remove", "mine")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "no trusted key is labelled")
}

// The official key still signs, and is named as such, with a user key present.
func TestOfficialKeyIsNamed(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, &b) // the test hook makes the test key "the official one"
	other := kittest.Build(t, t.TempDir(), kittest.Spec{})
	r := e.run("", "trust-key", "add", writePub(t, other, "other"), "--yes")
	expectRC(t, r, 0)
	r = e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "ok: signed by the official Akam release key", "Signed by the official Akam release key.")
}

func TestTrustKeyRefusesBadFilesAndLabels(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e := newEnv(t)
	dir := t.TempDir()
	good := strings.TrimSpace(string(gossh.MarshalAuthorizedKey(b.Signer.PublicKey())))
	cases := map[string]string{
		"empty.pub":    "",
		"garbage.pub":  "not a key\n",
		"private.pub":  "-----BEGIN OPENSSH " + "PRIVATE KEY-----\nAAAA\n-----END OPENSSH " + "PRIVATE KEY-----\n",
		"rsa.pub":      "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC7 x\n",
		"two.pub":      good + " a\n" + good + " b\n",
		"options.pub":  `command="x" ` + good + "\n",
		"truncated":    good[:len(good)-12] + "\n",
		"ctrl.pub":     good + " a\x01b\n",
		"comment-only": "# just a comment\n",
	}
	for name, content := range cases {
		f := filepath.Join(dir, name)
		if err := os.WriteFile(f, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		r := e.run("", "trust-key", "add", f, "--yes")
		if r.rc == 0 {
			t.Errorf("%s: accepted", name)
		}
	}
	if _, err := os.Stat(filepath.Join(e.state, "trusted_signers")); err == nil {
		t.Error("a refused key left a trust file")
	}
	r := e.run("", "trust-key", "add", filepath.Join(dir, "missing.pub"), "--yes")
	expectRC(t, r, 1)
	pub := writePub(t, b, "")
	for _, l := range []string{"official", "a b", "../x", "-x", strings.Repeat("a", 41)} {
		r = e.run("", "trust-key", "add", pub, "--name", l, "--yes")
		if r.rc == 0 {
			t.Errorf("label %q accepted", l)
		}
	}
	// duplicate key and duplicate label
	expectRC(t, e.run("", "trust-key", "add", pub, "--name", "one", "--yes"), 0)
	if e.run("", "trust-key", "add", pub, "--name", "two", "--yes").rc == 0 {
		t.Error("the same key twice")
	}
	// a damaged trust file is reported by list and never silently used
	if err := os.WriteFile(filepath.Join(e.state, "trusted_signers"), []byte("garbage\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = e.run("", "trust-key", "list")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "damaged")
	// --name only belongs to trust-key
	if e.run("", "status", "--name", "x").rc == 0 {
		t.Error("--name accepted elsewhere")
	}
	// dry run writes nothing
	e2 := newEnv(t)
	r = e2.run("", "trust-key", "add", pub, "--dry-run")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD trust this key")
	if _, err := os.Stat(filepath.Join(e2.state, "trusted_signers")); err == nil {
		t.Error("dry run wrote the trust file")
	}
}

func TestRepoOptionValidation(t *testing.T) {
	e := newEnv(t)
	dryState(t, e) // a refusal must come from the name, not from a missing step
	bad := []string{"owner", "owner/", "/name", "a/b/c", "../x/y", "owner/..", "owner/.", "owner/na..me", "https://evil.example/x/y",
		"evil.example:443/x", "user@evil.example/x", "owner/name?x=1", "owner/name#f", "owner\\name", "owner/na%2Fme", "owner/na me",
		"-owner/name", "own--er/name", "owner-/name", strings.Repeat("a", 40) + "/n", "owner/" + strings.Repeat("n", 101), "o/n\n"}
	for _, repo := range bad {
		r := e.run("", "get-kit", "--repo", repo, "--dry-run")
		if r.rc == 0 || !strings.Contains(r.err+r.out, "--repo must be a GitHub owner/name") {
			t.Errorf("--repo %q: rc %d, %s%s", repo, r.rc, r.out, r.err)
		}
		if r2 := e.run("", "release-source", repo); r2.rc == 0 {
			t.Errorf("release-source %q accepted", repo)
		}
	}
	if _, err := os.Stat(filepath.Join(e.state, "release_source")); err == nil {
		t.Error("a bad source was saved")
	}
	// --repo belongs to the download commands only
	if r := e.run("", "status", "--repo", "a/b"); r.rc == 0 {
		t.Error("--repo accepted by status")
	}
	r := e.run("", "get-kit", "--repo", "a/b", "--dry-run")
	if strings.Contains(r.err+r.out, "--repo must be") {
		t.Errorf("a good name was refused: %s", r.err)
	}
}

func TestRepoBuildsTheDownloadAddressAndIsShown(t *testing.T) {
	e := newEnv(t)
	dryState(t, e)
	r := e.run("", "get-kit", "--version", "1.2.3", "--repo", "someone/my-fork.v2", "--dry-run")
	expectRC(t, r, 0)
	contains(t, r.out, "https://github.com/someone/my-fork.v2/releases/download/firmware-v1.2.3/akam-kit-lsc-indoor-1080p-ak3918-1.2.3.tar.xz",
		"Release source: someone/my-fork.v2 (NOT the official source")
	r = e.run("", "get-kit", "--version", "1.2.3", "--dry-run")
	contains(t, r.out, "https://github.com/Foohx/akam-fw/releases/download/firmware-v1.2.3/", "Release source: Foohx/akam-fw (the official source)")
	notContains(t, r.out, "someone/")
}

func TestReleaseSourceIsPersistent(t *testing.T) {
	e := newEnv(t)
	dryState(t, e)
	r := e.run("", "release-source")
	expectRC(t, r, 0)
	contains(t, r.out, "Release source: Foohx/akam-fw (the official source)")
	r = e.run("", "release-source", "me/fork", "--dry-run")
	contains(t, r.out, "WOULD")
	if _, err := os.Stat(filepath.Join(e.state, "release_source")); err == nil {
		t.Error("dry run saved the source")
	}
	expectRC(t, e.run("", "release-source", "me/fork"), 0)
	r = e.run("", "release-source", "show")
	contains(t, r.out, "Release source: me/fork (NOT the official source")
	r = e.run("", "get-kit", "--version", "1.0.0", "--dry-run")
	contains(t, r.out, "https://github.com/me/fork/releases/download/firmware-v1.0.0/")
	// --repo wins for one run, the saved one stays
	r = e.run("", "get-kit", "--version", "1.0.0", "--repo", "other/x", "--dry-run")
	contains(t, r.out, "https://github.com/other/x/releases/")
	r = e.run("", "release-source", "show")
	contains(t, r.out, "me/fork")
	expectRC(t, e.run("", "release-source", "default"), 0)
	r = e.run("", "release-source", "show")
	contains(t, r.out, "(the official source)")
	// naming the official one stores nothing
	expectRC(t, e.run("", "release-source", "Foohx/akam-fw"), 0)
	if _, err := os.Stat(filepath.Join(e.state, "release_source")); err == nil {
		t.Error("the official source was stored")
	}
}

// A download from another source is never accepted unsigned, a trusted key present or not, --allow-unsigned or not;
// and the same for the 2K path.
func TestUnsignedDownloadRefusedWithTrustedKeyAndCustomRepo(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{Unsigned: true})
	e := getKitEnv(t, newKitServer(t, b, true), &b)
	expectRC(t, e.run("", "trust-key", "add", writePub(t, b, "k"), "--yes"), 0)
	for _, extra := range [][]string{nil, {"--allow-unsigned"}} {
		args := append([]string{"get-kit", "--version", "1.0.0", "--repo", "me/fork"}, extra...)
		r := e.run("y\n", args...)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "must be signed")
		if e.readState("step_get_kit_done") != "" {
			t.Error("marked done")
		}
	}
	// a wrongly signed kit from a custom repo, the trusted key being another one
	s := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e = getKitEnv(t, newKitServer(t, s, false), nil)
	expectRC(t, e.run("", "trust-key", "add", writePub(t, b, "k"), "--yes"), 0)
	r := e.run("", "get-kit", "--version", "1.0.0", "--repo", "me/fork")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "INVALID signature")
}

func TestAppfsBuildAcceptsAUserKey(t *testing.T) {
	b := build2KKit(t, kittest.Spec{})
	_, bdir := appfsDirs(t)
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, nil)
	e.writeState("camera_model=" + rotModel + "\n")
	out := filepath.Join(t.TempDir(), "o")
	r := e.run("", "appfs-build", "--version", "1.0.0", "--backup-dir", bdir, "--out", out)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "INVALID signature")
	expectRC(t, e.run("", "trust-key", "add", writePub(t, b, "fork"), "--yes"), 0)
	r = e.run("", "appfs-build", "--version", "1.0.0", "--backup-dir", bdir, "--out", out)
	contains(t, r.out+r.err, "Release source: Foohx/akam-fw (the official source)", `Signed by your key "fork".`, "Kit verified (version 1.0.0")
}

// Settings reaches the screen: add a key, see it, remove it, change the source.
func TestSettingsMenuForOwnBuilds(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e := newEnv(t)
	akamState(e, "")
	pub := writePub(t, b, "menu")
	r := e.run("6\n3\n1\n"+pub+"\ny\n3\nme/fork\n2\nmenu\n4\n4\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Your own firmware builds", "Trusted: menu", "Release source: me/fork", "Removed.")
}

// dryState prepares the state a rehearsal of get-kit needs (a rehearsal keeps its own state.dry).
func dryState(t *testing.T, e *env) {
	t.Helper()
	e.writeState("camera_model=lsc-indoor-1080p-ak3918\n")
	if err := os.WriteFile(filepath.Join(e.state, "state.dry"), []byte("step_doctor_done=2026-11-11T00:00:00Z\ncamera_model=lsc-indoor-1080p-ak3918\n"), 0o600); err != nil {
		t.Fatal(err)
	}
}
