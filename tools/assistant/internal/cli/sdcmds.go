package cli

// prepare-backup-sd, prepare-flash-sd and wipe-sd: the microSD card steps (tools/sd/sd_card.sh and
// steps_host.sh). The card layer (internal/sdcard) lists eligible cards without privilege, and writes
// the MBR + FAT32 + files directly on the device through `akam sd-helper`, re-executed with sudo or
// pkexec only for that step, after the red-box confirmation and one more yes/no.

import (
	"crypto/md5"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func md5sum(b []byte) [16]byte { return md5.Sum(b) }

// ignoreSignals makes Ctrl-C, hangup and TERM harmless while a flash is being written.
//
// It catches the signals into a channel nobody reads (Notify) instead of Ignore/Reset: Ignore and Reset
// undo every Notify of the program, including the handler of the full-screen session, which would
// then be gone for the rest of the session.
func ignoreSignals() func() {
	sigs := platform.SignalsToIgnore()
	ch := make(chan os.Signal, 1) // a full channel drops the signal: that is the point
	signal.Notify(ch, sigs...)
	return func() { signal.Stop(ch) }
}

func (a *App) sd() sdcard.Backend {
	if a.Hooks.SD != nil {
		return a.Hooks.SD
	}
	return sdcard.NewSystem()
}

func (a *App) toolkitDir() string {
	if d := os.Getenv("AKAM_TOOLKIT_DIR"); d != "" {
		return d
	}
	h, _ := os.UserHomeDir()
	return filepath.Join(h, "akam-sdprep", "toolkit")
}

func gbString(size int64) string {
	return fmt.Sprintf("%d.%d GB", size/1000000000, size/100000000%10)
}

func mountsString(c *sdcard.Candidate) string {
	var m []string
	for _, x := range c.Mounts {
		m = append(m, x.Node+":"+x.Point)
	}
	return strings.Join(m, ",")
}

// sdChooseDevice is sd_choose_device: lists the eligible cards, then asks. Returns the checked card
// (nil in a rehearsal) and its description for the confirmation box.
func (a *App) sdChooseDevice() (*sdcard.Candidate, string, error) {
	be := a.sd()
	a.UI.Say("  Removable disks that can be used as a microSD card (system disks are never listed):")
	cands := be.List()
	for _, c := range cands {
		m := mountsString(&c)
		if m == "" {
			m = "not mounted"
		}
		a.UI.Say(fmt.Sprintf("%-14s %8d MiB  %-24s %-4s %s", c.Dev, c.Size/1048576, c.Model, c.Tran, m))
	}
	if len(cands) == 0 && !a.UI.Dry() {
		return nil, "", a.UI.Fail("no microSD card found.", "insert the card (USB reader or built-in SD slot), wait a few seconds, then run this step again.")
	}
	dev := a.Opt.Device
	if dev == "" && !a.UI.Dry() && len(cands) > 0 && a.UI.Interactive() {
		items := make([]string, len(cands))
		for i, c := range cands {
			mm := ""
			if m := mountsString(&c); m != "" {
				mm = "  mounted: " + m
			}
			model := c.Model
			if model == "" {
				model = "unknown model"
			}
			items[i] = fmt.Sprintf("%s  %s  %s  (%s)%s", c.Dev, gbString(c.Size), model, c.Tran, mm)
		}
		if i, err := a.UI.Pick("Which disk is your microSD card?", items); err == nil && i >= 0 && i < len(cands) {
			dev = cands[i].Dev
		} else {
			return nil, "", a.UI.Fail("no card chosen.", "run this step again and pick your microSD card, or pass --device /dev/sdX.")
		}
	}
	if dev == "" {
		if a.UI.Dry() {
			dev = "/dev/sdX"
		} else {
			d, err := a.UI.Prompt("  Device of your microSD card from the list above (for example /dev/sdb), or empty to cancel:", "")
			if err != nil {
				return nil, "", ui.ErrFailed
			}
			dev = d
		}
	}
	if dev == "" || strings.ContainsAny(dev, " \t\n") {
		return nil, "", a.UI.Fail("no valid device given.", "pass --device /dev/sdX (the whole disk, not a partition).")
	}
	if a.UI.Dry() {
		return nil, "Model and size: see the list above", nil
	}
	c, err := be.Check(dev)
	if err != nil {
		a.UI.Err("refusing: " + err.Error())
		return nil, "", a.UI.Fail(fmt.Sprintf("%s is not in the list of usable cards.", dev),
			"pick a device from the list above; system disks, partitions and non-removable disks are refused on purpose.")
	}
	m := mountsString(c)
	if m == "" {
		m = "nothing"
	}
	model := c.Model
	if model == "" {
		model = "unknown"
	}
	info := fmt.Sprintf("Model: %s  |  Size: %s  |  Connected by: %s  |  Mounted: %s", model, gbString(c.Size), c.Tran, m)
	return c, info, nil
}

// sdGetWifi is sd_get_wifi: SSID and password from --ssid, the environment (read once, then removed) or a prompt.
func (a *App) sdGetWifi() (ssid, pass string, err error) {
	ssid, pass = a.Opt.SSID, a.envSecret("AKAM_WIFI_PASS")
	if ssid == "" {
		ssid = os.Getenv("AKAM_WIFI_SSID")
	}
	os.Unsetenv("AKAM_WIFI_SSID")
	a.Secrets.Add(pass)
	if a.UI.Dry() {
		return ssid, pass, nil
	}
	if ssid == "" && pass == "" {
		if s, p, ok := a.cfgWifiOffer(); ok { // the Wi-Fi saved in the config chosen for this camera
			return s, p, nil
		}
	}
	if ssid == "" {
		if ssid, err = a.UI.Prompt("  Wi-Fi network name (SSID, 2.4 GHz):", ""); err != nil {
			return "", "", ui.ErrFailed
		}
	}
	if pass == "" {
		if pass, err = a.UI.Secret("  Wi-Fi password (never saved by the wizard):"); err != nil {
			return "", "", ui.ErrFailed
		}
		a.Secrets.Add(pass)
	}
	if ssid == "" || pass == "" {
		return "", "", a.UI.Fail("Wi-Fi name and password are required.", "the camera needs them to join your network.")
	}
	a.UI.Say("  Wi-Fi settings go to the card writer over a pipe (never on a command line or in the environment).")
	return ssid, pass, nil
}

// sdConfirmErase is sd_confirm_erase: one yes/no (default No) in a red box that names the device path, its
// model and size, then (for a real card) the administrator-rights question. what: "erase" or "wipe".
func (a *App) sdConfirmErase(c *sdcard.Candidate, info, what string) error {
	if err := a.UI.RequireTTY("erasing the microSD card"); err != nil {
		return ui.ErrFailed
	}
	dev := "/dev/sdX"
	if c != nil {
		dev = c.Dev
	}
	var title, q string
	var lines []string
	if what == "wipe" {
		title, q = "ZERO-FILL "+dev, fmt.Sprintf("Zero-fill %s now?", dev)
		lines = append(lines, fmt.Sprintf("Every byte of %s will be overwritten with zeros (it takes a few minutes). Nothing can be recovered.", dev))
	} else {
		title, q = "ERASE "+dev, fmt.Sprintf("Erase %s now?", dev)
		lines = append(lines, fmt.Sprintf("Everything on %s will be ERASED: all partitions and files. This cannot be undone.", dev))
	}
	lines = append(lines, "Device: "+dev, info, "This must be your microSD card, not another disk. If in doubt, answer No.",
		"Writing the card may need administrator rights (sudo or pkexec, asked once more); the disk is checked again right before anything is written.")
	if ok, _ := a.UI.Danger(title, q, lines...); !ok {
		return a.UI.Fail("not confirmed.", "nothing was changed.")
	}
	if c != nil && a.sd().NeedsElevation(c) {
		if ok, _ := a.UI.AskYN("This step needs administrator rights to write the card. Continue?"); !ok {
			return a.UI.Fail("administrator rights not granted.", "nothing was changed.")
		}
	}
	return nil
}

func (a *App) showVerify(lines []string) {
	for _, l := range lines {
		a.UI.Say("sd_card: " + l)
	}
}

// ---------------------------------------------------------------- prepare-backup-sd

func (a *App) cmdPrepareBackupSD() error {
	if err := a.stepNeed("prepare-backup-sd"); err != nil {
		return err
	}
	a.stepBanner("prepare-backup-sd")
	a.UI.Say("  This card boots the camera into a temporary rescue shell (Telnet on port 24) so that")
	a.UI.Say("  you can copy the original firmware. It holds your Wi-Fi password IN CLEAR TEXT:")
	a.UI.Say(fmt.Sprintf("  treat the card as secret and wipe it when you are done (%s wipe-sd).", Prog))
	if err := a.UI.RequireTTY("preparing the backup card"); err != nil {
		return ui.ErrFailed
	}
	tk := a.toolkitDir()
	a.UI.Say(fmt.Sprintf("  The SD tool uses the pinned LSC toolkit in %s (downloaded once if missing, then", tk))
	a.UI.Say("  checked against a fixed commit and file hashes).")
	c, info, err := a.sdChooseDevice()
	if err != nil {
		return err
	}
	ssid, pass, err := a.sdGetWifi()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD check the toolkit, ask the red-box yes/no, then write an MBR + FAT32 card (mode backup) and verify it.")
		_ = a.St.Set("backup_sd_device", "/dev/sdX")
		_ = a.St.MarkDone("prepare-backup-sd")
		return nil
	}
	toolkit, err := a.loadToolkit(tk)
	if err != nil {
		return err
	}
	files, err := sdcard.BuildPayload(sdcard.PayloadInput{Mode: sdcard.ModeBackup, Toolkit: toolkit, SSID: ssid, Pass: pass})
	if err != nil {
		return a.UI.Fail("preparing the card failed: "+err.Error(), "nothing else was changed; fix the problem above and retry.")
	}
	if err := a.sdConfirmErase(c, info, "erase"); err != nil {
		return err
	}
	lines, err := a.sd().Prepare(c, files, sdcard.VerifySpec{Mode: sdcard.ModeBackup, Toolkit: sdcard.ToolkitHashes(toolkit)})
	a.showVerify(lines)
	if err != nil {
		a.UI.Err(err.Error())
		return a.UI.Fail("preparing the card failed.", "nothing else was changed; fix the problem above and retry.")
	}
	_ = a.St.Set("backup_sd_device", c.Dev)
	_ = a.St.MarkDone("prepare-backup-sd")
	a.UI.Good(fmt.Sprintf("  Backup card ready. Put it in the camera, power the camera on, then run: %s", Prog))
	return nil
}

func (a *App) loadToolkit(dir string) (map[string][]byte, error) {
	tk, err := sdcard.EnsureToolkit(a.Ctx, a.Hooks.HTTP, dir, func(s string) { a.UI.Say("sd_card: " + s) })
	if err != nil {
		return nil, a.UI.Fail("the toolkit cannot be used: "+err.Error(), "nothing was changed; check the network, or put a checkout of the pinned toolkit in "+dir+".")
	}
	return tk, nil
}

// ---------------------------------------------------------------- prepare-flash-sd

func pubkeyCheck(a *App, path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		a.UI.Err("public key not found: " + path)
		return ui.ErrFailed
	}
	if strings.Contains(string(b), "PRIVATE KEY") {
		a.UI.Err(path + " is a PRIVATE key. Never put it on the card; use the .pub file.")
		return ui.ErrFailed
	}
	if err := sdcard.CheckPubKey(b); err != nil {
		a.UI.Err(fmt.Sprintf("%s: %v", path, err))
		return ui.ErrFailed
	}
	return nil
}

// sshChoosePubkey is ssh_choose_pubkey: --pubkey, the key chosen before (state), else the question
// "Which SSH key should open this camera?": the shared key ~/.ssh/akam (the default, made when it does
// not exist), a key for this camera only (~/.ssh/akam-NAME, made now), or an ed25519 key of ~/.ssh.
func (a *App) sshChoosePubkey() (string, error) {
	if a.Opt.Pubkey != "" {
		return a.Opt.Pubkey, nil
	}
	if cur := a.St.Get("ssh_pubkey"); cur != "" && exists(cur) {
		a.UI.Say("  SSH key (chosen before): " + cur)
		return cur, nil
	}
	if !a.UI.Dry() {
		if p, ok := a.cfgKeyOffer(); ok { // the SSH key choice of the config chosen for this camera
			return p, nil
		}
	}
	shared := camssh.SharedKeyPath() + ".pub"
	own := camssh.OwnKeyPath(a.Cams.Name) + ".pub"
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD ask: Which SSH key should open this camera? (the shared key %s, a key for this camera only %s, or a key of ~/.ssh)", tilde(shared), tilde(own)))
		if !exists(shared) {
			a.UI.Say(fmt.Sprintf("  WOULD create an ed25519 key %s (passphrase asked twice, written with mode 600)", tilde(strings.TrimSuffix(shared, ".pub"))))
		}
		return shared, nil
	}
	cands := keyCandidates(shared, own)
	items := []string{
		fmt.Sprintf("Shared key %s (all your cameras)", tilde(strings.TrimSuffix(shared, ".pub"))),
		fmt.Sprintf("A key for this camera only (%s)", tilde(strings.TrimSuffix(own, ".pub"))),
	}
	items = append(items, cands...)
	n, err := a.UI.Pick("Which SSH key should open this camera?", items)
	if err != nil {
		return "", a.UI.Fail("no key chosen.", "run this step again, or pass --pubkey FILE.")
	}
	switch {
	case n == 0:
		_, err = a.keyEnsure(shared, "akam", true)
		return shared, err
	case n == 1:
		_, err = a.keyEnsure(own, "akam-"+a.Cams.Name, true)
		return own, err
	}
	a.UI.Say(fmt.Sprintf("  Using %s (its private key stays on this PC; only the .pub goes on the card).", cands[n-2]))
	return cands[n-2], nil
}

func (a *App) cmdPrepareFlashSD() error {
	if err := a.stepNeed("prepare-flash-sd"); err != nil {
		return err
	}
	a.stepBanner("prepare-flash-sd")
	pub, err := a.sshChoosePubkey()
	if err != nil {
		return err
	}
	if exists(pub) || !a.UI.Dry() {
		if err := pubkeyCheck(a, pub); err != nil {
			return err
		}
	}
	if !a.UI.Dry() {
		_ = a.St.Set("ssh_pubkey", pub)
	}
	if err := a.UI.RequireTTY("preparing the flash card"); err != nil {
		return ui.ErrFailed
	}
	ft := a.flashToolPath()
	if !a.UI.Dry() {
		if st, err := os.Stat(ft); err != nil || st.Size() == 0 {
			return a.UI.Fail("flash_tool not found: "+ft, fmt.Sprintf("run %s get-kit again (or %s build).", Prog, Prog))
		}
	}
	img := a.St.Get("image_path")
	bk := a.St.Get("backup_dir")
	tk := a.toolkitDir()
	c, info, err := a.sdChooseDevice()
	if err != nil {
		return err
	}
	ssid, pass, err := a.sdGetWifi()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD check the toolkit, the image, flash_tool and your stock APP backup, ask the red-box yes/no, then write an MBR + FAT32 card")
		a.UI.Say("  (mode first-boot + the flash files: image, flash_tool, flash scripts, original_firmware/mtdblock7.bin) and verify it.")
		_ = a.St.Set("flash_sd_device", "/dev/sdX")
		_ = a.St.MarkDone("prepare-flash-sd")
		return nil
	}
	pk, _ := os.ReadFile(pub)
	toolkit, err := a.loadToolkit(tk)
	if err != nil {
		return err
	}
	// everything that goes on the card is read once, here
	imgB, err := os.ReadFile(img)
	if err != nil {
		return a.UI.Fail("image not found: "+img, fmt.Sprintf("run %s step image.", Prog))
	}
	// the structure check runs on these very bytes, before the red box and before anything is written
	if err := a.imageSquashBytes(imgB); err != nil {
		return err
	}
	toolB, err := os.ReadFile(ft)
	if err != nil {
		return a.UI.Fail("flash_tool not found: "+ft, "")
	}
	if p := stockBackupProblem(bk); p != "" {
		return a.UI.Fail("the stock backup does not verify: "+p+".", "nothing was written to the card; make the backup again (or use your second copy).")
	}
	mtd7, err := os.ReadFile(filepath.Join(bk, "mtdblock7.bin"))
	if err != nil || len(mtd7) != sdcard.MTD7Size {
		return a.UI.Fail(fmt.Sprintf("%s/mtdblock7.bin missing or not %d bytes (stock APP backup)", bk, sdcard.MTD7Size), fmt.Sprintf("run: %s backup --backup-dir DIR", Prog))
	}
	if err := a.imageSquashBytes(mtd7); err != nil {
		return err
	}
	scripts := map[string][]byte{}
	for _, n := range sdcard.FlashScriptNames {
		b, err := a.embeddedScript(n)
		if err != nil {
			return a.UI.Fail(n+" is not available: "+err.Error(), "")
		}
		scripts[n] = b
	}
	files, err := sdcard.BuildPayload(sdcard.PayloadInput{Mode: sdcard.ModeFirstBoot, Toolkit: toolkit, PubKey: pk, SSID: ssid, Pass: pass,
		Flash: &sdcard.FlashFiles{ImageName: filepath.Base(img), Image: imgB, FlashTool: toolB, Scripts: scripts, MTD7: mtd7}})
	if err != nil {
		return a.UI.Fail("preparing the card failed: "+err.Error(), "the camera was not touched; fix the problem above and retry.")
	}
	if err := a.sdConfirmErase(c, info, "erase"); err != nil {
		return err
	}
	lines, err := a.sd().Prepare(c, files, sdcard.VerifySpec{Mode: sdcard.ModeFirstBoot, Flash: true, Toolkit: sdcard.ToolkitHashes(toolkit)})
	a.showVerify(lines)
	if err != nil {
		a.UI.Err(err.Error())
		return a.UI.Fail("preparing the card failed.", "the camera was not touched; fix the problem above and retry.")
	}
	_ = a.St.Set("flash_sd_device", c.Dev)
	_ = a.St.Set("flash_sd_image_sha", a.St.Get("image_sha"))
	_ = a.St.MarkDone("prepare-flash-sd")
	a.UI.Good(fmt.Sprintf("  Flash card ready. Remember: wifi.conf on the card holds your Wi-Fi password in clear text (%s wipe-sd erases it).", Prog))
	return nil
}

// ---------------------------------------------------------------- wipe-sd

func (a *App) cmdWipeSD() error {
	a.UI.Bold("Erase a microSD card")
	a.UI.Say("  Why: the cards made by this wizard hold your Wi-Fi password in clear text.")
	a.UI.Say("  Risk: ERASES the whole card (it takes a few minutes). Nothing else is touched.")
	if err := a.UI.RequireTTY("erasing the microSD card"); err != nil {
		return ui.ErrFailed
	}
	c, info, err := a.sdChooseDevice()
	if err != nil {
		return err
	}
	if err := a.sdConfirmErase(c, info, "wipe"); err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD zero-fill the card.")
		return nil
	}
	if err := a.sd().Wipe(c); err != nil {
		a.UI.Err(err.Error())
		return a.UI.Fail("wiping the card failed.", "nothing else was changed; fix the problem above and retry.")
	}
	for _, d := range []string{"backup_sd_device", "flash_sd_device"} {
		if a.St.Get(d) == c.Dev {
			_ = a.St.Unset(d)
		}
	}
	a.UI.Good("  Card wiped: it no longer holds your Wi-Fi password.")
	return nil
}

var _ = assets.Names
