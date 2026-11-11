//go:build akamtest

package cli_test

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The SD card commands run against a regular file that plays the card (the helper's image-file mode,
// compiled only under -tags akamtest) and a synthetic toolkit with its own pin. No block device, no
// sudo, no network.

type sdWorld struct {
	*env
	img  string
	be   *sdcard.ImageBackend
	tk   string
	tree map[string][]byte
}

const cardSize = 192 << 20

func newSDWorld(t *testing.T) *sdWorld {
	t.Helper()
	e := newEnv(t)
	w := &sdWorld{env: e}
	w.tree = map[string][]byte{
		"hack.sh":                      []byte("#!/bin/sh\nSCRIPTS_DIR=/tmp/sd/custom/scripts"),
		"custom/configs/hack.conf":     []byte("TELNET=0\nTELNET_PORT=23\nOFFLINE=0\n"),
		"custom/configs/wifi.conf":     []byte("MODE=ap\n"),
		"custom/scripts/entrypoint.sh": []byte("#!/bin/sh\necho start\n"),
		"custom/scripts/ftp.sh":        []byte("echo ftp\n"),
		"logs/.gitkeep":                {},
	}
	w.tk = filepath.Join(t.TempDir(), "toolkit")
	for p, b := range w.tree {
		fp := filepath.Join(w.tk, "sd_card", p)
		os.MkdirAll(filepath.Dir(fp), 0o755)
		os.WriteFile(fp, b, 0o644)
	}
	oldPin := sdcard.PinTreeSHA
	sdcard.PinTreeSHA = sdcard.TreeHash(w.tree)
	t.Cleanup(func() { sdcard.PinTreeSHA = oldPin })
	t.Setenv("AKAM_TOOLKIT_DIR", w.tk)
	t.Setenv("AKAM_WIFI_SSID", "Home Net")
	t.Setenv("AKAM_WIFI_PASS", "sup3r-secret-wifi")
	w.img = filepath.Join(t.TempDir(), "card.img")
	f, _ := os.Create(w.img)
	f.Truncate(cardSize)
	f.Close()
	w.be = &sdcard.ImageBackend{Path: w.img, Model: "Test Reader"}
	e.sd = w.be
	e.writeState("step_doctor_done=2026-01-01T00:00:00Z\n")
	return w
}

// run sets the Wi-Fi environment again first: the program removes it from its environment once read.
func (w *sdWorld) run(stdin string, args ...string) result {
	os.Setenv("AKAM_WIFI_SSID", "Home Net")
	os.Setenv("AKAM_WIFI_PASS", "sup3r-secret-wifi")
	return w.env.run(stdin, args...)
}

func (w *sdWorld) cardBytes(t *testing.T) []byte {
	b, err := os.ReadFile(w.img)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func (w *sdWorld) untouched(t *testing.T) {
	t.Helper()
	if n := bytes.Count(w.cardBytes(t), []byte{0}); n != cardSize {
		t.Errorf("the card was written to (%d non-zero bytes)", cardSize-n)
	}
	if w.be.Prepared != 0 || w.be.Wiped != 0 {
		t.Errorf("destructive calls: prepared %d wiped %d", w.be.Prepared, w.be.Wiped)
	}
}

func (w *sdWorld) allText(t *testing.T) string {
	var sb strings.Builder
	filepath.Walk(w.state, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() {
			b, _ := os.ReadFile(p)
			sb.Write(b)
		}
		return nil
	})
	return sb.String()
}

func TestPrepareBackupSDWritesAVerifiedCard(t *testing.T) {
	w := newSDWorld(t)
	r := w.run("y\n", "prepare-backup-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "Step 3/9 - Prepare the backup microSD card",
		"  Removable disks that can be used as a microSD card (system disks are never listed):",
		"Test Reader", "sd_card: VERIFY: OK",
		"  Backup card ready. Put it in the camera, power the camera on, then run: ./akam")
	contains(t, r.err, "Erase "+w.img+" now? [y/N]")
	contains(t, r.out, "Device: "+w.img, "Model: Test Reader  |  Size: 0.2 GB")
	f, _ := os.Open(w.img)
	defer f.Close()
	card, err := sdcard.OpenCard(f)
	if err != nil {
		t.Fatal(err)
	}
	if lines, bad := sdcard.VerifyTree(card, sdcard.VerifySpec{Mode: sdcard.ModeBackup, Toolkit: sdcard.ToolkitHashes(w.tree)}); bad != 0 {
		t.Errorf("verify: %v", lines)
	}
	wifi, _ := card.ReadFile("custom/configs/wifi.conf")
	if !strings.Contains(string(wifi), "PASS='sup3r-secret-wifi'") || !strings.Contains(string(wifi), "SSID='Home Net'") {
		t.Errorf("wifi.conf: %s", wifi)
	}
	if w.readState("backup_sd_device") != w.img || w.readState("step_prepare_backup_sd_done") == "" {
		t.Error("state not updated")
	}
	// the password is only on the card: not in the state, the log, the output, nor the environment
	for name, text := range map[string]string{"state and log": w.allText(t), "stdout": r.out, "stderr": r.err} {
		if strings.Contains(text, "sup3r-secret-wifi") {
			t.Errorf("the Wi-Fi password leaked into the %s", name)
		}
	}
	if os.Getenv("AKAM_WIFI_PASS") != "" {
		t.Error("AKAM_WIFI_PASS is still in the environment")
	}
}

func TestPrepareBackupSDDefaultsToNo(t *testing.T) {
	w := newSDWorld(t)
	for _, in := range []string{"", "\n", "n\n", "N\n", "q\n", "\x1b\n", "maybe\n", "no\n"} {
		r := w.run(in, "prepare-backup-sd", "--device", w.img)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "not confirmed.", "nothing was changed.")
		w.untouched(t)
		if w.readState("step_prepare_backup_sd_done") != "" {
			t.Fatal("recorded after a no")
		}
	}
}

func TestSDCommandsRefuseWithoutATerminal(t *testing.T) {
	w := newSDWorld(t)
	t.Setenv("AKAM_FORCE_TTY", "")
	for _, c := range [][]string{{"prepare-backup-sd", "--device", w.img}, {"wipe-sd", "--device", w.img}} {
		r := w.run("y\ny\ny\n", c...)
		expectRC(t, r, 1)
		contains(t, r.err, "needs an interactive terminal and refuses to run without one.")
		w.untouched(t)
	}
}

func TestSDRefusesTheWrongDeviceAndSystemDisks(t *testing.T) {
	w := newSDWorld(t)
	// a device that is not in the list (a system disk, a partition, another disk)
	for _, dev := range []string{"/dev/sda", "/dev/sdb1", "/dev/nvme0n1", "/dev/loop0", w.img + "x"} {
		r := w.run("y\ny\n", "prepare-backup-sd", "--device", dev)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, dev+" is not in the list of usable cards.", "system disks, partitions and non-removable disks are refused on purpose.")
		w.untouched(t)
	}
	// the guards of the real backend refuse it (size, system mounts, ...): see internal/sdcard; here the
	// backend reports a system disk and no card is offered at all
	w.be.Refuse = "/dev/sdb carries the system path /home"
	r := w.run("y\ny\n", "prepare-backup-sd", "--device", w.img)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no microSD card found.")
	w.untouched(t)
	r = w.run("y\ny\n", "wipe-sd", "--device", w.img)
	expectRC(t, r, 1)
	w.untouched(t)
}

func TestSDAdministratorRightsAreAskedAndDefaultToNo(t *testing.T) {
	w := newSDWorld(t)
	w.be.Elevate = true
	for _, in := range []string{"y\n", "y\n\n", "y\nn\n"} {
		r := w.run(in, "prepare-backup-sd", "--device", w.img)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "administrator rights not granted.")
		w.untouched(t)
	}
	r := w.run("y\ny\n", "prepare-backup-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.err, "This step needs administrator rights to write the card. Continue? [y/N]")
}

func TestSDToolkitTamperingIsRefusedBeforeAnyWrite(t *testing.T) {
	w := newSDWorld(t)
	os.WriteFile(filepath.Join(w.tk, "sd_card", "custom", "scripts", "ftp.sh"), []byte("evil\n"), 0o644)
	r := w.run("y\n", "prepare-backup-sd", "--device", w.img)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "sd_card tree hash mismatch", "the toolkit cannot be used")
	w.untouched(t)
}

func TestSDDryRunTouchesNothing(t *testing.T) {
	w := newSDWorld(t)
	os.WriteFile(filepath.Join(w.state, "state.dry"), []byte("step_doctor_done=2026-01-01T00:00:00Z\n"), 0o600)
	for _, c := range [][]string{{"prepare-backup-sd"}, {"wipe-sd"}} {
		r := w.run("", append([]string{"--dry-run"}, c...)...)
		expectRC(t, r, 0)
		w.untouched(t)
	}
}

func TestWipeSDZeroFillsAndForgetsTheDevice(t *testing.T) {
	w := newSDWorld(t)
	expectRC(t, w.run("y\n", "prepare-backup-sd", "--device", w.img), 0)
	if !bytes.Contains(w.cardBytes(t), []byte("sup3r-secret-wifi")) {
		t.Fatal("test premise")
	}
	r := w.run("n\n", "wipe-sd", "--device", w.img)
	expectRC(t, r, 1)
	if !bytes.Contains(w.cardBytes(t), []byte("sup3r-secret-wifi")) || w.be.Wiped != 0 {
		t.Error("wiped after a no")
	}
	r = w.run("y\n", "wipe-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "ZERO-FILL "+w.img, "  Card wiped: it no longer holds your Wi-Fi password.")
	if bytes.Count(w.cardBytes(t), []byte{0}) != cardSize {
		t.Error("the wipe left non-zero bytes")
	}
	if w.readState("backup_sd_device") != "" {
		t.Error("the wiped card is still recorded")
	}
}

func TestPrepareFlashSDWritesTheFlashFiles(t *testing.T) {
	w := newSDWorld(t)
	dir := t.TempDir()
	img := bytes.Repeat([]byte("akam image block 0123456789abcdef"), 5242880/33+1)[:5242880]
	imgPath := filepath.Join(dir, "ak_rtsp_firmware_20261111_101010.squashfs")
	os.WriteFile(imgPath, img, 0o600)
	tool := filepath.Join(dir, "flash_tool")
	os.WriteFile(tool, []byte("fake flash tool"), 0o755)
	t.Setenv("AKAM_FLASH_TOOL", tool)
	bk := filepath.Join(dir, "backup")
	os.MkdirAll(bk, 0o755)
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		os.WriteFile(filepath.Join(bk, fmt.Sprintf("mtdblock%d.bin", i)), testfake.StockMtdContent(i), 0o600)
		fmt.Fprintf(&sums, "%s  mtdblock%d.bin\n", shaOf(testfake.StockMtdContent(i)), i)
	}
	os.WriteFile(filepath.Join(bk, "SHA256SUMS"), []byte(sums.String()), 0o600)
	app, isha := shaOf(testfake.StockMtdContent(7)), shaOf(img)
	st := "mode=source\nimage_path=" + imgPath + "\nimage_sha=" + isha + "\nimage_kit_sha=source\nimage_backup_sha=" + app + "\nbackup_dir=" + bk + "\nbackup_sha=" + app + "\n"
	for _, s := range []string{"doctor", "get_kit", "prepare_backup_sd", "find_camera", "backup", "image"} {
		st += "step_" + s + "_done=2026-01-01T00:00:00Z\n"
	}
	w.writeState(st)
	pub, err := os.ReadFile(w.key + ".pub")
	if err != nil {
		t.Fatal(err)
	}
	r := w.run("n\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 1)
	w.untouched(t)
	r = w.run("y\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out, "Step 7/9 - Prepare the flash microSD card", "sd_card: VERIFY: OK", "  Flash card ready. Remember: wifi.conf on the card holds your Wi-Fi password in clear text (./akam wipe-sd erases it).")
	f, _ := os.Open(w.img)
	defer f.Close()
	card, err := sdcard.OpenCard(f)
	if err != nil {
		t.Fatal(err)
	}
	got, _ := card.ReadFile("custom_firmware/ak_rtsp_firmware_20261111_101010.squashfs")
	if !bytes.Equal(got, img) {
		t.Error("image differs on the card")
	}
	if b, _ := card.ReadFile("original_firmware/mtdblock7.bin"); !bytes.Equal(b, testfake.StockMtdContent(7)) {
		t.Error("stock APP differs on the card")
	}
	if k, _ := card.ReadFile("akam/authorized_keys"); !bytes.Equal(k, pub) {
		t.Errorf("authorized_keys %q", k)
	}
	for _, n := range sdcard.FlashScriptNames {
		b, _ := card.ReadFile(n)
		want, _ := assets.Script(n)
		if !bytes.Equal(b, want) {
			t.Errorf("%s on the card is not the embedded script", n)
		}
	}
	if !card.Exists("AKAM_MAINTENANCE") || w.readState("flash_sd_device") != w.img || w.readState("flash_sd_image_sha") != isha {
		t.Error("state or marker")
	}
	// a private key is never put on the card
	priv := filepath.Join(dir, "id")
	os.WriteFile(priv, []byte("-----BEGIN OPENSSH "+"PRIVATE KEY-----\nabc\n"), 0o600)
	w.be.Prepared = 0
	r = w.run("y\n", "prepare-flash-sd", "--device", w.img, "--pubkey", priv)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is a PRIVATE key. Never put it on the card")
	if w.be.Prepared != 0 {
		t.Error("prepared with a private key")
	}
}
