package cli_test

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

const doneAt = "2026-11-11T00:00:00Z\n"

// seedUpdate makes the state of an Akam camera that has its kit; backup is the folder recorded as its backup.
func (w *fw) seedUpdate(t *testing.T, backup string) {
	t.Helper()
	st := "camera_ip=127.0.0.1\ncamera_kind=akam\nmode=source\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt
	if backup != "" {
		st += "backup_dir=" + backup + "\n"
	}
	w.writeState(st)
}

func (w *fw) homeRun(stdin string, extra ...string) result {
	return w.run(stdin, append([]string{"--camera", "default"}, extra...)...)
}

// The update flow end to end: the backup gate re-verifies SHA256SUMS, the red box asks (default No),
// the image is written and read back, and the camera is never power-cycled by the assistant.
func TestUpdateFlowEndToEnd(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	bk := backupFixture(t)
	w.seedUpdate(t, bk)
	// home 2 = update; red box y; "verify now?" Enter (No); 9 = quit
	r := w.homeRun("2\ny\n\n10\n", "--image", w.img)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Update default (127.0.0.1)", "[next]  1  Kit downloaded and verified",
		"Backup checked: "+bk+" (SHA256SUMS ok, re-hashed just now).",
		"Updating: WRITES THE CAMERA FLASH", "Image md5 written and read back: "+w.imgMD5,
		"Flash done and verified. The wizard NEVER power-cycles the camera.",
		"Verify it now? [y/N]", "When it is back: ./akam verify")
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Fatal("the image was not written")
	}
	w.checkClosedSet(t)
}

// Not confirmed in the red box: nothing is written, and the default of every question is No.
func TestUpdateFlowDefaultsToNo(t *testing.T) {
	for _, in := range []string{"2\n\n10\n", "2\nn\n10\n", "2\nq\n10\n"} {
		w := newFlashWorld(t, testfake.FlashOptions{})
		w.seedUpdate(t, backupFixture(t))
		r := w.homeRun(in, "--image", w.img)
		expectRC(t, r, 0)
		contains(t, r.out+r.err, "not confirmed.", "nothing was written.")
		w.unchanged(t)
	}
}

// A backup that no longer verifies stops the update before the camera is touched.
func TestUpdateFlowBackupGateRehashes(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	bk := backupFixture(t)
	w.seedUpdate(t, bk)
	// the backup is damaged after it was recorded
	if err := os.WriteFile(filepath.Join(bk, "mtdblock3.bin"), []byte("rotten"), 0o600); err != nil {
		t.Fatal(err)
	}
	r := w.homeRun("2\nn\ny\n10\n", "--image", w.img) // refuse the offered SSH backup; (the extra y must not flash)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "no verified backup: SHA256SUMS does not match the files in "+bk,
		"Back up this camera over SSH now? [y/N]", "the update needs a backup that verifies first.")
	w.unchanged(t)
	if len(w.cam.RamFile("image.squashfs")) != 0 {
		t.Error("the image was uploaded without a verified backup")
	}
}

// No recorded backup: one is offered over SSH (read-only), written to the proposed folder, and the
// update then goes on.
func TestUpdateFlowOffersAnSSHBackupFirst(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	root := t.TempDir()
	t.Setenv("AKAM_BACKUP_ROOT", root)
	w.seedUpdate(t, "")
	// 2 update; y = back up over SSH now; 1 = use the proposed folder; y = red box; Enter = no verify; 9 quit
	r := w.homeRun("2\ny\n1\ny\n\n10\n", "--image", w.img)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "no verified backup: no backup of this camera is recorded.",
		"Where should the backup of the camera be kept?", "Suggested:  "+root, "Backup saved and verified",
		"Backup checked: ", "Updating: WRITES THE CAMERA FLASH")
	dirs, _ := filepath.Glob(filepath.Join(root, "akam-backup-*"))
	if len(dirs) != 1 {
		t.Fatalf("backup folders: %v", dirs)
	}
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Error("the image was not written after the backup")
	}
	// the dumps were read, never written back
	for _, c := range w.cam.Commands() {
		if strings.Contains(c, "of=/dev/") {
			t.Errorf("write command %q", c)
		}
	}
}

// A stock camera is not updated here, and a rehearsal sends nothing.
func TestUpdateFlowRefusesStockAndRehearses(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.writeState("camera_ip=127.0.0.1\ncamera_kind=stock\n")
	r := w.homeRun("2\n10\n", "--image", w.img)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "this camera runs the stock firmware, not Akam")
	w.unchanged(t)

	w.seedUpdate(t, backupFixture(t))
	r = w.homeRun("", "--dry-run", "--image", w.img)
	expectRC(t, r, 0)
	if len(w.cam.Commands()) != 0 {
		t.Errorf("a rehearsal sent %q", w.cam.Commands())
	}
}
