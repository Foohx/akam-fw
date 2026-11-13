package cli_test

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// What the camera runs, what is recorded, and the safety of the kit swap (review round).

func withManifest(v string) func(*testfake.Options) {
	return func(o *testfake.Options) { o.Manifest = v }
}

// An image that was imported (--image or a chosen file) is never recorded as a release, even when the user
// went through a release row and that release's kit is the recorded one.
func TestImportedImageIsNeverLabelledARelease(t *testing.T) {
	sum := sha256.Sum256(validImage(t))
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "image_origin=imported:"+hex.EncodeToString(sum[:])+"\n")
	r := w.update("1\ny\n\n9\n")
	expectRC(t, r, 0)
	if !strings.Contains(r.out+r.err, "Image md5 written and read back") {
		t.Fatal(r.out + r.err)
	}
	if w.readState("fw_source") != "image:"+w.img || w.readState("fw_version") != "" {
		t.Errorf("source %q version %q", w.readState("fw_source"), w.readState("fw_version"))
	}
}

// The version of the camera is read from the manifest its image carries; the version recorded by this
// assistant is only a fallback, and is dropped once the camera reports another build.
func TestCameraVersionComesFromItsManifest(t *testing.T) {
	w := newFwWorldKit(t, relList(relJSON("v2.0.0", false, false), relJSON("v1.0.0", false, false)), "", "", withManifest("# kit\nakam_version=2.0.0\nbuilt_utc=x\n"))
	// no fw_version in the state: only the camera knows. 2 = Akam 1.0.0 -> downgrade, Enter = No, 6 = Back
	r := w.update("2\n\n6\n9\n")
	contains(t, r.out+r.err, "the camera runs: b35-2af81c5; Akam 2.0.0)", "1) Akam 2.0.0 (latest)  - on the camera now", "this is a downgrade.", "No firmware chosen.")
	notContains(t, r.out+r.err, "installed here")
	if w.api.kitHits() != 0 {
		t.Error("a refused downgrade downloaded")
	}
	w.unchanged(t)
	// the manifest of a source build is not a version: unknown, no question
	w = newFwWorldKit(t, relList(relJSON("v1.0.0", false, false)), "", "", withManifest("akam_version=v0.9-12-gabc1234-dirty\n"))
	r = w.update("1\n\n9\n")
	notContains(t, r.out+r.err, "downgrade", "Akam v0.9")
	// no manifest (an older image): the recorded version is used while the build is the one seen after the write
	w = newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "fw_version=2.0.0\nfw_build=b35-2af81c5\n")
	r = w.update("1\n\n9\n")
	contains(t, r.out+r.err, "Akam 2.0.0 installed here", "this is a downgrade.")
	// another build than the one seen: something else was installed since, the recorded version is dropped
	w = newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "fw_version=2.0.0\nfw_build=b12-0000000\n")
	r = w.update("1\n\n9\n")
	notContains(t, r.out+r.err, "installed here", "downgrade")
}

// Nothing the camera says reaches the screen with control characters in it.
func TestHostileCameraVersionTextIsNeverShownRaw(t *testing.T) {
	w := newFwWorldKit(t, relList(relJSON("v1.0.0", false, false)), "", "", func(o *testfake.Options) {
		o.FwVersion = "\x1b[31mevil\x1b]0;pwned\x07"
		o.Manifest = "akam_version=\x1b[2J1.0.0\n"
	})
	r := w.update("2\n9\n")
	out := r.out + r.err
	if strings.ContainsAny(out, "\x1b\x07") {
		t.Errorf("control characters on the screen: %q", out)
	}
	notContains(t, out, "pwned\x07")
}

// A downgrade refused after a local kit was verified changes nothing: the kit state and the kit folder are
// exactly what they were.
func TestRefusedDowngradeOfALocalKitChangesNothing(t *testing.T) {
	w := newFwWorld(t, relList(), "fw_version=2.0.0\n")
	kitDir := filepath.Join(w.state, "kit")
	_ = os.MkdirAll(kitDir, 0o700)
	_ = os.WriteFile(filepath.Join(kitDir, "MARK"), []byte("previous"), 0o600)
	keys := []string{"mode", "kit_path", "kit_sha", "kit_version", "step_get_kit_done", "step_image_done", "image_kit_sha"}
	snap := func() string {
		var o []string
		for _, k := range keys {
			o = append(o, k+"="+w.readState(k))
		}
		return strings.Join(o, "|")
	}
	before := snap()
	kitFile := filepath.Join(w.api.kb.Dir, "akam-kit-1.0.0.tar.xz")
	r := w.update("1\n" + kitFile + "\nn\n9\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Kit verified", "this is a downgrade.", "Go back to Akam 1.0.0? [y/N]", "downgrade not confirmed.", "nothing was changed.")
	if snap() != before {
		t.Errorf("state changed:\n%s\n%s", before, snap())
	}
	if b, _ := os.ReadFile(filepath.Join(kitDir, "MARK")); string(b) != "previous" {
		t.Error("the previous kit folder was replaced")
	}
	w.unchanged(t)
}

// The kit that only supplies the flash tools of an image file is never a downgrade question.
func TestKitForAnImageAsksNoDowngradeQuestion(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "fw_version=2.0.0\n")
	w.writeState(strings.ReplaceAll(readAll(t, filepath.Join(w.state, "state")), "step_get_kit_done="+doneAt, ""))
	// 3 = image file; path; yes; then "Which kit?": 1 = Akam 1.0.0; the red box: No
	r := w.update("3\n" + w.img + "\ny\n1\n\n9\n")
	contains(t, r.out+r.err, "Which kit? (it supplies the flash tools)", "Kit verified", "not confirmed.")
	notContains(t, r.out+r.err, "downgrade", "Go back to Akam")
	w.unchanged(t)
}

// An unsigned kit and a kit that is not release grade are refused through the chooser, and nothing changes.
func TestChooserRefusesUnsignedAndTestGradeKits(t *testing.T) {
	for name, sp := range map[string]kittest.Spec{"unsigned": {Unsigned: true}, "test grade": {Grade: "test"}} {
		w := newFwWorld(t, relList(), "")
		other := kittest.Build(t, t.TempDir(), sp)
		f := filepath.Join(t.TempDir(), "allowed_signers")
		_ = os.WriteFile(f, append(append([]byte{}, w.api.kb.AllowedSigners...), other.AllowedSigners...), 0o600)
		t.Setenv("AKAM_ALLOWED_SIGNERS", f)
		before := w.readState("kit_sha") + w.readState("mode") + w.readState("step_get_kit_done")
		r := w.update("1\n" + other.Tarball + "\nn\n9\n") // No to any "use it anyway" question
		expectRC(t, r, 0)
		contains(t, r.out+r.err, "nothing was changed")
		notContains(t, r.out+r.err, "WRITES THE CAMERA FLASH")
		if w.readState("kit_sha")+w.readState("mode")+w.readState("step_get_kit_done") != before || w.readState("kit_unsigned_ok") == "1" {
			t.Errorf("%s: the kit state changed", name)
		}
		w.unchanged(t)
		if t.Failed() {
			t.Fatal(name)
		}
	}
}

// After a failed read of the list, "Try again" asks GitHub again (and only then).
func TestTryAgainRowAfterAFailedList(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	w.api.failFirst = 1
	// rows: kit file, image file, stock, Try again(4), Back(5); after the retry: 1.0.0, kit, image, stock, Back(5)
	r := w.update("4\n5\n9\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "could not read the list of releases: GitHub refused the request (HTTP 403",
		"4) Try again to read the releases", "1) Akam 1.0.0 (latest)")
	if w.api.listHits != 2 {
		t.Errorf("the list was asked %d times", w.api.listHits)
	}
	notContains(t, r.out+r.err, "Try again to read the releases\n  5)")
}

// In the first install, picking a release downloads and verifies its kit again, whatever kit is recorded.
func TestInstallPickingAReleaseVerifiesTheKitAgain(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("mode=kit\nkit_path=" + a.kb.Tarball + "\nkit_sha=" + a.kb.TarSHA + "\nkit_version=1.0.0\nkit_format=1\nkit_unsigned_ok=1\ncamera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt)
	r := e.run("1\n1\n", "--camera", "default")
	contains(t, r.out+r.err, "1) Akam 1.0.0 (latest)", "ok: signature valid")
	if a.kitHits() != 3 {
		t.Errorf("downloads %v", a.hits)
	}
	if e.readState("kit_unsigned_ok") != "0" {
		t.Errorf("unsigned %q", e.readState("kit_unsigned_ok"))
	}
}

// With --builder native, an image file chosen in the first install is imported, not rebuilt.
func TestInstallImageChoiceWinsOverBuilderNative(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	useAPI(t, fw.env, a)
	st, _ := os.ReadFile(filepath.Join(fw.state, "state"))
	var keep []string
	for _, l := range strings.Split(string(st), "\n") {
		if strings.HasPrefix(l, "step_image_done") || strings.HasPrefix(l, "step_prepare_flash_sd_done") || strings.HasPrefix(l, "image_") || strings.HasPrefix(l, "flash_sd_image_sha") || strings.HasPrefix(l, "mode=") {
			continue
		}
		keep = append(keep, l)
	}
	keep = append(keep, "mode=kit", "kit_path="+a.kb.Tarball, "kit_sha="+a.kb.TarSHA, "kit_version=1.0.0", "kit_format=1")
	fw.writeState(strings.Join(keep, "\n") + "\n")
	// 1 = set up; 3 = an image file; its path; yes; then the image step imports it
	r := fw.run("1\n3\n"+fw.imgPath+"\ny\n9\n", "--camera", "default", "--builder", "native")
	contains(t, r.out+r.err, "this image was built elsewhere", "Image recorded: ")
	notContains(t, r.out+r.err, "EXPERIMENTAL")
	if fw.readState("image_origin") != "imported:"+shaOf(fw.img) {
		t.Errorf("origin %q", fw.readState("image_origin"))
	}
}
