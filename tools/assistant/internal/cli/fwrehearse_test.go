package cli_test

import (
	"crypto/sha256"
	"encoding/hex"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

// treeHash is a hash of every file, folder and link under dir (names, modes and contents), except the files
// a rehearsal is allowed to write: its own state (state.dry), the log and the lock.
func treeHash(t *testing.T, dir string) string {
	t.Helper()
	lines := treeLines(t, dir)
	h := sha256.Sum256([]byte(strings.Join(lines, "\n")))
	return hex.EncodeToString(h[:]) + " (" + itoa(len(lines)) + " entries)\n" + strings.Join(lines, "\n")
}

func treeLines(t *testing.T, dir string) []string {
	t.Helper()
	var lines []string
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		switch filepath.Base(p) {
		case "state.dry", "akam.log", ".akam.lock":
			return nil
		}
		info, _ := d.Info()
		line := rel + " " + info.Mode().String()
		if d.Type().IsRegular() {
			b, _ := os.ReadFile(p)
			h := sha256.Sum256(b)
			line += " " + hex.EncodeToString(h[:])
		}
		lines = append(lines, line)
		return nil
	})
	sort.Strings(lines)
	return lines
}

// A rehearsal never installs a kit: --dry-run get-kit with a signed or an unsigned kit, as the command, as a step
// and in the first install, leaves the kit folder, the downloads and the state byte for byte as they were.
func TestRehearsalsNeverInstallAKit(t *testing.T) {
	signed := kittest.Build(t, t.TempDir(), kittest.Spec{})
	unsigned := kittest.Build(t, t.TempDir(), kittest.Spec{Version: "1.1.0", Unsigned: true})
	for name, c := range map[string]struct {
		kit  kittest.Built
		args []string
		in   string
	}{
		"signed get-kit":   {signed, []string{"get-kit"}, ""},
		"unsigned get-kit": {unsigned, []string{"get-kit"}, ""},
		"signed step":      {signed, []string{"step", "get-kit"}, ""},
		"unsigned step":    {unsigned, []string{"step", "get-kit"}, ""},
		"install":          {signed, nil, "1\n10\n"},
	} {
		w := newFwWorld(t, relList(), "")
		tarball := recordKit(t, w)
		f := filepath.Join(t.TempDir(), "allowed_signers")
		_ = os.WriteFile(f, append(append([]byte{}, w.api.kb.AllowedSigners...), c.kit.AllowedSigners...), 0o600)
		t.Setenv("AKAM_ALLOWED_SIGNERS", f)
		// the rehearsal's own state knows the doctor step (it needs it)
		_ = os.WriteFile(filepath.Join(w.state, "state.dry"), []byte(readAll(t, filepath.Join(w.state, "state"))), 0o600)
		before := treeHash(t, w.state)
		kitDir := filepath.Join(w.state, "kit")
		mark, _ := os.ReadFile(filepath.Join(kitDir, "MARK"))
		args := append([]string{"--camera", "default", "--dry-run"}, c.args...)
		args = append(args, "--kit", c.kit.Tarball, "--allow-unsigned")
		_ = w.run(c.in, args...)
		if after := treeHash(t, w.state); after != before {
			t.Errorf("%s: the rehearsal changed the state folder\n%s\n%s", name, before, after)
		}
		if b, _ := os.ReadFile(filepath.Join(kitDir, "MARK")); string(b) != string(mark) {
			t.Errorf("%s: the kit folder was replaced", name)
		}
		for _, d := range []string{"kit.new", "kit.dry", "kit.old", "kit.swap"} {
			if _, err := os.Stat(filepath.Join(w.state, d)); err == nil {
				t.Errorf("%s: %s was created", name, d)
			}
		}
		if got, _ := hashOf(tarball); got != w.api.kb.TarSHA {
			t.Errorf("%s: the recorded tarball changed", name)
		}
		w.unchanged(t)
	}
}

// No rehearsal writes outside the dry state: every read-only command and flow, run with --dry-run over a state
// that holds a recorded kit, leaves the whole state folder as it was.
func TestRehearsalsWriteNothingOutsideTheDryState(t *testing.T) {
	for _, args := range [][]string{
		{"update", "--version", "1.0.0"}, {"update", "--stock"}, {"releases"}, {"status"}, {"verify"}, {"restore"},
		{"flash"}, {"image"}, {"backup"}, {"prepare-flash-sd"}, {"prepare-backup-sd"},
	} {
		w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
		recordKit(t, w)
		before := treeHash(t, w.state)
		_ = w.run("", append([]string{"--camera", "default", "--dry-run"}, args...)...)
		if after := treeHash(t, w.state); after != before {
			t.Errorf("--dry-run %v changed the state folder\n%s\n%s", args, before, after)
		}
		w.unchanged(t)
	}
}

// The camera that started a swap decides it: a marker, a kit.old and a kit are judged from the marker and that
// camera's state, whatever camera is selected when the program starts.
func TestKitSwapRecoveryIsJudgedFromTheMarker(t *testing.T) {
	for _, recorded := range []bool{true, false} {
		w := newFwWorld(t, relList(), "")
		kd := filepath.Join(w.state, "kit")
		_ = os.MkdirAll(kd, 0o700)
		_ = os.WriteFile(filepath.Join(kd, "MARK"), []byte("new"), 0o600)
		_ = os.MkdirAll(kd+".old", 0o700)
		_ = os.WriteFile(filepath.Join(kd+".old", "MARK"), []byte("old"), 0o600)
		_ = os.WriteFile(kd+".swap", []byte("camera=salon\nnew_sha=NEWSHA\nnew_version=2.0.0\nold_sha=OLDSHA\nold_version=1.0.0\n"), 0o600)
		sha := "OLDSHA"
		if recorded {
			sha = "NEWSHA"
		}
		cdir := filepath.Join(w.state, "cameras", "salon")
		_ = os.MkdirAll(cdir, 0o700)
		_ = os.WriteFile(filepath.Join(cdir, "state"), []byte("kit_sha="+sha+"\nkit_version=2.0.0\n"), 0o600)
		// another camera (default) is selected, at yet another kit version
		w.writeState(readAll(t, filepath.Join(w.state, "state")) + "kit_sha=OTHER\nkit_version=1.5.0\n")
		r := w.run("", "--camera", "default", "status")
		expectRC(t, r, 0)
		want := "old"
		if recorded {
			want = "new"
		}
		if b, _ := os.ReadFile(filepath.Join(kd, "MARK")); string(b) != want {
			t.Errorf("recorded=%v: the kit is %q, want %q", recorded, b, want)
		}
		for _, d := range []string{".old", ".swap", ".new"} {
			if _, err := os.Stat(kd + d); err == nil {
				t.Errorf("recorded=%v: kit%s is still there", recorded, d)
			}
		}
		contains(t, r.out, "an interrupted kit swap was")
	}
}

// An unfinished download of an earlier run is swept at start.
func TestStartupSweepsStagingFolders(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	st := filepath.Join(w.state, "downloads", "1.0.0", ".staging")
	_ = os.MkdirAll(st, 0o700)
	_ = os.WriteFile(filepath.Join(st, "half"), []byte("x"), 0o600)
	_ = w.run("", "--camera", "default", "status")
	if _, err := os.Stat(st); err == nil {
		t.Error(".staging left behind")
	}
}

// The write succeeded but the kit could not be installed afterwards: the write is recorded from the staged
// kit, the power-cycle instructions follow, and no false "nothing was changed" or "do not power-cycle" appears.
func TestKitInstallFailureAfterAGoodWriteIsOwnMessage(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	// a non-empty folder where the swap marker goes: the commit fails after the write
	swap := filepath.Join(w.state, "kit.swap")
	_ = os.MkdirAll(filepath.Join(swap, "x"), 0o700)
	_ = os.WriteFile(filepath.Join(swap, "x", "f"), []byte("x"), 0o600)
	r := w.update("1\ny\n\n10\n")
	expectRC(t, r, 0)
	out := r.out + r.err
	contains(t, out, "the camera was written and verified, but the kit that went with it is NOT installed on this computer",
		"Flash done and verified.", "Image md5 written and read back: "+w.imgMD5)
	notContains(t, out, "DO NOT power-cycle the camera; the worker keeps running", "nothing was changed")
	if string(w.cam.Mtd7()) != string(w.imgData) {
		t.Fatal("the image was not written")
	}
	if w.readState("fw_source") != "release:1.0.0" || w.readState("fw_version") != "1.0.0" {
		t.Errorf("source %q version %q", w.readState("fw_source"), w.readState("fw_version"))
	}
}

// The first install asks the imported-image question too, and a No changes nothing.
func TestInstallAsksAboutAnImportedImage(t *testing.T) {
	w := newFwWorld(t, relList(), "image_origin=imported:"+shaOf(validImage(t))+"\n")
	tarball := recordKit(t, w)
	before := w.kitSnapshot(tarball)
	r := w.run("1\nn\n10\n", "--camera", "default", "--version", "1.0.0")
	contains(t, r.out+r.err, "the image on record was imported, not built from Akam 1.0.0", "Keep using that imported image? [y/N]",
		"the imported image on record is not used.")
	if w.kitSnapshot(tarball) != before {
		t.Error("a refused choice changed the recorded kit")
	}
}
