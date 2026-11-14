package cli_test

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"

	gossh "golang.org/x/crypto/ssh"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// A key already pinned for the new address that differs from the one presented there is a changed key
// at one address: the chooser must not offer a re-pin (it would double the pin and open the door).
func TestChooserNeverRepinsOverADifferentPinnedKey(t *testing.T) {
	e := newEnv(t)
	e.rememberCamera("salon", "192.0.2.21")
	e.fakeLAN("192.0.2.99")
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	other, err := gossh.NewPublicKey(pub)
	if err != nil {
		t.Fatal(err)
	}
	kh := filepath.Join(e.state, "known_hosts")
	if err := camssh.PinHostKey(kh, "192.0.2.99", other); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(kh)
	r := e.noIP("5\n3\ny\n1\ny\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	notContains(t, r.err, "Same camera, new address")
	contains(t, r.out+r.err, "already has a DIFFERENT SSH host key pinned")
	after, _ := os.ReadFile(kh)
	if string(before) != string(after) {
		t.Errorf("known_hosts changed:\n%s", after)
	}
}

// A backup made over SSH from an Akam camera (kind=akam) holds the Akam APP: it never satisfies the
// restore of the stock firmware, even when SHA256SUMS verifies, and nothing is sent.
func TestRestoreRefusesAnSSHBackup(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	bk := w.backupDir(t, true)
	if err := os.WriteFile(filepath.Join(bk, "camera_info.txt"), []byte("camera: 127.0.0.1\nkind=akam\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	w.writeState("camera_kind=akam\n")
	r := w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "kind=akam", "not the original firmware", "Nothing was written.")
	notContains(t, r.err, "WRITES THE CAMERA FLASH")
	w.unchanged(t)
}

// A backup without SHA256SUMS, or whose SHA256SUMS does not list the APP, never passes a gate.
func TestRestoreRefusesUnverifiableBackups(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	bk := w.backupDir(t, false)
	w.writeState("camera_kind=akam\n")
	r := w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "has no mtdblock7.bin or SHA256SUMS")
	w.unchanged(t)
	sums := sha256Of(t, filepath.Join(bk, "mtdblock0.bin")) + "  mtdblock0.bin\n"
	if err := os.WriteFile(filepath.Join(bk, "SHA256SUMS"), []byte(sums), 0o600); err != nil {
		t.Fatal(err)
	}
	r = w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "does not list mtdblock7.bin")
	w.unchanged(t)
}

// The direct commands refuse an APP-sized file that is not a squashfs, before any byte is sent.
func TestDirectCommandsRefuseANonSquashfsImage(t *testing.T) {
	junk := bytes.Repeat([]byte("not a squashfs "), 5242880/15+1)[:5242880]
	w := newFlashWorld(t, testfake.FlashOptions{})
	if err := os.WriteFile(w.img, junk, 0o600); err != nil {
		t.Fatal(err)
	}
	r := w.update("y\ny\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this file is not a valid squashfs image", "do not flash it")
	notContains(t, r.err, "WRITES THE CAMERA FLASH")
	w.unchanged(t)

	bk := w.backupDir(t, true)
	if err := os.WriteFile(filepath.Join(bk, "mtdblock7.bin"), junk, 0o600); err != nil {
		t.Fatal(err)
	}
	sum := sha256Of(t, filepath.Join(bk, "mtdblock7.bin"))
	if err := os.WriteFile(filepath.Join(bk, "SHA256SUMS"), []byte(sum+"  mtdblock7.bin\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	w.writeState("camera_kind=akam\n")
	r = w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this file is not a valid squashfs image")
	notContains(t, r.err, "WRITES THE CAMERA FLASH")
	w.unchanged(t)
}

// `akam update` re-hashes a recorded backup before it sends anything.
func TestUpdateCommandNeedsAVerifiedBackup(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.writeState("camera_kind=akam\n")
	r := w.run("y\n", "update", "--image", w.img)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no verified backup: no backup of this camera is recorded.")
	w.unchanged(t)
	bk := backupFixture(t)
	if err := os.WriteFile(filepath.Join(bk, "mtdblock3.bin"), []byte("rotten"), 0o600); err != nil {
		t.Fatal(err)
	}
	w.writeState("camera_kind=akam\nssh_backup_dir=" + bk + "\n")
	r = w.run("y\n", "update", "--image", w.img)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "SHA256SUMS does not match the files in "+bk)
	w.unchanged(t)
}

// The --image import is trusted only when the T11.12 reader accepts it: the right size is not enough.
func TestImageImportIsStructurallyChecked(t *testing.T) {
	good := validImage(t)
	gz, err := os.ReadFile(filepath.Join("..", "squashfs", "testdata", "tiny_gzip_4k.sqfs"))
	if err != nil {
		t.Fatal(err)
	}
	gz = append(gz, make([]byte, 5242880-len(gz))...)
	corrupt := append([]byte(nil), good...)
	for i := 100; i < 140; i++ {
		corrupt[i] ^= 0xff
	}
	for _, tc := range []struct {
		name string
		data []byte
		want string
	}{
		{"zeros", make([]byte, 5242880), "not a valid squashfs image"},
		{"text", []byte(strings.Repeat("akam image block 0123456789abcdef", 5242880/32+1)[:5242880]), "not a valid squashfs image"},
		{"gzip", gz, "compressed with gzip"},
		{"corrupt", corrupt, "do not flash it"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
			bad := filepath.Join(t.TempDir(), "bad.squashfs")
			if err := os.WriteFile(bad, tc.data, 0o600); err != nil {
				t.Fatal(err)
			}
			st, _ := os.ReadFile(filepath.Join(fw.state, "state"))
			var keep []string
			for _, l := range strings.Split(string(st), "\n") {
				if strings.HasPrefix(l, "step_image_done") || strings.HasPrefix(l, "step_prepare_flash_sd_done") || strings.HasPrefix(l, "image_") || strings.HasPrefix(l, "flash_sd_image_sha") {
					continue
				}
				keep = append(keep, l)
			}
			fw.writeState(strings.Join(keep, "\n"))
			r := fw.run("1\n10\n", "--camera", "default", "--image", bad)
			contains(t, r.out+r.err, tc.want)
			if fw.readState("step_image_done") != "" || fw.readState("image_path") != "" {
				t.Error("an invalid image was recorded")
			}
		})
	}
}

// A folder name with control characters never reaches the state file.
func TestBackupRootRefusesControlCharacters(t *testing.T) {
	e := newEnv(t)
	akamState(e, "")
	bad := filepath.Join(t.TempDir(), "x\x01y")
	os.MkdirAll(bad, 0o700)
	r := e.run("4\n4\n2\n"+bad+"\n3\n5\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "That is not a usable path")
	if e.readState("backup_root") != "" {
		t.Errorf("root kept: %q", e.readState("backup_root"))
	}
}
