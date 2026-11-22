//go:build unix

package appfs

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// localSh runs the camera-side script in a local sh (the scripts only use POSIX sh, grep, cut, md5sum, cp, mv).
type localSh struct{}

func (localSh) Run(ctx context.Context, cmd string, _ io.Reader) (camssh.Result, error) {
	c := exec.CommandContext(ctx, "sh", "-c", cmd)
	var o, e bytes.Buffer
	c.Stdout, c.Stderr = &o, &e
	err := c.Run()
	st := 0
	if x, ok := err.(*exec.ExitError); ok {
		st, err = x.ExitCode(), nil
	}
	return camssh.Result{Stdout: o.Bytes(), Stderr: e.Bytes(), Status: st}, err
}
func (localSh) Close() {}

const seedWpa = "network={\n\tssid=616263\n\tpsk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n}\n"
const seedKey = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC2qtKAhTkbHvkklqSiZAdGqq98kQiMmRzoOIBlmjPG1 test\n"

func seedRoot(t *testing.T, card bool, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	put := func(p, s string) {
		p = filepath.Join(root, p)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	m := "rootfs / squashfs ro 0 0\n"
	if card {
		m += "/dev/mmcblk0p1 /mnt/sdcard vfat rw 0 0\n"
	}
	put("proc/mounts", m)
	for p, s := range files {
		put(p, s)
	}
	return root
}

func TestSeedCopiesOnceAndKeepsWhatDiffers(t *testing.T) {
	if !haveTools("sh", "md5sum", "cut", "grep") {
		t.Skip("sh, md5sum, cut and grep are needed")
	}
	root := seedRoot(t, true, map[string]string{"mnt/sdcard/akam/wpa.conf": seedWpa, "mnt/sdcard/akam/authorized_keys": seedKey})
	var said []string
	say := func(s string) { said = append(said, s) }
	r, err := seedWith(context.Background(), localSh{}, root, say)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(r.Lines, "|") != "wpa.conf: seeded|authorized_keys: seeded" {
		t.Fatalf("first run: %q", r.Lines)
	}
	for f, want := range map[string]string{"wpa.conf": seedWpa, "authorized_keys": seedKey} {
		p := filepath.Join(root, "config/akam", f)
		b, err := os.ReadFile(p)
		if err != nil || string(b) != want {
			t.Fatalf("%s: %v %q", f, err, b)
		}
		if st, _ := os.Stat(p); st.Mode().Perm() != 0o600 {
			t.Errorf("%s mode %v", f, st.Mode().Perm())
		}
	}
	if st, _ := os.Stat(filepath.Join(root, "config/akam")); st.Mode().Perm() != 0o700 {
		t.Errorf("directory mode %v", st.Mode().Perm())
	}
	if ents, _ := os.ReadDir(filepath.Join(root, "config/akam")); len(ents) != 2 {
		t.Errorf("leftover files in /config/akam: %v", ents)
	}
	// idempotent
	r, err = seedWith(context.Background(), localSh{}, root, say)
	if err != nil || strings.Join(r.Lines, "|") != "wpa.conf: already seeded|authorized_keys: already seeded" {
		t.Fatalf("second run: %v %q", err, r)
	}
	// a file the operator changed is never overwritten
	if err := os.WriteFile(filepath.Join(root, "config/akam/authorized_keys"), []byte(seedKey+seedKey), 0o600); err != nil {
		t.Fatal(err)
	}
	r, err = seedWith(context.Background(), localSh{}, root, say)
	if err != nil || !strings.Contains(strings.Join(r.Lines, "|"), "authorized_keys: kept") {
		t.Fatalf("third run: %v %q", err, r)
	}
	if b, _ := os.ReadFile(filepath.Join(root, "config/akam/authorized_keys")); string(b) != seedKey+seedKey {
		t.Error("a differing file was overwritten")
	}
	// the secrets are in neither what was said nor in the result
	all := strings.Join(said, "\n") + strings.Join(r.Lines, "\n")
	for _, secret := range []string{"0123456789abcdef", "AAAAC3Nza", "616263"} {
		if strings.Contains(all, secret) {
			t.Errorf("a secret reached the output: %s", secret)
		}
	}
}

func TestSeedRefusals(t *testing.T) {
	if !haveTools("sh", "md5sum", "cut", "grep") {
		t.Skip("sh, md5sum, cut and grep are needed")
	}
	good := map[string]string{"mnt/sdcard/akam/wpa.conf": seedWpa, "mnt/sdcard/akam/authorized_keys": seedKey}
	cases := map[string]struct {
		root string
		want string
	}{
		"no card (card-less boot)": {seedRoot(t, false, good), "not running from the first-install card"},
		"no wpa.conf":              {seedRoot(t, true, map[string]string{"mnt/sdcard/akam/authorized_keys": seedKey}), "no usable wpa.conf"},
		"bad key":                  {seedRoot(t, true, map[string]string{"mnt/sdcard/akam/wpa.conf": seedWpa, "mnt/sdcard/akam/authorized_keys": "junk\n"}), "no usable wpa.conf"},
	}
	for name, c := range cases {
		_, err := seedWith(context.Background(), localSh{}, c.root, nil)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v", name, err)
		}
		if _, e := os.Stat(filepath.Join(c.root, "config")); e == nil {
			t.Errorf("%s: /config was touched", name)
		}
	}
}
