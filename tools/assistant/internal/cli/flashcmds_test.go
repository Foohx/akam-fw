package cli_test

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The reflash tests run the whole update/restore/verify flow against the fake camera: an in-process SSH
// server with a second Dropbear, a fake flash_tool (a copy into a byte slice that plays /dev/mtd7)
// and the camera script behaviour of tools/flash_scripts/akam_reflash_ssh.sh (the real script is
// tested by tests/reflash_ssh_test.sh). Nothing touches a camera, a block device or the host's /tmp.

type fw struct {
	*env
	img     string
	imgData []byte
	imgMD5  string
	dir     string
}

func newFlashWorld(t *testing.T, fo testfake.FlashOptions) *fw {
	t.Helper()
	e := newEnv(t, func(o *testfake.Options) { o.Flash = fo })
	e.flashTest = true
	w := &fw{env: e, dir: t.TempDir()}
	w.imgData = bytes.Repeat([]byte("akam image block 0123456789abcdef"), 5242880/33+1)[:5242880]
	w.img = filepath.Join(w.dir, "ak_rtsp_firmware_20261111_101010.squashfs")
	if err := os.WriteFile(w.img, w.imgData, 0o600); err != nil {
		t.Fatal(err)
	}
	m := md5.Sum(w.imgData)
	w.imgMD5 = hex.EncodeToString(m[:])
	tool := filepath.Join(w.dir, "flash_tool")
	if err := os.WriteFile(tool, []byte("#!/bin/sh\nflash_tool stand-in\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("AKAM_FLASH_TOOL", tool)
	t.Setenv("AKAM_FLASH_SCRIPTS_DIR", filepath.Join("..", "..", "..", "flash_scripts"))
	t.Setenv("AKAM_HWCHECK", filepath.Join("..", "..", "..", "hw_tests", "akam_hwcheck.sh"))
	t.Setenv("AKAM_PORT_SSH2", itoa(e.cam.SecondPort()))
	t.Setenv("AKAM_REFLASH_POLL_MS", "20")
	t.Setenv("AKAM_REFLASH_RETRY_MS", "5")
	return w
}

func (w *fw) unchanged(t *testing.T) {
	t.Helper()
	if !bytes.Equal(w.cam.Mtd7(), testfake.Mtd7Initial()) {
		t.Error("the camera's APP partition was modified")
	}
	if w.cam.FlashStarts() != 0 {
		t.Error("the flash worker was started")
	}
}

// closedSet is the closed list of command shapes the PC may send during an update: anything else
// (a reboot, a kill, a second flash path) fails the test.
var closedSet = []*regexp.Regexp{
	regexp.MustCompile(`^true$`),
	regexp.MustCompile(`^busy=0\nfor f in /tmp/akam_reflash/worker\.pid `),
	regexp.MustCompile(`^umask 077; cat > '/tmp/akam_reflash/(akam_reflash_ssh\.sh|flash_guard\.sh|flash_tool|image\.squashfs)'$`),
	regexp.MustCompile(`^sh '/tmp/akam_reflash/akam_reflash_ssh\.sh' (precheck|prepare --md5 [0-9a-f]{32})$`),
	regexp.MustCompile(`^sha256sum '/tmp/akam_reflash/(flash_tool|image\.squashfs)'$`),
	regexp.MustCompile(`^/tmp/akam_reflash/bin/busybox echo second-session-ok$`),
	regexp.MustCompile(`^/tmp/akam_reflash/bin/busybox sh '/tmp/akam_reflash/akam_reflash_ssh\.sh' start --md5 '[0-9a-f]{32}' --i-understand-no-uart$`),
	regexp.MustCompile(`^/tmp/akam_reflash/bin/busybox tail -c \+[0-9]+ '/tmp/akam_reflash/flash\.log'; printf '\\n=== akam-end-of-log\\n'; /tmp/akam_reflash/bin/busybox sh '/tmp/akam_reflash/akam_reflash_ssh\.sh' status \| /tmp/akam_reflash/bin/busybox grep '\^worker='$`),
	regexp.MustCompile(`^/tmp/akam_reflash/bin/busybox sh '/tmp/akam_reflash/akam_reflash_ssh\.sh' verify --md5 '[0-9a-f]{32}'$`),
}

func (w *fw) checkClosedSet(t *testing.T) {
	t.Helper()
	for _, c := range w.cam.Commands() {
		ok := false
		for _, re := range closedSet {
			if re.MatchString(c) {
				ok = true
			}
		}
		if !ok {
			t.Errorf("the PC sent a command outside the closed set: %q", c)
		}
	}
}

func (w *fw) update(stdin string, extra ...string) result {
	return w.run(stdin, append([]string{"update", "--image", w.img}, extra...)...)
}

func TestUpdateEndToEnd(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	r := w.update("y\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err,
		"Update Akam with a new image (SSH, no card)",
		"  Size 5242880 bytes (full APP partition), md5 "+w.imgMD5+".",
		"  Precheck: uploads two small scripts to the camera's RAM and checks it; nothing is written to flash.",
		"Updating over SSH has NO UART safety net.",
		"Updating: WRITES THE CAMERA FLASH",
		"Camera: 127.0.0.1 (route: SSH)",
		"This will write the new Akam image to the camera: it erases and rewrites the APP partition (/dev/mtd7).",
		"    | reflash_over_ssh: sha256 OK for image.squashfs",
		"    | reflash_over_ssh: read-back md5 of /dev/mtd7 = "+w.imgMD5+", equal to the image.",
		"    | reflash_over_ssh: VERIFIED. Now power-cycle BY HAND",
		"  The camera confirmed the written image (read-back md5 = "+w.imgMD5+").",
		"Flash done and verified. The wizard NEVER power-cycles the camera.",
		"Now power-cycle it BY HAND: unplug the USB-C power, wait 5 seconds, plug it back.",
		"Keep the card in for now; then run: ./akam step verify")
	contains(t, r.err, "Write the camera flash now? [y/N]")
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Fatal("the camera's APP partition does not hold the image")
	}
	if w.cam.FlashStarts() != 1 {
		t.Errorf("worker started %d times", w.cam.FlashStarts())
	}
	// the uploaded files are the ones the PC has
	if !bytes.Equal(w.cam.RamFile("image.squashfs"), w.imgData) || !bytes.Contains(w.cam.RamFile("flash_tool"), []byte("flash_tool stand-in")) {
		t.Error("the camera's RAM files differ from the PC's")
	}
	real, _ := os.ReadFile(filepath.Join("..", "..", "..", "flash_scripts", "akam_reflash_ssh.sh"))
	if !bytes.Equal(w.cam.RamFile("akam_reflash_ssh.sh"), real) {
		t.Error("the camera did not get the real akam_reflash_ssh.sh")
	}
	if w.readState("flash_md5") != w.imgMD5 {
		t.Errorf("flash_md5=%q", w.readState("flash_md5"))
	}
	w.checkClosedSet(t)
	// full output in the log file, progress bars folded
	b, _ := os.ReadFile(filepath.Join(w.state, "reflash.log"))
	if !strings.Contains(string(b), "RESULT: OK") || !strings.Contains(string(b), "camera: Erase: 100%") {
		t.Errorf("reflash.log: %s", b)
	}
	// no secret, no reboot anywhere in the program's own log
	// the order: precheck before the confirmation, the start after the sha256 checks
	cmds := w.cam.Commands()
	idx := func(sub string) int {
		for i, c := range cmds {
			if strings.Contains(c, sub) {
				return i
			}
		}
		return -1
	}
	if !(idx("precheck") < idx("image.squashfs'") && idx("sha256sum '/tmp/akam_reflash/image.squashfs'") < idx("prepare") && idx("prepare") < idx("second-session-ok") && idx("second-session-ok") < idx(" start ") && idx(" start ") < idx("verify --md5")) {
		t.Errorf("unexpected order of the camera commands: %q", cmds)
	}
}

func TestUpdateNotConfirmedWritesNothing(t *testing.T) {
	for _, in := range []string{"", "\n", "n\n", "N\n", "q\n", "no\n", "yes please\n", "maybe\n"} {
		w := newFlashWorld(t, testfake.FlashOptions{})
		r := w.update(in)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "ERROR: not confirmed.", "nothing was written.")
		w.unchanged(t)
		if len(w.cam.RamFile("image.squashfs")) != 0 || len(w.cam.RamFile("flash_tool")) != 0 {
			t.Errorf("input %q: the image or flash_tool was uploaded before the yes", in)
		}
		if w.readState("flash_md5") != "" {
			t.Error("flash_md5 recorded")
		}
	}
}

func TestUpdateRefusedWithoutTerminal(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	t.Setenv("AKAM_FORCE_TTY", "")
	r := w.update("y\ny\n") // the input must not be used as an answer
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "needs an interactive terminal and refuses to run without one.", "not confirmed.")
	w.unchanged(t)
}

func TestUpdateDryRun(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	r := w.run("", "--dry-run", "update", "--image", w.img)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "REHEARSAL (--dry-run)", "WOULD run the precheck of the SSH reflash (writes nothing), ask a yes/no (default No), then:",
		"It never reboots the camera.", "WOULD ask: Write the camera flash now? [y/N] (default: No)")
	if n := len(w.cam.Commands()); n != 0 {
		t.Errorf("a dry run sent %d commands: %q", n, w.cam.Commands())
	}
	w.unchanged(t)
	if w.readState("flash_md5") != "" {
		t.Error("a dry run recorded a flash")
	}
	// without --image either
	r = w.run("", "--dry-run", "update")
	expectRC(t, r, 0)
	if len(w.cam.Commands()) != 0 {
		t.Error("commands sent")
	}
}

func TestUpdateRefusesBadImages(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	expectRC(t, w.run("y\n", "update"), 1) // no image
	r := w.run("y\n", "update", "--image", filepath.Join(w.dir, "nope"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "image not found:")
	short := filepath.Join(w.dir, "short.squashfs")
	_ = os.WriteFile(short, []byte("tiny"), 0o600)
	r = w.run("y\n", "update", "--image", short)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the image is 4 bytes, expected exactly 5242880.", "do not flash it.")
	if len(w.cam.Commands()) != 0 {
		t.Errorf("commands sent for a bad image: %q", w.cam.Commands())
	}
	w.unchanged(t)
}

func TestUpdateAbortsWhenTheCameraSha256DiffersAfterUpload(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{CorruptUpload: true})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "reflash_over_ssh: ERROR: sha256 mismatch for image.squashfs", "nothing was written",
		"the SSH reflash failed or could not be verified.", "DO NOT power-cycle yet.")
	w.unchanged(t)
	for _, c := range w.cam.Commands() {
		if strings.Contains(c, "prepare") || strings.Contains(c, " start ") {
			t.Errorf("the flow went on after the mismatch: %s", c)
		}
	}
	if w.readState("flash_md5") != "" {
		t.Error("flash_md5 recorded after a failure")
	}
}

func TestUpdateAbortsOnReadbackMismatch(t *testing.T) {
	// the worker says OK, the independent read-back disagrees
	w := newFlashWorld(t, testfake.FlashOptions{VerifyLies: true})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "reflash_over_ssh: ERROR: independent read-back md5 is '00000000000000000000000000000000', expected "+w.imgMD5+". DO NOT power-cycle.",
		"DO NOT power-cycle yet.")
	notContains(t, r.out+r.err, "VERIFIED", "The camera confirmed", "Flash done and verified")
	if w.readState("flash_md5") != "" {
		t.Error("flash_md5 recorded after a failed verification")
	}
	// the worker's own failure (wrong data written on every attempt)
	w = newFlashWorld(t, testfake.FlashOptions{BadWrite: true})
	r = w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the flash worker reported:", "RESULT: FAILED after 3 attempts. DO NOT power cycle.", "DO NOT power-cycle")
	notContains(t, r.out+r.err, "VERIFIED")
	if w.cam.FlashStarts() != 1 {
		t.Errorf("starts %d", w.cam.FlashStarts())
	}
}

func TestUpdateInterruptedUpload(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{DropUploadAfter: 100000})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "reflash_over_ssh: ERROR: upload of image.squashfs failed", "DO NOT power-cycle yet.")
	w.unchanged(t)
	if bytes.Equal(w.cam.RamFile("image.squashfs"), w.imgData) {
		t.Error("the truncated upload was kept as complete")
	}
	for _, c := range w.cam.Commands() {
		if strings.Contains(c, "prepare") || strings.Contains(c, " start ") {
			t.Errorf("the flow went on after the interrupted upload: %s", c)
		}
	}
}

func TestUpdatePrecheckFailureStopsBeforeTheQuestion(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{PrecheckFails: true})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the precheck failed (see above).", "nothing was written to the camera.", "reflash_over_ssh: ERROR: camera pre-check failed, nothing was written")
	notContains(t, r.err, "Write the camera flash now?")
	w.unchanged(t)
}

func TestUpdateStopsWhenTheSecondDropbearIsMissing(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{NoSecondDropbear: true})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "reflash_over_ssh: ERROR: cannot log in on port "+itoa(w.cam.SecondPort())+" (the RAM Dropbear): nothing was written, aborting before the flash")
	w.unchanged(t)
}

func TestUpdateSurvivesADroppedPollAndReportsADeadWorker(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{DropPollOnce: true})
	r := w.update("y\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "reflash_over_ssh: poll failed (1 in a row); the camera may be busy or the session dropped, retrying",
		"VERIFIED. Now power-cycle BY HAND")
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Error("not flashed")
	}
	w = newFlashWorld(t, testfake.FlashOptions{WorkerDies: true})
	r = w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no result from the flash worker (worker dead: 2", "DO NOT power-cycle.")
}

func TestUpdateRefusesWhileAWorkerRuns(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{WorkerBusy: true})
	r := w.update("y\n")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "a flash worker is still running or starting in /tmp/akam_reflash on the camera: nothing touched.", "do not power-cycle.")
	w.unchanged(t)
}

func TestUpdateWithoutLoginDoesNothing(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	r := w.run("y\n", "--identity", filepath.Join(w.dir, "nokey"), "update", "--image", w.img)
	expectRC(t, r, 1)
	if len(w.cam.Commands()) != 0 {
		t.Error("commands sent without a login")
	}
	w.unchanged(t)
}

// The flash is also refused when the SSH host key changed: nothing is uploaded to an impostor.
func TestUpdateRefusesAChangedHostKey(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	expectRC(t, w.update("y\n"), 0)
	w.cam.RotateHostKey()
	n := len(w.cam.Commands())
	args := []string{"--no-color", "--state-dir", w.state, "--ip", "127.0.0.1", "--identity", w.key, "--trust-new-host-key", w.cam.HostFingerprint(), "update", "--image", w.img}
	r := w.runRaw("y\ny\n", args...)
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "REMOTE HOST IDENTIFICATION HAS CHANGED")
	if len(w.cam.Commands()) != n {
		t.Error("commands reached a camera whose key changed")
	}
}

// ---------------------------------------------------------------- restore

func (w *fw) backupDir(t *testing.T, sums bool) string {
	t.Helper()
	d := filepath.Join(w.dir, "akam-backup")
	if err := os.MkdirAll(d, 0o700); err != nil {
		t.Fatal(err)
	}
	var lines []string
	for i := 0; i < 8; i++ {
		name := fmt.Sprintf("mtdblock%d.bin", i)
		data := []byte(fmt.Sprintf("partition %d\n", i))
		if i == 7 {
			data = bytes.Repeat([]byte("stock APP"), 5242880/9+1)[:5242880]
		}
		if err := os.WriteFile(filepath.Join(d, name), data, 0o600); err != nil {
			t.Fatal(err)
		}
		h := sha256.Sum256(data)
		lines = append(lines, hex.EncodeToString(h[:])+"  "+name)
	}
	if sums {
		_ = os.WriteFile(filepath.Join(d, "SHA256SUMS"), []byte(strings.Join(lines, "\n")+"\n"), 0o600)
	}
	return d
}

func TestRestoreOverSSH(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	bk := w.backupDir(t, true)
	w.writeState("camera_kind=akam\n")
	app, _ := os.ReadFile(filepath.Join(bk, "mtdblock7.bin"))
	r := w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 0)
	m := md5.Sum(app)
	md5s := hex.EncodeToString(m[:])
	contains(t, r.out+r.err, "Restore the original (stock) APP from your backup",
		"Risk: WRITES the camera flash; a power cut can brick it.",
		"Stock APP sha256: ", "Akam runs and SSH works.", "Route: ssh",
		"Restoring: WRITES THE CAMERA FLASH",
		"This will put the ORIGINAL (stock) APP back on the camera",
		"Restore written and verified (read-back md5 = "+md5s+").",
		"Power-cycle the camera BY HAND", "CONFIG was not touched, so the Wi-Fi settings stay.")
	contains(t, r.err, "Restore over SSH (no card needed)? [no = use the maintenance telnet and the card] [y/N]", "Write the camera flash now? [y/N]")
	if !bytes.Equal(w.cam.Mtd7(), app) {
		t.Error("the stock APP was not written")
	}
	w.checkClosedSet(t)
}

func TestRestoreRefusals(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.writeState("camera_kind=akam\n")
	r := w.run("y\ny\n", "restore")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no backup found.", "pass --backup-dir DIR (it must hold mtdblock7.bin).")

	bk := w.backupDir(t, true)
	// a damaged backup
	if err := os.WriteFile(filepath.Join(bk, "mtdblock3.bin"), []byte("tampered"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "SHA256SUMS does not match the files in "+bk+".", "this backup is damaged: use your second copy.")
	// the user says no to the SSH route, or no to the flash
	bk = w.backupDir(t, true)
	_ = os.WriteFile(filepath.Join(bk, "mtdblock3.bin"), []byte("partition 3\n"), 0o600)
	for _, in := range []string{"n\n", "\n", "y\nn\n", "y\n\n"} {
		r = w.run(in, "restore", "--backup-dir", bk)
		expectRC(t, r, 1)
		w.unchanged(t)
	}
	// a camera not known to run Akam: the telnet route is not native yet
	w.writeState("camera_kind=stock\n")
	r = w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not in the native program yet")
	w.unchanged(t)
	// dry run
	n := len(w.cam.Commands())
	r = w.run("", "--dry-run", "restore", "--backup-dir", bk)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "WOULD check "+bk+"/mtdblock7.bin", "no reboot is ever sent")
	if len(w.cam.Commands()) != n {
		t.Error("a dry run sent commands")
	}
	// no terminal
	w.writeState("camera_kind=akam\n")
	t.Setenv("AKAM_FORCE_TTY", "")
	r = w.run("y\ny\n", "restore", "--backup-dir", bk)
	expectRC(t, r, 1)
	w.unchanged(t)
}

// ---------------------------------------------------------------- verify

// verifyWorld writes a state where every step before verify is done and valid (source mode: no kit file).
func (w *fw) verifyWorld(t *testing.T) {
	t.Helper()
	bk := w.backupDir(t, true)
	app := filepath.Join(bk, "mtdblock7.bin")
	imgSHA := sha256Of(t, w.img)
	appSHA := sha256Of(t, app)
	st := "mode=source\ncamera_ip=127.0.0.1\ncamera_kind=akam\nbackup_dir=" + bk + "\nbackup_sha=" + appSHA +
		"\nimage_path=" + w.img + "\nimage_sha=" + imgSHA + "\nimage_kit_sha=source\nimage_backup_sha=" + appSHA +
		"\nflash_sd_image_sha=" + imgSHA + "\n"
	for _, s := range []string{"doctor", "get_kit", "prepare_backup_sd", "find_camera", "backup", "image", "prepare_flash_sd", "flash"} {
		st += "step_" + s + "_done=2026-11-11T00:00:00Z\n"
	}
	w.writeState(st)
	if err := os.WriteFile(filepath.Join(w.state, "state.dry"), []byte(st), 0o600); err != nil { // the rehearsal's own copy
		t.Fatal(err)
	}
	t.Setenv("AKAM_WAIT_SECS", "2")
	t.Setenv("AKAM_PORT_FTP", freeClosedPort(t))
	t.Setenv("AKAM_PORT_TELNET23", freeClosedPort(t))
	t.Setenv("AKAM_PORT_HTTP", freeClosedPort(t))
}

func sha256Of(t *testing.T, p string) string {
	t.Helper()
	h, err := hashOf(p)
	if err != nil {
		t.Fatal(err)
	}
	return h
}

func TestVerifyEndToEnd(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	// Enter (power cycled), then "no" to the stream password and "no" to ONVIF
	r := w.run("\nn\nn\n", "verify")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Step 9/9 - Verify the result",
		"  Waiting for the camera to answer on SSH: 127.0.0.1 port "+itoa(w.cam.SSHPort()),
		"  SSH with your key works.",
		"  Copying akam_hwcheck.sh to the camera's RAM (/tmp, nothing is installed) ...",
		"    PASS ssh key login", "    PASS ak_rtsp running", "    INFO RTSP auth: off",
		"  Open ports: ",
		"  Checks passed with the card in.",
		"  /etc/config/wpa_supplicant.conf holds a Wi-Fi network: the camera keeps its network without the card.",
		"    1. unplug the camera's USB-C power  2. take the microSD card out  3. plug the power back in.",
		"  Verified: SSH works, only 22, 554 and 8091 are open, telnet and FTP are off.",
		"  Optional: protect the video stream with a password.",
		"  Stream left open.", "  ONVIF left off.")
	contains(t, r.err, "Press Enter when the camera is powered again (or q to stop):")
	if w.readState("step_verify_done") == "" {
		t.Error("verify not marked done")
	}
	if !bytes.Contains(w.cam.CamFile("akam_hwcheck.sh"), []byte("akam_hwcheck")) {
		t.Error("the camera did not receive akam_hwcheck.sh")
	}
	if !bytes.Equal(w.cam.Mtd7(), testfake.Mtd7Initial()) || w.cam.FlashStarts() != 0 {
		t.Error("verify wrote the flash")
	}
}

func TestVerifyFailures(t *testing.T) {
	t.Run("hwcheck fails", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{HwcheckFails: true})
		w.verifyWorld(t)
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "    FAIL ak_rtsp is not running", "akam_hwcheck.sh reported a failure (lines above).", "do not remove the card yet; read docs/akam_v1_test_plan.md.")
		if w.readState("step_verify_done") != "" {
			t.Error("marked done")
		}
	})
	t.Run("no saved wifi", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{WifiVerdict: "NO_CARD_CONFIG"})
		w.verifyWorld(t)
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "the camera has no saved Wi-Fi (/etc/config/wpa_supplicant.conf)", "The step is NOT done: do not remove the card.")
		notContains(t, r.out+r.err, "Now the real test: without the card.")
	})
	t.Run("ONVIF port open while ONVIF is off", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{})
		w.verifyWorld(t)
		t.Setenv("AKAM_PORT_HTTP", itoa(listen(t)))
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "should be closed (ONVIF is off) and is open.", "unexpected port state.")
	})
	t.Run("telnet still open without the card", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{})
		w.verifyWorld(t)
		t.Setenv("AKAM_PORT_TELNET", itoa(listen(t)))
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "Checks passed with the card in.", "should be closed (telnet/FTP must be off without the card) and is open.", "port check failed without the card.")
	})
	t.Run("stopped at the prompt", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{})
		w.verifyWorld(t)
		r := w.run("q\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "stopped.")
		r = w.run("", "verify") // no input at all
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "no input.")
	})
	t.Run("camera silent", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{})
		w.verifyWorld(t)
		t.Setenv("AKAM_PORT_SSH", freeClosedPort(t))
		t.Setenv("AKAM_WAIT_SECS", "1")
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "No answer on port", "the camera does not answer on SSH.")
	})
	t.Run("blocked before the flash step", func(t *testing.T) {
		w := newFlashWorld(t, testfake.FlashOptions{})
		r := w.run("\n", "verify")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "step 'verify' needs these steps first: flash")
	})
}

func TestVerifyDryRun(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	r := w.run("", "--dry-run", "verify")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "WOULD wait for the camera, check SSH with your key", "telnet 24 and FTP 21 must then be closed. Only then is the step marked done.",
		"WOULD ask: Protect the video stream with a username/password? [y/N] (default: No)",
		"WOULD run over SSH on the camera: /usr/sbin/akam_onvif.sh enable")
	if len(w.cam.Commands()) != 0 {
		t.Error("a dry run sent commands")
	}
}
