package cli_test

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

func hex256(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// appfsDirs writes an image directory and a six-partition backup directory the way appfs-build and the backup
// do: a tiny valid squashfs padded to the partition, its sha256 and appfs_sign, and the backup SHA256SUMS.
func appfsDirs(t *testing.T) (imageDir, backupDir string) {
	t.Helper()
	root := squashfs.NewDir("", 0o755).Add(squashfs.NewFile("a", 0o644, []byte("hello")))
	sq, _, err := squashfs.Build(root, squashfs.Options{Time: 1700000000, AllTime: true})
	if err != nil {
		t.Fatal(err)
	}
	img := make([]byte, appfs.PartitionSize)
	copy(img, sq)
	sign, err := appfs.Sign(img)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	imageDir = filepath.Join(dir, "image")
	backupDir = filepath.Join(dir, "backup")
	for _, d := range []string{imageDir, backupDir} {
		if err := os.MkdirAll(d, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	w := func(p string, b []byte) {
		if err := os.WriteFile(p, b, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	w(filepath.Join(imageDir, appfs.ImageName), img)
	w(filepath.Join(imageDir, appfs.ImageSumName), []byte(hex256(img)+"  "+appfs.ImageName+"\n"))
	w(filepath.Join(imageDir, appfs.SignName), []byte(sign+"\n"))
	var sums strings.Builder
	for i := 0; i < 6; i++ {
		b := make([]byte, 4096)
		b[0] = byte(i)
		if i == 3 {
			stock := make([]byte, appfs.PartitionSize)
			copy(stock, sq)
			stock[100] = 7 // a "stock" appfs that is not the Akam image
			b = stock
		}
		name := fmt.Sprintf("mtd%d.bin", i)
		w(filepath.Join(backupDir, name), b)
		fmt.Fprintf(&sums, "%s  %s\n", hex256(b), name)
	}
	w(filepath.Join(backupDir, "SHA256SUMS"), []byte(sums.String()))
	return imageDir, backupDir
}

// The appfs installer is chosen by the camera's kind: a 1080p (also the type of a profile that has none) is refused.
func TestAppfsCommandsAreRefusedOnAnotherKind(t *testing.T) {
	for _, cmd := range []string{"appfs-build", "appfs-flash"} {
		for name, state := range map[string]string{"1080p": "camera_model=" + lsc1080 + "\n", "no type": "camera_ip=192.0.2.10\n"} {
			t.Run(cmd+" "+name, func(t *testing.T) {
				e := newEnv(t)
				e.writeState(state)
				r := e.run("", cmd, "--kit", "k", "--backup-dir", "b", "--image-dir", "i")
				expectRC(t, r, 1)
				contains(t, r.out+r.err, "card-less appfs installer", "is not served by it", "nothing was changed")
			})
		}
	}
}

// ... and the 1080p flows stay refused on a 2K (the gate of the other kind is unchanged).
func TestAppfsCommandsNeedTheirInputs(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	for _, args := range [][]string{{"appfs-build"}, {"appfs-build", "--kit", "k"}, {"appfs-flash"}, {"appfs-flash", "--image-dir", "i"},
		{"appfs-flash", "--image-dir", "i", "--backup-dir", "b", "--hot"}} {
		r := e.run("", args...)
		if r.rc == 0 {
			t.Errorf("%v succeeded", args)
		}
		contains(t, r.out+r.err, "akam: ")
	}
}

func TestAppfsBuildRefusesAnUnusableKitAndBackup(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	_, bdir := appfsDirs(t)
	r := e.run("", "appfs-build", "--kit", t.TempDir(), "--backup-dir", bdir, "--out", filepath.Join(t.TempDir(), "o"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "appfs kit is not usable")
	r = e.run("", "--dry-run", "--model", rotModel, "appfs-build", "--kit", "k", "--backup-dir", "b")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD read the kit")
}

func TestAppfsFlashDryRunNeverTouchesTheCamera(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	idir, bdir := appfsDirs(t)
	work := filepath.Join(t.TempDir(), "work")
	// default = dry run: local checks and the printed plan, no connection (192.0.2.10 does not answer)
	r := e.run("", "appfs-flash", "--image-dir", idir, "--backup-dir", bdir, "--work-dir", work)
	expectRC(t, r, 0)
	contains(t, r.out, "[dry-run]", "dry run finished", "add --execute")
	notContains(t, r.out, "LIVE run")
	if _, err := os.Stat(work); err == nil {
		t.Error("a dry run created the work dir")
	}
	// --execute inside a rehearsal of the program is still a dry run
	r = e.run("", "--dry-run", "--model", rotModel, "appfs-flash", "--execute", "--image-dir", idir, "--backup-dir", bdir, "--work-dir", work)
	expectRC(t, r, 0)
	notContains(t, r.out, "LIVE run")
	// a tampered backup is refused before anything else, also in a dry run
	if err := os.WriteFile(filepath.Join(bdir, "mtd0.bin"), []byte("tampered"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = e.run("", "appfs-flash", "--image-dir", idir, "--backup-dir", bdir, "--work-dir", work)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "does not match")
}

// A live run needs a terminal: without one nothing is asked, nothing is sent.
func TestAppfsFlashExecuteNeedsATerminalAndConfirmations(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	idir, bdir := appfsDirs(t)
	work := filepath.Join(t.TempDir(), "work")
	r := e.run("", "appfs-flash", "--execute", "--image-dir", idir, "--backup-dir", bdir, "--work-dir", work)
	if r.rc == 0 {
		t.Fatalf("a live flash ran without a terminal:\n%s%s", r.out, r.err)
	}
	notContains(t, r.out, "LIVE run", "WRITING mtd3")
	if _, err := os.Stat(work); err == nil {
		t.Error("the work dir exists: the run went past the terminal gate")
	}
	// a typed answer on stdin is not a terminal: still refused
	r = e.run("yes\nyes\n", "appfs-flash", "--execute", "--image-dir", idir, "--backup-dir", bdir, "--work-dir", work)
	if r.rc == 0 {
		t.Fatal("piped answers confirmed a live flash")
	}
}

// build2KKit makes a signed tarball of the 2K kit with the right file set (stand-in contents: the real busybox pin
// is not satisfied, so a build from it stops inside the builder, after the kit was verified and loaded).
func build2KKit(t *testing.T, sp kittest.Spec) kittest.Built {
	t.Helper()
	files := map[string][]byte{"bin/akamd": nil, "bin/dropbearmulti": nil, "bin/flash_tool": nil, "patches/patch_service_sh.sh": nil,
		"scripts/flash/akam_reflash_ssh.sh": nil, "scripts/flash/flash_guard.sh": nil, "scripts/flash/preflight_check.sh": nil,
		"scripts/hwcheck/akam_hwcheck.sh": nil}
	for _, p := range appfs.RequiredKitPaths() {
		if p != "MANIFEST.txt" && p != "SHA256SUMS" {
			files[p] = []byte("stand-in " + p + "\n")
		}
	}
	sp.Model, sp.Target, sp.NoLink = rotModel, "arm-linux", true
	sp.Files = files
	sp.ManifestExtra = "kind=cardless-appfs\n"
	return kittest.Build(t, t.TempDir(), sp)
}

// B2: appfs-build takes the signed release tarball (--kit FILE) or downloads it: the signature is verified, as for
// prepare-2k-sd and update, before the kit is read.
func TestAppfsBuildVerifiesTheSignedKit(t *testing.T) {
	b := build2KKit(t, kittest.Spec{})
	_, bdir := appfsDirs(t)
	tar := filepath.Join(b.Dir, "akam-kit-"+rotModel+"-1.0.0.tar.xz")

	// the signed tarball given as a file: verified, loaded, and the build starts (the stand-in busybox stops it)
	e := getKitEnv(t, nil, &b)
	e.writeState("camera_model=" + rotModel + "\n")
	r := e.run("", "appfs-build", "--kit", tar, "--backup-dir", bdir, "--out", filepath.Join(t.TempDir(), "o"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "signature valid", "Kit verified (version 1.0.0", "the image was not built")
	notContains(t, r.out+r.err, "appfs kit is not usable")

	// downloaded: no --kit, the three files are fetched over HTTPS and verified the same way
	ks := newKitServer(t, b, false)
	e = getKitEnv(t, ks, &b)
	e.writeState("camera_model=" + rotModel + "\n")
	r = e.run("", "appfs-build", "--version", "1.0.0", "--backup-dir", bdir, "--out", filepath.Join(t.TempDir(), "o"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "signature valid", "Kit verified (version 1.0.0", "the image was not built")
	if len(ks.hits) != 3 {
		t.Errorf("requests: %v", ks.hits)
	}

	// a kit signed by a key the program does not trust is refused before it is read
	other := build2KKit(t, kittest.Spec{}) // another test key: this program trusts only the key it is given
	e = getKitEnv(t, nil, &other)
	e.writeState("camera_model=" + rotModel + "\n")
	r = e.run("", "appfs-build", "--kit", tar, "--backup-dir", bdir, "--out", filepath.Join(t.TempDir(), "o"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the kit failed verification")
	notContains(t, r.out+r.err, "the image was not built")

	// an unsigned tarball is refused unless --allow-unsigned (and asks no question without a terminal)
	u := build2KKit(t, kittest.Spec{Unsigned: true})
	e = getKitEnv(t, nil, &u)
	e.writeState("camera_model=" + rotModel + "\n")
	r = e.run("", "appfs-build", "--kit", filepath.Join(u.Dir, "akam-kit-"+rotModel+"-1.0.0.tar.xz"), "--backup-dir", bdir)
	if r.rc == 0 {
		t.Fatalf("an unsigned kit was accepted:\n%s%s", r.out, r.err)
	}
	notContains(t, r.out+r.err, "the image was not built")
}

func TestAppfsBuildDryRunWithoutKitSaysItDownloads(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "--dry-run", "--model", rotModel, "appfs-build", "--backup-dir", "b")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD download the signed 2K kit")
}
