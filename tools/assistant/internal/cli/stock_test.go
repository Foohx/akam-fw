//go:build akamtest

package cli_test

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// These tests drive the stock-camera commands (backup, flash over the maintenance telnet) against the
// fake stock camera: a telnet server over a private directory tree whose mtd "devices" are files.

type stockWorld struct {
	*env
	st         *testfake.Stock
	backupRoot string
}

func newStockWorld(t *testing.T, so testfake.StockOptions) *stockWorld {
	t.Helper()
	e := newEnv(t)
	st := testfake.NewStock(t, so)
	w := &stockWorld{env: e, st: st, backupRoot: t.TempDir()}
	t.Setenv("AKAM_PORT_TELNET", itoa(st.Port()))
	t.Setenv("AKAM_PORT_XFER_IN", freeClosedPort(t))
	t.Setenv("AKAM_PORT_XFER_OUT", freeClosedPort(t))
	t.Setenv("AKAM_CAMSH_TIMEOUT_MS", "1500")
	t.Setenv("AKAM_XFER_CONNECT_TRIES", "20")
	t.Setenv("AKAM_XFER_CONNECT_MS", "50")
	t.Setenv("AKAM_BACKUP_ROOT", w.backupRoot)
	t.Cleanup(func() {
		if ev := st.Events(); len(ev) != 0 {
			t.Errorf("the stock camera received reboot/poweroff/kill commands: %v", ev)
		}
		if u := st.Unknown(); len(u) != 0 {
			t.Errorf("commands the fake does not know (not mirrored from bash?): %v", u)
		}
		for _, c := range st.Commands() {
			if strings.Contains(c, "reboot") || strings.Contains(c, "watchdog") || strings.Contains(c, "kill") {
				t.Errorf("forbidden command sent: %s", c)
			}
		}
	})
	return w
}

func shaOf(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

const stockSteps = "step_doctor_done=2026-01-01T00:00:00Z\nstep_prepare_backup_sd_done=2026-01-01T00:00:00Z\nstep_find_camera_done=2026-01-01T00:00:00Z\ncamera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\n"

func (w *stockWorld) backupDirs(t *testing.T) []string {
	ms, _ := filepath.Glob(filepath.Join(w.backupRoot, "akam-backup-*"))
	return ms
}

func (w *stockWorld) ran(sub string) bool {
	for _, c := range w.st.Commands() {
		if strings.Contains(c, sub) {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------- find-camera

func TestFindCameraTelnetDetectsAkamWithTheMaintenanceCard(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{Akam: true})
	w.writeState("step_doctor_done=2026-01-01T00:00:00Z\nstep_prepare_backup_sd_done=2026-01-01T00:00:00Z\n")
	r := w.run("", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "  Shell answered: Linux version", "  Camera found: it already runs Akam (127.0.0.1).")
	if w.readState("camera_kind") != "akam" || w.readState("camera_telnet") != "1" {
		t.Errorf("state: kind=%q telnet=%q", w.readState("camera_kind"), w.readState("camera_telnet"))
	}
}

func TestFindCameraTelnetNotTheToolkitShell(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.st.Drop(10) // the shell never answers: a login prompt or another service
	w.writeState("step_doctor_done=2026-01-01T00:00:00Z\nstep_prepare_backup_sd_done=2026-01-01T00:00:00Z\n")
	t.Setenv("AKAM_CAMSH_TIMEOUT_MS", "300")
	r := w.run("", "find-camera")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no shell answered", "stop here. The toolkit shell has no login.")
}

func TestFindCameraManualTelnetDoesNotOpenTheShell(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState("step_doctor_done=2026-01-01T00:00:00Z\nstep_prepare_backup_sd_done=2026-01-01T00:00:00Z\n")
	r := w.run("", "--manual-telnet", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "  Manual telnet mode: not opening the shell; assuming stock firmware with the toolkit.")
	if len(w.st.Commands()) != 0 {
		t.Error("the shell was used in manual mode")
	}
}

// ---------------------------------------------------------------- backup

func TestBackupEndToEnd(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup") // untested firmware, second copy
	expectRC(t, r, 0)
	contains(t, r.out, "Step 5/9 - Back up the camera (all 8 partitions)",
		"  Partition table: LAYOUT_OK (8 partitions, APP 00500000).",
		"  Kernel: 4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025",
		"  The card is mounted at /tmp/sd on the camera.",
		"  Free space on the card: 900000 KiB.",
		"  Reading mtdblock0 (run 1 of 2) ...", "  Reading mtdblock7 (run 2 of 2) ...",
		"  Sizes of the 16 dump files are exact.",
		"  READS_IDENTICAL: both dumps match on all 8 partitions.",
		"  Backup copied and verified (every file matches the camera's two reads). SHA256SUMS written.",
		"  Hardware verdict for this stock APP: untested - not in the supported hardware list (unknown stock APP)",
		"  Backup folder accepted: ")
	dirs := w.backupDirs(t)
	if len(dirs) != 1 {
		t.Fatalf("backup folders: %v", dirs)
	}
	for i := 0; i < 8; i++ {
		got, err := os.ReadFile(filepath.Join(dirs[0], fmt.Sprintf("mtdblock%d.bin", i)))
		if err != nil || !bytes.Equal(got, testfake.StockMtdContent(i)) {
			t.Errorf("mtdblock%d differs (err %v)", i, err)
		}
	}
	sums, _ := os.ReadFile(filepath.Join(dirs[0], "SHA256SUMS"))
	if !strings.Contains(string(sums), shaOf(testfake.StockMtdContent(7))+"  mtdblock7.bin") {
		t.Errorf("SHA256SUMS: %s", sums)
	}
	for _, f := range []string{"README.txt", "camera_info.txt"} {
		if _, err := os.Stat(filepath.Join(dirs[0], f)); err != nil {
			t.Error(err)
		}
	}
	if w.readState("backup_dir") != dirs[0] || w.readState("backup_sha") != shaOf(testfake.StockMtdContent(7)) || !strings.Contains(w.logText()+w.readState("step_backup_done"), "20") {
		t.Errorf("state backup_dir=%q", w.readState("backup_dir"))
	}
	if w.readState("step_backup_done") == "" {
		t.Error("backup not marked done")
	}
	// the two dumps are on the card, and nothing was written to a flash partition
	if !w.ran("dd if=/dev/mtdblock7 of=/tmp/sd/dump2/mtdblock7.bin bs=64k") {
		t.Error("the dd of run 2 was not sent")
	}
	if w.ran("of=/dev/") {
		t.Error("a write to /dev was sent")
	}
	// sha256sum -c works on the folder (same layout as the bash wizard)
	if w.ran("sha256sum mtdblock*.bin | tee /tmp/sd/sums_run1.sha256sum") == false {
		t.Error("hash run 1 command differs from bash")
	}
}

func TestBackupRefusalsLeaveNothingBehind(t *testing.T) {
	// the user says No to the untested firmware: the verified copy stays, nothing is accepted
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(stockSteps)
	r := w.run("n\n", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "untested firmware not confirmed.", "The verified copy stays in ")
	if w.readState("step_backup_done") != "" || w.readState("backup_dir") != "" {
		t.Error("a refused backup was recorded")
	}
	// no terminal: refused (the red box needs one)
	t.Setenv("AKAM_FORCE_TTY", "")
	w2 := w.run("y\ny\n", "backup")
	expectRC(t, w2, 1)
	if w.readState("step_backup_done") != "" {
		t.Error("recorded without a terminal")
	}
}

func TestBackupSecondCopyDefaultsToNo(t *testing.T) {
	for _, in := range []string{"y\n\n", "y\nn\n", "y\n"} {
		w := newStockWorld(t, testfake.StockOptions{})
		w.writeState(stockSteps)
		r := w.run(in, "backup")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "second copy not confirmed.")
		if w.readState("step_backup_done") != "" {
			t.Errorf("input %q: recorded without a second copy", in)
		}
	}
}

func TestBackupRefusesAnUnsupportedLayout(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{Layout: "dev:    size   erasesize  name\nmtd0: 00100000 00001000 \"ALL\"\n"})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the partition table of this camera is not the supported 8-partition layout:", "do NOT continue: this camera model or firmware layout is not supported. Nothing was changed.")
	if w.ran("dd if=") || w.ran("mkdir") {
		t.Error("a dump was started on an unsupported layout")
	}
}

func TestBackupRefusesWhenTheCardHasNoRoom(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{FreeKiB: 1000})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the card has only 1000 KiB free, 17000 KiB are needed for the two dumps.")
	if w.ran("dd if=") {
		t.Error("dump started")
	}
}

func TestBackupRefusesAnAkamCamera(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(strings.Replace(stockSteps, "camera_kind=stock", "camera_kind=akam", 1))
	r := w.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this camera already runs Akam: its flash no longer holds the original firmware.")
	if len(w.st.Commands()) != 0 {
		t.Error("the camera was contacted")
	}
}

func TestBackupDifferentReadsAreFatalExceptConfig(t *testing.T) {
	// a flash partition that reads differently the second time: the dumps are not usable
	w := newStockWorld(t, testfake.StockOptions{DiffSecondRead: []int{3}})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the two reads differ on mtdblock3 (not CONFIG only): the read is not reliable.")
	if len(w.backupDirs(t)) != 0 {
		t.Error("a backup folder was made from unreliable reads")
	}
	// CONFIG alone is read again twice, then the first read is used (like bash)
	w = newStockWorld(t, testfake.StockOptions{DiffSecondRead: []int{6}})
	w.writeState(stockSteps)
	r = w.run("y\ny\n", "backup")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "reading it again (1 of 2).", "reading it again (2 of 2).", "mtdblock6 (CONFIG) still differs between the two reads.")
	b, _ := os.ReadFile(filepath.Join(w.backupDirs(t)[0], "camera_info.txt"))
	if !strings.Contains(string(b), "config_partition_differs_between_reads: 1") {
		t.Errorf("camera_info: %s", b)
	}
}

func TestBackupTransferFaults(t *testing.T) {
	cases := map[string]func(*testfake.Stock){
		"hash mismatch":        func(s *testfake.Stock) { s.CorruptTx(100) },
		"interrupted transfer": func(s *testfake.Stock) { s.TruncateTx(100, 1000) },
	}
	for name, inject := range cases {
		w := newStockWorld(t, testfake.StockOptions{})
		w.writeState(stockSteps)
		inject(w.st)
		r := w.run("y\ny\n", "backup")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "transfer from the camera failed after 3 tries", "The automatic transfer did not work. You still have the dumps on the card:", "backup --backup-dir THAT_FOLDER")
		for _, d := range w.backupDirs(t) {
			ents, _ := os.ReadDir(d)
			for _, e := range ents {
				if strings.HasSuffix(e.Name(), ".part") || e.Name() == "SHA256SUMS" {
					t.Errorf("%s: %s left in the backup folder", name, e.Name())
				}
			}
		}
		if w.readState("step_backup_done") != "" {
			t.Errorf("%s: recorded", name)
		}
	}
}

func TestBackupTransferRecoversFromOneBadTry(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(stockSteps)
	w.st.CorruptTx(1)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "the received file differs", "try 2 of 3")
}

func TestBackupMd5OnlyCamera(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{NoSha256: true})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 0)
	contains(t, r.out, "(md5sum, this takes a moment)")
	b, _ := os.ReadFile(filepath.Join(w.backupDirs(t)[0], "camera_info.txt"))
	if !strings.Contains(string(b), "camera_hash_tool: md5sum") {
		t.Errorf("%s", b)
	}
}

func TestBackupCameraSilentFallsBackToManualAndNeverInventsOutput(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(stockSteps)
	t.Setenv("AKAM_CAMSH_TIMEOUT_MS", "300")
	w.st.Drop(1000)
	r := w.run("", "backup") // no input: nobody pastes anything
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the camera did not answer (failure 1 of 2).", "failure 2 of 2", "switching to MANUAL mode",
		"MANUAL STEP - in your own telnet session to the camera (127.0.0.1 port", "      cat /proc/mtd", "no input: cannot continue in manual mode.")
	if len(w.backupDirs(t)) != 0 || w.readState("step_backup_done") != "" {
		t.Error("something was recorded from invented output")
	}
}

func TestBackupDryRunTouchesNothing(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState("")
	os.WriteFile(filepath.Join(w.state, "state.dry"), []byte(stockSteps), 0o600)
	r := w.run("", "--dry-run", "backup")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD open the telnet shell on the camera and: check /proc/mtd is the 8-partition layout (refuse otherwise),")
	if len(w.st.Commands()) != 0 || len(w.backupDirs(t)) != 0 {
		t.Error("a rehearsal touched the camera or the disk")
	}
}

func TestBackupImportOfAFolderMadeByHand(t *testing.T) {
	w := newStockWorld(t, testfake.StockOptions{})
	dir := t.TempDir()
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		c := testfake.StockMtdContent(i)
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("mtdblock%d.bin", i)), c, 0o600)
		fmt.Fprintf(&sums, "%s  mtdblock%d.bin\n", shaOf(c), i)
	}
	os.WriteFile(filepath.Join(dir, "SHA256SUMS"), []byte(sums.String()), 0o600)
	w.writeState("")
	r := w.run("n\n", "backup", "--backup-dir", dir)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "  SHA256SUMS verified.", "COPY THIS FOLDER TO A SECOND MEDIUM", "second copy not confirmed.")
	r = w.run("y\n", "backup", "--backup-dir", dir)
	expectRC(t, r, 0)
	contains(t, r.out, "  Backup folder accepted: ")
	// a damaged copy is refused
	os.WriteFile(filepath.Join(dir, "mtdblock2.bin"), []byte("x"), 0o600)
	r = w.run("y\n", "backup", "--backup-dir", dir)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "SHA256SUMS does not match the files in")
	if len(w.st.Commands()) != 0 {
		t.Error("the camera was contacted for an import")
	}
}

// ---------------------------------------------------------------- flash over the maintenance telnet

type cardFlashWorld struct {
	*stockWorld
	img      []byte
	imgName  string
	imgPath  string
	toolPath string
	tool     []byte
	bkDir    string
}

func newCardFlashWorld(t *testing.T, so testfake.StockOptions, cardMod func(files map[string][]byte)) *cardFlashWorld {
	t.Helper()
	dir := t.TempDir()
	fw := &cardFlashWorld{imgName: "ak_rtsp_firmware_20261111_101010.squashfs"}
	fw.img = bytes.Repeat([]byte("akam image block 0123456789abcdef"), 5242880/33+1)[:5242880]
	fw.tool = []byte("#!fake flash_tool\n")
	files := map[string][]byte{"custom_firmware/" + fw.imgName: fw.img, "flash_tool": fw.tool, "hack.sh": []byte("x")}
	for _, n := range []string{"install_with_flash_tool.sh", "flash_guard.sh", "preflight_check.sh"} {
		b, err := assets.Script(n)
		if err != nil {
			t.Fatal(err)
		}
		files[n] = b
	}
	if cardMod != nil {
		cardMod(files)
	}
	so.Files = files
	fw.stockWorld = newStockWorld(t, so)
	fw.imgPath = filepath.Join(dir, fw.imgName)
	os.WriteFile(fw.imgPath, fw.img, 0o600)
	fw.toolPath = filepath.Join(dir, "flash_tool")
	os.WriteFile(fw.toolPath, fw.tool, 0o755)
	t.Setenv("AKAM_FLASH_TOOL", fw.toolPath)
	fw.bkDir = filepath.Join(dir, "backup")
	os.MkdirAll(fw.bkDir, 0o755)
	for i := 0; i < 8; i++ {
		os.WriteFile(filepath.Join(fw.bkDir, fmt.Sprintf("mtdblock%d.bin", i)), testfake.StockMtdContent(i), 0o600)
	}
	fw.seed()
	return fw
}

// seed writes the state of a camera whose flash card was prepared (re-run after changing a file).
func (fw *cardFlashWorld) seed() {
	img, _ := os.ReadFile(fw.imgPath)
	app := shaOf(testfake.StockMtdContent(7))
	steps := ""
	for _, s := range []string{"doctor", "get_kit", "prepare_backup_sd", "find_camera", "backup", "image", "prepare_flash_sd"} {
		steps += "step_" + s + "_done=2026-01-01T00:00:00Z\n"
	}
	state := (steps + "mode=source\ncamera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\n" +
		"image_path=" + fw.imgPath + "\nimage_sha=" + shaOf(fw.img) + "\nimage_kit_sha=source\nimage_backup_sha=" + app + "\n" +
		"backup_dir=" + fw.bkDir + "\nbackup_sha=" + app + "\nflash_sd_image_sha=" + shaOf(fw.img) + "\n")
	state = strings.ReplaceAll(state, shaOf(fw.img), shaOf(img))
	fw.writeState(state)
	os.WriteFile(filepath.Join(fw.state, "state.dry"), []byte(state), 0o600)
}

func (fw *cardFlashWorld) mtd7() []byte { return fw.st.Mtd(7) }

func TestFlashOverTelnetEndToEnd(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	r := fw.run("y\ny\n", "flash") // untested firmware, then the flash itself
	expectRC(t, r, 0)
	contains(t, r.out, "Step 8/9 - Flash the image to the camera",
		"  Route: telnet",
		"  Partition table: LAYOUT_OK (8 partitions, APP 00500000).",
		"  The card is on /tmp/sd; binding it to /mnt (every script reads /mnt).",
		"  Image on the card: "+fw.imgName+", md5 ",
		"  flash_tool on the card: md5 matches this PC.",
		"  Checking that the camera will keep its Wi-Fi after the card is removed ...",
		"  /etc/config/wpa_supplicant.conf holds a Wi-Fi network: the camera keeps its network without the card.",
		"  The camera shows the same md5 as this PC. Its script now waits for a confirmation.",
		"  The flash script reported: completed successfully.",
		"  Reading the partition back to check it ...",
		"  Flash done and verified. The wizard NEVER power-cycles the camera.")
	if got := fw.st.Confirms(); len(got) != 1 || got[0] != "YES" {
		t.Errorf("confirmations: %v", got)
	}
	if !bytes.Equal(fw.mtd7(), fw.img) {
		t.Error("the partition does not hold the image")
	}
	if fw.readState("flash_route") != "telnet" || fw.readState("step_flash_done") == "" || fw.readState("flash_md5") == "" {
		t.Errorf("state: route=%q", fw.readState("flash_route"))
	}
}

func TestFlashNeverSendsYesWithoutAnExplicitYes(t *testing.T) {
	for _, in := range []string{"y\n", "y\nn\n", "y\n\n", "y\nq\n", "y\nmaybe\n", "y\nno\n"} {
		fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
		before := fw.mtd7()
		r := fw.run(in, "flash")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "not confirmed. Aborting; nothing was written.")
		if got := fw.st.Confirms(); len(got) != 1 || got[0] != "NO" {
			t.Errorf("input %q: the script saw %v", in, got)
		}
		if !bytes.Equal(fw.mtd7(), before) {
			t.Errorf("input %q: the partition changed", in)
		}
		if fw.readState("step_flash_done") != "" {
			t.Errorf("input %q: recorded", in)
		}
	}
}

func TestFlashWithoutATerminalIsRefused(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	t.Setenv("AKAM_FORCE_TTY", "")
	r := fw.run("y\ny\ny\n", "flash")
	expectRC(t, r, 1)
	if len(fw.st.Confirms()) != 0 || fw.ran("trap '' HUP") {
		t.Error("the flash script was started without a terminal")
	}
	if !bytes.Equal(fw.mtd7(), testfake.StockMtdContent(7)) {
		t.Error("the partition changed")
	}
}

func TestFlashManualModeNeverTypesYes(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	r := fw.run("y\n", "--manual-telnet", "flash")
	expectRC(t, r, 1)
	if len(fw.st.Commands()) != 0 || len(fw.st.Confirms()) != 0 {
		t.Error("the camera was used in manual mode")
	}
	contains(t, r.out+r.err, "MANUAL STEP")
}

func TestFlashRefusesAnUnsupportedLayout(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{Layout: "dev:    size   erasesize  name\nmtd0: 00100000 00001000 \"ALL\"\n"}, nil)
	r := fw.run("y\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not the supported 8-partition layout")
	if fw.ran("trap '' HUP") || len(fw.st.Confirms()) != 0 {
		t.Error("the flash script was started")
	}
}

func TestFlashWrongImageOnTheCardNeedsAYes(t *testing.T) {
	other := bytes.Repeat([]byte("other"), 5242880/5+1)[:5242880]
	fw := newCardFlashWorld(t, testfake.StockOptions{}, func(f map[string][]byte) {
		f["custom_firmware/ak_rtsp_firmware_20250101_000000.squashfs"] = other
		delete(f, "custom_firmware/ak_rtsp_firmware_20261111_101010.squashfs")
	})
	r := fw.run("y\nn\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the card does not carry exactly the image you want to flash", "the image on the card does not match.")
	if fw.ran("rm -f /mnt/custom_firmware") || len(fw.st.Confirms()) != 0 {
		t.Error("the card was changed without a yes")
	}
	// with a yes the image is replaced over nc, verified, and flashed
	fw = newCardFlashWorld(t, testfake.StockOptions{}, func(f map[string][]byte) {
		f["custom_firmware/ak_rtsp_firmware_20250101_000000.squashfs"] = other
		delete(f, "custom_firmware/ak_rtsp_firmware_20261111_101010.squashfs")
	})
	r = fw.run("y\ny\ny\n", "flash")
	expectRC(t, r, 0)
	contains(t, r.out, "  Verified on the camera: /mnt/custom_firmware/"+fw.imgName)
	if !bytes.Equal(fw.mtd7(), fw.img) {
		t.Error("not flashed")
	}
}

func TestFlashPushedFileMismatchIsDetected(t *testing.T) {
	// flash_tool missing on the card: pushed after a yes, but the nc transfer is corrupted every time
	fw := newCardFlashWorld(t, testfake.StockOptions{}, func(f map[string][]byte) { delete(f, "flash_tool") })
	fw.st.CorruptRx(10)
	r := fw.run("y\ny\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "flash_tool is missing on the card.", "the camera received a different file", "transfer to the camera failed after 3 tries")
	if len(fw.st.Confirms()) != 0 || !bytes.Equal(fw.mtd7(), testfake.StockMtdContent(7)) {
		t.Error("flashing went on after a failed transfer")
	}
}

func TestFlashRefusesWithoutWifiOnTheCamera(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{WifiVerdict: "NO_CARD_CONFIG"}, nil)
	r := fw.run("y\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the camera has no saved Wi-Fi", "Nothing was written to the camera.")
	if len(fw.st.Confirms()) != 0 || fw.ran("trap '' HUP") {
		t.Error("the flash was started without a Wi-Fi network on the camera")
	}
}

func TestFlashDetectsABadWriteByReadBack(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{FlashMode: "badwrite"}, nil)
	r := fw.run("y\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the partition read back as md5", "DO NOT power-cycle.")
	if fw.readState("step_flash_done") != "" {
		t.Error("a bad write was recorded as done")
	}
}

func TestFlashScriptFailureAndMissingSuccessLine(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{FlashMode: "fail"}, nil)
	r := fw.run("y\ny\n", "flash")
	expectRC(t, r, 1) // the script wrote nothing: the read-back differs
	contains(t, r.out+r.err, "the flash script exited with code 1; the read-back below decides.", "the partition read back as md5")
	// no success line, but the partition is right: the read-back decides
	fw = newCardFlashWorld(t, testfake.StockOptions{FlashMode: "noline"}, nil)
	r = fw.run("y\ny\n", "flash")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "the flash script ended without its success line; the read-back below decides.")
}

func TestFlashRefusesAWrongSizeImage(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	os.WriteFile(fw.imgPath, []byte("short"), 0o600)
	fw.seed()
	r := fw.run("y\ny\n", "flash")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the image is 5 bytes, expected exactly 5242880.")
	if len(fw.st.Commands()) != 0 {
		t.Error("the camera was contacted")
	}
}

func TestFlashDryRunShowsTheRouteAndTouchesNothing(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	r := fw.run("", "--dry-run", "flash")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD open the telnet shell, bind the card on /mnt if it is on /tmp/sd", "No reboot is ever sent.")
	if len(fw.st.Commands()) != 0 {
		t.Error("a rehearsal touched the camera")
	}
}

func TestRestoreOverTelnetWritesTheStockApp(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{Mtd7: bytes.Repeat([]byte("akam"), 5242880/4)}, func(f map[string][]byte) {
		for k := range f {
			if strings.HasPrefix(k, "custom_firmware/") {
				delete(f, k)
			}
		}
		f["custom_firmware/placeholder.txt"] = []byte("x")
	})
	r := fw.run("y\ny\ny\n", "restore", "--backup-dir", fw.bkDir)
	// card carries no image: it is pushed after the yes, then the stock APP is written back
	expectRC(t, r, 0)
	contains(t, r.out, "  Route: telnet", "  Restore written and verified (read-back md5 = ")
	if !bytes.Equal(fw.mtd7(), testfake.StockMtdContent(7)) {
		t.Error("the stock APP was not restored")
	}
	if got := fw.st.Confirms(); len(got) != 1 || got[0] != "YES" {
		t.Errorf("confirmations: %v", got)
	}
}
