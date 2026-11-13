package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The kit swap is staged: nothing recorded changes until everything succeeded (round 3).

// recordKit makes the 1.0.0 kit "recorded": its three files in downloads/1.0.0, the kit folder, the kit state.
func recordKit(t *testing.T, w *fwWorld) (tarball string) {
	t.Helper()
	dl := filepath.Join(w.state, "downloads", "1.0.0")
	if err := os.MkdirAll(dl, 0o700); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"akam-kit-1.0.0.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"} {
		b, _ := os.ReadFile(filepath.Join(w.api.kb.Dir, f))
		if err := os.WriteFile(filepath.Join(dl, f), b, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	kd := filepath.Join(w.state, "kit")
	_ = os.MkdirAll(kd, 0o700)
	_ = os.WriteFile(filepath.Join(kd, "MARK"), []byte("recorded kit"), 0o600)
	tarball = filepath.Join(dl, "akam-kit-1.0.0.tar.xz")
	st := strings.ReplaceAll(readAll(t, filepath.Join(w.state, "state")), "mode=source\n", "mode=kit\n")
	w.writeState(st + "kit_path=" + tarball + "\nkit_sha=" + w.api.kb.TarSHA + "\nkit_version=1.0.0\nkit_format=1\nkit_unsigned_ok=0\n")
	return tarball
}

func (w *fwWorld) kitSnapshot(tarball string) string {
	sha, _ := hashOf(tarball)
	b, _ := os.ReadFile(filepath.Join(w.state, "kit", "MARK"))
	_, stagingErr := os.Stat(filepath.Join(filepath.Dir(tarball), ".staging"))
	_, newErr := os.Stat(filepath.Join(w.state, "kit.new"))
	return strings.Join([]string{sha, string(b), w.readState("kit_sha"), w.readState("kit_path"), w.readState("mode"), w.readState("step_get_kit_done"),
		w.readState("step_image_done"), w.readState("image_kit_sha"), w.readState("kit_version"),
		map[bool]string{true: "staging-left", false: "clean"}[stagingErr == nil], map[bool]string{true: "kit.new-left", false: "clean"}[newErr == nil]}, "|")
}

// A failed download or a failed verification of the same version never touches the recorded kit, its
// downloaded files or the image step.
func TestFailedDownloadOrVerificationLeavesTheRecordedKitAlone(t *testing.T) {
	for name, damage := range map[string]func(w *fwWorld){
		"download fails": func(w *fwWorld) { _ = os.Remove(filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz")) },
		"sums fail":      func(w *fwWorld) { _ = os.WriteFile(filepath.Join(w.api.kb.Dir, "SHA256SUMS"), []byte("junk\n"), 0o600) },
		"tarball is not the signed one": func(w *fwWorld) {
			_ = os.WriteFile(filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz"), []byte("tampered"), 0o600)
		},
	} {
		w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
		tarball := recordKit(t, w)
		before := w.kitSnapshot(tarball)
		damage(w)
		r := w.update("2\ny\n10\n") // rows: the image already built (valid for the recorded kit), the release
		expectRC(t, r, 0)
		contains(t, r.out+r.err, "nothing was changed")
		if after := w.kitSnapshot(tarball); after != before {
			t.Errorf("%s: the recorded kit changed:\n%s\n%s", name, before, after)
		}
		w.unchanged(t)
		if t.Failed() {
			t.Fatal(name)
		}
	}
	// the plain command too
	w := newFwWorld(t, relList(), "")
	tarball := recordKit(t, w)
	before := w.kitSnapshot(tarball)
	_ = os.WriteFile(filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz"), []byte("tampered"), 0o600)
	r := w.run("", "--camera", "default", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 1)
	if after := w.kitSnapshot(tarball); after != before {
		t.Errorf("get-kit: the recorded kit changed:\n%s\n%s", before, after)
	}
}

// An interruption at the downgrade question (the input ends there) leaves everything as it was.
func TestInterruptedAtTheDowngradePromptChangesNothing(t *testing.T) {
	w := newFwWorld(t, relList(), "fw_version=2.0.0\n")
	tarball := recordKit(t, w)
	before := w.kitSnapshot(tarball)
	kitFile := filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz")
	r := w.update("3\n" + kitFile + "\n") // rows: built image, downloaded kit, a kit file; no answer: the input ends at "Go back to Akam 1.0.0?"
	contains(t, r.out+r.err, "Go back to Akam 1.0.0? [y/N]", "downgrade not confirmed.")
	if after := w.kitSnapshot(tarball); after != before {
		t.Errorf("changed:\n%s\n%s", before, after)
	}
	w.unchanged(t)
}

// A kit that is installed (the write succeeded) replaces the recorded one, and the previous folder is gone.
func TestKitIsInstalledOnlyAfterTheWrite(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	kd := filepath.Join(w.state, "kit")
	_ = os.MkdirAll(kd, 0o700)
	_ = os.WriteFile(filepath.Join(kd, "MARK"), []byte("previous"), 0o600)
	// No at the red box: the previous kit folder and the kit state stay
	r := w.update("1\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "not confirmed.")
	if b, _ := os.ReadFile(filepath.Join(kd, "MARK")); string(b) != "previous" || w.readState("mode") != "source" {
		t.Errorf("the kit was installed although nothing was written (mode %q)", w.readState("mode"))
	}
	if _, err := os.Stat(kd + ".new"); err == nil {
		t.Error("kit.new left behind")
	}
	// Yes: the new kit is installed after the write, kit.old is gone
	r = w.update("1\ny\n\n10\n")
	expectRC(t, r, 0)
	if w.readState("mode") != "kit" || w.readState("kit_version") != "1.0.0" || w.readState("fw_source") != "release:1.0.0" {
		t.Errorf("mode %q version %q source %q", w.readState("mode"), w.readState("kit_version"), w.readState("fw_source"))
	}
	if _, err := os.Stat(filepath.Join(kd, "MARK")); err == nil {
		t.Error("the previous kit folder is still the kit")
	}
	if _, err := os.Stat(filepath.Join(kd, "MANIFEST.txt")); err != nil {
		t.Errorf("the new kit is not installed: %v", err)
	}
	for _, d := range []string{kd + ".old", kd + ".new", kd + ".keep"} {
		if _, err := os.Stat(d); err == nil {
			t.Errorf("%s left behind", d)
		}
	}
	r = w.run("", "--camera", "default", "status")
	contains(t, r.out, "Firmware source: release:1.0.0  (recorded by akam on 20")
}

// Start-up recovery: a leftover kit.new goes, a kit.old (or kit.keep) comes back when the kit is absent, and
// is never deleted then.
func TestStartupRecoversAnInterruptedKitSwap(t *testing.T) {
	for _, leftover := range []string{"kit.old", "kit.keep"} {
		w := newFwWorld(t, relList(), "")
		_ = os.MkdirAll(filepath.Join(w.state, leftover), 0o700)
		_ = os.WriteFile(filepath.Join(w.state, leftover, "MARK"), []byte("old kit"), 0o600)
		_ = os.MkdirAll(filepath.Join(w.state, "kit.new"), 0o700)
		_ = os.WriteFile(filepath.Join(w.state, "kit.new", "HALF"), []byte("half"), 0o600)
		r := w.run("", "--camera", "default", "status")
		expectRC(t, r, 0)
		if b, err := os.ReadFile(filepath.Join(w.state, "kit", "MARK")); err != nil || string(b) != "old kit" {
			t.Errorf("%s: the old kit did not come back: %v", leftover, err)
		}
		for _, d := range []string{leftover, "kit.new"} {
			if _, err := os.Stat(filepath.Join(w.state, d)); err == nil {
				t.Errorf("%s: %s is still there", leftover, d)
			}
		}
	}
	// both there and nothing contradicts the state: the old one is only a leftover
	w := newFwWorld(t, relList(), "")
	_ = os.MkdirAll(filepath.Join(w.state, "kit"), 0o700)
	_ = os.WriteFile(filepath.Join(w.state, "kit", "MARK"), []byte("current"), 0o600)
	_ = os.MkdirAll(filepath.Join(w.state, "kit.old"), 0o700)
	r := w.run("", "--camera", "default", "status")
	expectRC(t, r, 0)
	if b, _ := os.ReadFile(filepath.Join(w.state, "kit", "MARK")); string(b) != "current" {
		t.Error("the current kit was replaced")
	}
	if _, err := os.Stat(filepath.Join(w.state, "kit.old")); err == nil {
		t.Error("kit.old not cleared")
	}
}

// A kit.old that cannot be removed stops the install before anything is touched.
func TestUnremovableKitOldStopsTheInstall(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	kd := filepath.Join(w.state, "kit")
	_ = os.MkdirAll(kd, 0o700)
	_ = os.WriteFile(filepath.Join(kd, "MARK"), []byte("current"), 0o600)
	sub := filepath.Join(w.state, "kit.old", "sub")
	_ = os.MkdirAll(sub, 0o700)
	_ = os.WriteFile(filepath.Join(sub, "f"), []byte("x"), 0o600)
	if err := os.Chmod(sub, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sub, 0o700) })
	if f, err := os.OpenFile(filepath.Join(sub, "probe"), os.O_CREATE|os.O_WRONLY, 0o600); err == nil {
		f.Close()
		t.Skip("directory permissions are not enforced here (running as root?)")
	}
	before := w.readState("kit_sha") + w.readState("mode")
	kitFile := filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz")
	r := w.run("", "--camera", "default", "get-kit", "--kit", kitFile)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "cannot clear", "nothing was changed")
	if b, _ := os.ReadFile(filepath.Join(kd, "MARK")); string(b) != "current" || w.readState("kit_sha")+w.readState("mode") != before {
		t.Error("the kit or its state changed")
	}
	if _, err := os.Stat(kd + ".new"); err == nil {
		t.Error("kit.new left behind")
	}
}

// --version or --kit on the first install is never ignored: the recorded kit (here a local unsigned one) is
// replaced by the verified release; a failed download keeps the recorded kit and its done mark.
func TestInstallFlagsApplyTheSameInvalidation(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("mode=kit\nkit_path=" + a.kb.Tarball + "\nkit_sha=" + a.kb.TarSHA + "\nkit_version=1.0.0\nkit_format=1\nkit_unsigned_ok=1\ncamera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt)
	r := e.run("1\n", "--camera", "default", "--version", "1.0.0")
	contains(t, r.out+r.err, "ok: signature valid")
	if a.kitHits() != 3 || e.readState("kit_unsigned_ok") != "0" {
		t.Errorf("hits %v unsigned %q", a.hits, e.readState("kit_unsigned_ok"))
	}
	// a download that fails keeps what was recorded (the done mark included)
	a2 := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e2 := newEnv(t)
	useAPI(t, e2, a2)
	kept := filepath.Join(t.TempDir(), "akam-kit-1.0.0.tar.xz")
	kb, _ := os.ReadFile(a2.kb.Tarball)
	_ = os.WriteFile(kept, kb, 0o600)
	_ = os.Remove(filepath.Join(a2.kb.Dir, "akam-kit-1.0.0.tar.xz"))
	e2.writeState("mode=kit\nkit_path=" + kept + "\nkit_sha=" + a2.kb.TarSHA + "\nkit_version=1.0.0\nkit_format=1\nkit_unsigned_ok=1\ncamera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt)
	r = e2.run("1\n", "--camera", "default", "--version", "1.0.0")
	contains(t, r.out+r.err, "Nothing was changed: the previous kit stays in use.")
	if e2.readState("step_get_kit_done") == "" || e2.readState("kit_unsigned_ok") != "1" {
		t.Error("the recorded kit was touched by a failed download")
	}
}

// An imported image on record is not passed off as the image of a release.
func TestImportedImageOnRecordIsSaidAndAsked(t *testing.T) {
	w := newFwWorldBuilt(t, relList(relJSON("v1.0.0", false, false)), "image_origin=imported:"+shaOf(validImage(t))+"\n")
	r := w.update("9\n10\n")
	contains(t, r.out+r.err, "1) The image you imported earlier (")
	notContains(t, r.out+r.err, "already built from your backup")
	// a release row with an imported image on record asks (default No) and changes nothing
	w = newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "image_origin=imported:"+shaOf(validImage(t))+"\n")
	tarball := recordKit(t, w)
	before := w.kitSnapshot(tarball)
	r = w.update("2\n\n10\n")
	contains(t, r.out+r.err, "the image on record was imported, not built from Akam 1.0.0", "Write that imported image anyway? [y/N]",
		"the imported image on record is not used.", "choose 'The image you imported earlier'")
	if w.kitSnapshot(tarball) != before {
		t.Error("a refused choice changed the recorded kit")
	}
	w.unchanged(t)
}

// `update --image F --version X`: a No at the red box leaves the previous kit; a yes installs the new one.
func TestImageWithVersionStagesTheKitUntilTheWrite(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	kd := filepath.Join(w.state, "kit")
	_ = os.MkdirAll(kd, 0o700)
	_ = os.WriteFile(filepath.Join(kd, "MARK"), []byte("previous"), 0o600)
	r := w.run("n\n", "--camera", "default", "update", "--image", w.img, "--version", "1.0.0")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "Kit verified", "not confirmed.")
	if b, _ := os.ReadFile(filepath.Join(kd, "MARK")); string(b) != "previous" || w.readState("mode") != "source" {
		t.Error("the kit changed although nothing was written")
	}
	r = w.run("y\n", "--camera", "default", "update", "--image", w.img, "--version", "1.0.0")
	expectRC(t, r, 0)
	// w.img is the image on record, built from this kit: a fact, so the source is the release
	if w.readState("mode") != "kit" || w.readState("fw_source") != "release:1.0.0" {
		t.Errorf("mode %q source %q", w.readState("mode"), w.readState("fw_source"))
	}
}

// The image the camera already runs (same md5) is not the default row.
func TestImageAlreadyOnTheCameraIsNoDefault(t *testing.T) {
	w := newFwWorldBuilt(t, relList(relJSON("v2.0.0", false, false)), "image_md5=abc123\nflash_md5=abc123\n")
	r := w.update("\n10\n")
	contains(t, r.out+r.err, "1) Akam 2.0.0 (latest)", "The image already built from your backup (", "  - already on the camera")
}
