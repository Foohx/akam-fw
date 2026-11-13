package ssh_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	gossh "golang.org/x/crypto/ssh"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

func TestKindOfSharedOwnAndCustom(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	for _, c := range []struct{ pub, camera, want string }{
		{filepath.Join(home, ".ssh", "akam.pub"), "salon", camssh.KindShared},
		{filepath.Join(home, ".ssh", "akam"), "salon", camssh.KindShared},
		{filepath.Join(home, ".ssh", "akam-salon.pub"), "salon", camssh.KindOwn},
		{filepath.Join(home, ".ssh", "akam-garage.pub"), "salon", camssh.KindCustom}, // another camera's key is custom here
		{filepath.Join(home, ".ssh", "id_ed25519.pub"), "salon", camssh.KindCustom},
		{"/elsewhere/akam.pub", "salon", camssh.KindCustom},
	} {
		if got := camssh.KindOf(c.pub, c.camera); got != c.want {
			t.Errorf("KindOf(%s, %s) = %s, want %s", c.pub, c.camera, got, c.want)
		}
	}
	if camssh.OwnKeyPath("salon") != filepath.Join(home, ".ssh", "akam-salon") {
		t.Errorf("OwnKeyPath = %s", camssh.OwnKeyPath("salon"))
	}
}

// A key made natively reads back with the library (and, when present, with ssh-keygen), has the mode
// and the comment asked for, and is never written over an existing file.
func TestGenerateKeyFormatModeCommentAndNoOverwrite(t *testing.T) {
	dir := t.TempDir()
	for _, pass := range []string{"", "a long passphrase"} {
		priv := filepath.Join(dir, "k-"+strings.ReplaceAll(pass, " ", "_"))
		var pp []byte
		if pass != "" {
			pp = []byte(pass)
		}
		if err := camssh.GenerateKey(priv, "akam-salon", pp); err != nil {
			t.Fatal(err)
		}
		b, _ := os.ReadFile(priv)
		if !strings.HasPrefix(string(b), "-----BEGIN OPENSSH "+"PRIVATE KEY-----\n") {
			t.Errorf("format: %.40s", b)
		}
		if runtime.GOOS != "windows" {
			if st, _ := os.Stat(priv); st.Mode().Perm() != 0o600 {
				t.Errorf("private mode %v", st.Mode().Perm())
			}
			if st, _ := os.Stat(priv + ".pub"); st.Mode().Perm() != 0o644 {
				t.Errorf("public mode %v", st.Mode().Perm())
			}
		}
		s, closeFn, err := camssh.LoadSigner(priv, func() (string, error) { return pass, nil })
		if err != nil {
			t.Fatalf("LoadSigner(%q): %v", pass, err)
		}
		closeFn()
		pub, err := camssh.ParsePubFile(priv + ".pub")
		if err != nil || gossh.FingerprintSHA256(pub) != gossh.FingerprintSHA256(s.PublicKey()) {
			t.Errorf("the .pub does not match: %v", err)
		}
		line, _ := os.ReadFile(priv + ".pub")
		if !strings.HasPrefix(string(line), "ssh-ed25519 ") || !strings.HasSuffix(string(line), " akam-salon\n") || strings.Count(string(line), "\n") != 1 {
			t.Errorf("public line: %q", line)
		}
		if pass != "" {
			if _, err := gossh.ParsePrivateKey(b); err == nil {
				t.Error("a passphrase key opened without it")
			}
			if _, _, err := camssh.LoadSigner(priv, func() (string, error) { return "wrong", nil }); err == nil {
				t.Error("a wrong passphrase was accepted")
			}
		}
		if kg, err := exec.LookPath("ssh-keygen"); err == nil && pass == "" {
			if out, err := exec.Command(kg, "-y", "-f", priv).CombinedOutput(); err != nil || !strings.HasPrefix(string(out), "ssh-ed25519 ") {
				t.Errorf("ssh-keygen cannot read the key: %v %s", err, out)
			}
		}
		// never over an existing file, either half of the pair
		before, _ := os.ReadFile(priv)
		if err := camssh.GenerateKey(priv, "x", nil); err == nil {
			t.Error("overwrote an existing key")
		}
		if after, _ := os.ReadFile(priv); string(after) != string(before) {
			t.Error("the existing key changed")
		}
	}
	lone := filepath.Join(dir, "lone")
	if err := os.WriteFile(lone+".pub", []byte("ssh-ed25519 AAAA x\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := camssh.GenerateKey(lone, "x", nil); err == nil {
		t.Error("overwrote a lone .pub")
	}
	if _, err := os.Stat(lone); err == nil {
		t.Error("a private key was written next to a foreign .pub")
	}
}
