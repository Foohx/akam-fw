package cli_test

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The first install resumes where the state says: everything up to the flash card is done, so the
// list shows nine ticks and the flash is next. The red box still needs its yes (default No).
func TestInstallFlowResumesAtTheFlashAndNeverPowerCycles(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	// 1 = set up; "y" untested firmware; "y" red box; Enter = do not verify now; 9 = quit
	r := fw.run("1\ny\ny\n\n9\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "[done]  1  Check this computer", "[done]  3  Choose the backup folder", "[done]  8  Prepare the flash card",
		"[next]  9  Write the camera flash", "Flash done and verified. The wizard NEVER power-cycles the camera.",
		"Now power-cycle the camera BY HAND", "Is the camera power-cycled and back on the network? Verify it now? [y/N]",
		"'verify' is not finished. Nothing else was started.")
	if !bytes.Equal(fw.mtd7(), fw.img) {
		t.Error("the partition does not hold the image")
	}
	if fw.readState("step_flash_done") == "" {
		t.Error("the flash step was not recorded")
	}
	if fw.readState("step_verify_done") != "" {
		t.Error("verify recorded without a yes")
	}
}

func TestInstallFlowRedBoxDefaultsToNo(t *testing.T) {
	for _, in := range []string{"1\ny\n\n9\n", "1\ny\nn\n9\n", "1\ny\nq\n9\n"} {
		fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
		before := fw.mtd7()
		r := fw.run(in, "--camera", "default")
		expectRC(t, r, 0)
		if !bytes.Equal(fw.mtd7(), before) {
			t.Errorf("input %q: the partition changed without a yes", in)
		}
		for _, c := range fw.st.Confirms() {
			if c == "YES" {
				t.Errorf("input %q: YES sent to the camera", in)
			}
		}
		contains(t, r.out+r.err, "Stopped at 'flash'")
	}
}

// The image step: on Linux without --image it says how the image is built; with --image it imports a
// file of exactly the APP size, shows the md5 and records it (the state the flash step checks).
func TestInstallFlowImageImport(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	// forget the image and the later steps
	st, _ := os.ReadFile(filepath.Join(fw.state, "state"))
	var keep []string
	for _, l := range strings.Split(string(st), "\n") {
		if strings.HasPrefix(l, "step_image_done") || strings.HasPrefix(l, "step_prepare_flash_sd_done") || strings.HasPrefix(l, "image_") || strings.HasPrefix(l, "flash_sd_image_sha") {
			continue
		}
		keep = append(keep, l)
	}
	fw.writeState(strings.Join(keep, "\n"))
	r := fw.run("1\n9\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "[next]  7  Build the Akam image from your own backup", "Stopped at 'image'")
	if fw.readState("step_image_done") != "" {
		t.Error("image step recorded without an image")
	}
	// import: the md5 is printed, the red box of the flash step will show it again
	sum := md5.Sum(fw.img)
	want := hex.EncodeToString(sum[:])
	r = fw.run("1\n9\n", "--camera", "default", "--image", fw.imgPath)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "md5 "+want, "this image was built elsewhere", "Image recorded: ")
	if fw.readState("image_path") != fw.imgPath || fw.readState("image_sha") != shaOf(fw.img) || fw.readState("step_image_done") == "" {
		t.Errorf("state: path=%q sha=%q", fw.readState("image_path"), fw.readState("image_sha"))
	}
	// a wrong size is refused
	bad := filepath.Join(t.TempDir(), "bad.squashfs")
	os.WriteFile(bad, []byte("short"), 0o600)
	fw.writeState(strings.Join(keep, "\n"))
	r = fw.run("1\n9\n", "--camera", "default", "--image", bad)
	contains(t, r.out+r.err, "the image is 5 bytes, expected exactly 5242880.")
	if fw.readState("step_image_done") != "" {
		t.Error("a wrong-size image was recorded")
	}
}
