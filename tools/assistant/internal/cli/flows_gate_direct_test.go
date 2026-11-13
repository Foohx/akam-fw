package cli_test

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// seedFlashCard writes the state and the files of a prepared flash-card run: image is the file to flash,
// stock the stock APP of the backup folder (its SHA256SUMS is made to match, so only the structure check
// can refuse it).
func seedFlashCard(t *testing.T, w *sdWorld, image, stock []byte) {
	t.Helper()
	dir := t.TempDir()
	imgPath := filepath.Join(dir, "ak_rtsp_firmware_20261111_101010.squashfs")
	os.WriteFile(imgPath, image, 0o600)
	tool := filepath.Join(dir, "flash_tool")
	os.WriteFile(tool, []byte("fake flash tool"), 0o755)
	t.Setenv("AKAM_FLASH_TOOL", tool)
	bk := filepath.Join(dir, "backup")
	os.MkdirAll(bk, 0o755)
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		b := stockMtd(t, i)
		if i == 7 {
			b = stock
		}
		os.WriteFile(filepath.Join(bk, fmt.Sprintf("mtdblock%d.bin", i)), b, 0o600)
		fmt.Fprintf(&sums, "%s  mtdblock%d.bin\n", shaOf(b), i)
	}
	os.WriteFile(filepath.Join(bk, "SHA256SUMS"), []byte(sums.String()), 0o600)
	app := shaOf(stock)
	st := "mode=source\nimage_path=" + imgPath + "\nimage_sha=" + shaOf(image) + "\nimage_kit_sha=source\nimage_backup_sha=" + app + "\nbackup_dir=" + bk + "\nbackup_sha=" + app + "\n"
	for _, s := range []string{"doctor", "get_kit", "prepare_backup_sd", "find_camera", "backup", "image"} {
		st += "step_" + s + "_done=2026-01-01T00:00:00Z\n"
	}
	w.writeState(st)
}

func junkImage() []byte {
	return bytes.Repeat([]byte("not a squashfs "), 5242880/15+1)[:5242880]
}

// cmdPrepareFlashSD refuses a non-squashfs image, and a non-squashfs stock APP for the card, before the
// red box and before one byte is written to the card.
func TestPrepareFlashSDRefusesANonSquashfsImage(t *testing.T) {
	for _, tc := range []struct {
		name         string
		image, stock []byte
	}{
		{"image", junkImage(), validImage(t)},
		{"stock APP", validImage(t), junkImage()},
	} {
		t.Run(tc.name, func(t *testing.T) {
			w := newSDWorld(t)
			seedFlashCard(t, w, tc.image, tc.stock)
			r := w.run("1\ny\ny\n", "prepare-flash-sd", "--device", w.img)
			expectRC(t, r, 1)
			contains(t, r.out+r.err, "this file is not a valid squashfs image", "do not flash it")
			notContains(t, r.out+r.err, "sd_card: VERIFY")
			if w.be.Prepared != 0 {
				t.Error("the card was prepared")
			}
			w.untouched(t)
		})
	}
}

// cmdFlash refuses a non-squashfs image before the red box, the card or the camera.
func TestFlashCommandRefusesANonSquashfsImage(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	before := fw.mtd7()
	if err := os.WriteFile(fw.imgPath, junkImage(), 0o600); err != nil {
		t.Fatal(err)
	}
	fw.seed() // image_sha follows the file: only the structure check can refuse it
	r := fw.run("y\ny\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this file is not a valid squashfs image", "do not flash it")
	notContains(t, r.out+r.err, "WRITES THE CAMERA FLASH")
	if len(fw.st.Confirms()) != 0 || !bytes.Equal(fw.mtd7(), before) || fw.ran("trap '' HUP") {
		t.Error("the camera was reached")
	}
	if fw.readState("step_flash_done") != "" {
		t.Error("recorded")
	}
}

// The guided update says "Image structure checked" once, not once per layer.
func TestUpdateFlowSaysTheStructureCheckOnce(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.seedUpdate(t, backupFixture(t))
	r := w.homeRun("2\ny\n\n9\n", "--image", w.img)
	expectRC(t, r, 0)
	if n := strings.Count(r.out+r.err, "Image structure checked"); n != 1 {
		t.Errorf("structure line printed %d times", n)
	}
}

// A vendor-style squashfs WITH extended attributes is not refused: the reader only flags them.
func TestImageWithXattrsIsAccepted(t *testing.T) {
	for _, tool := range []string{"mksquashfs", "setfattr"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip(tool + " is not installed")
		}
	}
	src := filepath.Join(t.TempDir(), "src")
	os.MkdirAll(src, 0o755)
	f := filepath.Join(src, "file")
	os.WriteFile(f, bytes.Repeat([]byte("xattr data "), 5000), 0o644)
	if out, err := exec.Command("setfattr", "-n", "user.akam", "-v", "x", f).CombinedOutput(); err != nil {
		t.Skip("xattrs not supported here: " + string(out))
	}
	img := filepath.Join(t.TempDir(), "x.sqfs")
	if out, err := exec.Command("mksquashfs", src, img, "-noappend", "-quiet", "-comp", "xz", "-xattrs", "-all-root").CombinedOutput(); err != nil {
		t.Skip("mksquashfs failed: " + string(out))
	}
	b, err := os.ReadFile(img)
	if err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint64(b[56:]) == ^uint64(0) {
		t.Skip("the image has no xattr table")
	}
	b = append(b, make([]byte, 5242880-len(b))...)
	w := newSDWorld(t)
	seedFlashCard(t, w, b, b) // as image and as stock APP
	r := w.run("1\ny\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "Image structure checked", "sd_card: VERIFY: OK")
	notContains(t, r.out+r.err, "not a valid squashfs image")
}
