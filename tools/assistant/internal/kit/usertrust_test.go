package kit_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func TestValidRepo(t *testing.T) {
	for _, ok := range []string{"Foohx/akam-fw", "a/b", "me/my-fork.v2", "me/.github", "a-b/c_d", "x1/y"} {
		if !kit.ValidRepo(ok) {
			t.Errorf("%q refused", ok)
		}
	}
	for _, bad := range []string{"", "a", "a/", "/b", "a/b/c", "a/..", "a/.", "a/b..c", "http://h/a/b", "h:1/a", "u@h/a", "a/b?x", "a/b#x", `a\b`, "a/b%2f", "a /b", "-a/b", "a-/b", "a--b/c"} {
		if kit.ValidRepo(bad) {
			t.Errorf("%q accepted", bad)
		}
	}
}

// A kit signed by a user-trusted key verifies and names it; the same kit without the key is refused.
func TestVerifyTarballUserSigners(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	other := kittest.Build(t, t.TempDir(), kittest.Spec{}) // its key stands for "the official key"
	base := t.TempDir()
	ut := kit.OpenUserTrust(base)
	if _, err := ut.Add("mine", b.Signer.PublicKey()); err != nil {
		t.Fatal(err)
	}
	user, err := ut.Bytes()
	if err != nil || len(user) == 0 {
		t.Fatal(err)
	}
	res, err := kit.VerifyTarball(b.Tarball, kit.Options{AllowedSigners: other.AllowedSigners, UserSigners: user})
	if err != nil {
		t.Fatal(err)
	}
	if res.SignerLabel != `your key "mine"` {
		t.Errorf("label %q", res.SignerLabel)
	}
	res.Close()
	if _, err = kit.VerifyTarball(b.Tarball, kit.Options{AllowedSigners: other.AllowedSigners}); err == nil {
		t.Error("untrusted key accepted")
	}
	res, err = kit.VerifyTarball(other.Tarball, kit.Options{AllowedSigners: other.AllowedSigners, UserSigners: user})
	if err != nil || res.SignerLabel != kit.OfficialLabel {
		t.Fatalf("official: %v %v", err, res)
	}
	res.Close()
	// only user keys, no official list at all
	res, err = kit.VerifyTarball(b.Tarball, kit.Options{UserSigners: user})
	if err != nil {
		t.Fatal(err)
	}
	res.Close()
	if ok, _ := ut.Remove("mine"); !ok {
		t.Fatal("not removed")
	}
	if user, _ = ut.Bytes(); len(user) != 0 {
		t.Error("key still listed")
	}
}

func TestParsePubKeyFile(t *testing.T) {
	s := kittest.NewSigner(t)
	line := strings.TrimSpace(string(gossh.MarshalAuthorizedKey(s.PublicKey())))
	if _, c, err := kit.ParsePubKeyFile([]byte(line + " me@host\n")); err != nil || c != "me@host" {
		t.Errorf("%v %q", err, c)
	}
	for _, bad := range []string{"", line[:30], "ssh-rsa AAAA x", line + "\n" + line, "from=\"1.2.3.4\" " + line, "-----BEGIN OPENSSH " + "PRIVATE KEY-----"} {
		if _, _, err := kit.ParsePubKeyFile([]byte(bad)); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	if _, _, err := kit.ParsePubKeyFile(make([]byte, kit.MaxPubKeyFile+1)); err == nil {
		t.Error("oversized accepted")
	}
}

func TestSourceFile(t *testing.T) {
	d := t.TempDir()
	if s, err := kit.ReadSource(d); s != "" || err != nil {
		t.Fatal(s, err)
	}
	if err := kit.WriteSource(d, "a/b"); err != nil {
		t.Fatal(err)
	}
	if s, _ := kit.ReadSource(d); s != "a/b" {
		t.Error(s)
	}
	if err := kit.WriteSource(d, "../x"); err == nil {
		t.Error("bad source written")
	}
	// a hand-edited file with a bad name is never used
	_ = os.WriteFile(filepath.Join(d, kit.SourceFile), []byte("https://evil/x\n"), 0o600)
	if _, err := kit.ReadSource(d); err == nil {
		t.Error("bad file accepted")
	}
	_ = kit.WriteSource(d, "")
	if s, err := kit.ReadSource(d); s != "" || err != nil {
		t.Error(s, err)
	}
}
