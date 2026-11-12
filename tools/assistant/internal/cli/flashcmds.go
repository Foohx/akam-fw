package cli

import (
	"crypto/md5"
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/flash"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// This file: update, restore and verify (steps_camera.sh) over SSH. Every write of the camera's flash
// goes through flashSSH, which calls the one audited function, flash.Flash, after the human's
// explicit yes (default No, refused without a terminal). Nothing here reboots or power-cycles the
// camera: the user does it BY HAND.

// ---------------------------------------------------------------- tools on this computer

func (a *App) kitDir() string { return filepath.Join(a.Cams.Base, "kit") }

func exists(p string) bool { st, err := os.Stat(p); return err == nil && !st.IsDir() }

// flashScriptsDir is a test override of the scripts folder (akamtest builds only). In a release build it
// is "" and the scripts embedded in the binary (internal/assets) are sent: no folder on disk, in the
// working directory or in a checkout, is ever trusted to provide what goes to the camera.
func (a *App) flashScriptsDir() string { return testhook.Env("AKAM_FLASH_SCRIPTS_DIR") }

// embeddedScript is a camera-side script: the test override folder, else the embedded copy.
func (a *App) embeddedScript(name string) ([]byte, error) {
	if name == "akam_hwcheck.sh" {
		if p := testhook.Env("AKAM_HWCHECK"); p != "" {
			return os.ReadFile(p)
		}
	} else if d := a.flashScriptsDir(); d != "" {
		return os.ReadFile(filepath.Join(d, name))
	}
	return assets.Script(name)
}

// flashToolPath is cam_flash_tool: flash_tool is an ARM binary that is only taken from the verified kit.
func (a *App) flashToolPath() string {
	if p := testhook.Env("AKAM_FLASH_TOOL"); p != "" {
		return p
	}
	return filepath.Join(a.kitDir(), "bin", "flash_tool")
}

// ---------------------------------------------------------------- image gate and confirmation

// imageGate is cam_image_gate: exactly the APP partition size; returns the md5.
func (a *App) imageGate(img string) (string, error) {
	_, m, err := a.imageRead(img)
	return m, err
}

// imageGateChecked is imageGate plus the squashfs check, both on the SAME bytes (the file is read once):
// the md5 it returns is the md5 of the bytes that were checked, and flash.Flash / flashTelnet refuse any
// bytes whose md5 differs, so a file swapped after the check cannot be written.
func (a *App) imageGateChecked(img string) (string, error) {
	b, m, err := a.imageRead(img)
	if err != nil {
		return "", err
	}
	if err := a.imageSquashBytes(b); err != nil {
		return "", err
	}
	return m, nil
}

func (a *App) imageRead(img string) ([]byte, string, error) {
	st, err := os.Stat(img)
	if err != nil || !st.Mode().IsRegular() {
		return nil, "", a.UI.Fail("image not found: "+img, fmt.Sprintf("run %s step image.", Prog))
	}
	if st.Size() != appPartitionBytes {
		return nil, "", a.UI.Fail(fmt.Sprintf("the image is %d bytes, expected exactly %d.", st.Size(), appPartitionBytes), "do not flash it.")
	}
	b, err := os.ReadFile(img)
	if err != nil {
		return nil, "", a.UI.Fail("cannot read the image: "+err.Error(), "")
	}
	sum := md5.Sum(b)
	m := fmt.Sprintf("%x", sum)
	a.UI.Say("  Image: " + img)
	a.UI.Say(fmt.Sprintf("  Size %d bytes (full APP partition), md5 %s.", st.Size(), m))
	return b, m, nil
}

// flashConfirm is cam_flash_confirm: the red box and one yes/no (default No, needs a terminal).
func (a *App) flashConfirm(label, ip, md5, route string) (bool, error) {
	// Danger answers "yes" in a rehearsal: that must never become a real confirmation.
	if a.UI.Dry() {
		return false, errors.New("a rehearsal never confirms a flash")
	}
	var what string
	switch label {
	case "Restoring":
		what = "put the ORIGINAL (stock) APP back on the camera"
	case "Updating":
		what = "write the new Akam image to the camera"
	default:
		what = "write the Akam image to the camera"
	}
	return a.UI.Danger(label+": WRITES THE CAMERA FLASH", "Write the camera flash now?",
		fmt.Sprintf("Camera: %s (route: %s)", ip, route),
		"This will "+what+": it erases and rewrites the APP partition (/dev/mtd7).",
		fmt.Sprintf("Image md5 (checked on this PC and on the camera): %s", md5),
		"Do NOT unplug the camera or the network until the wizard says it is finished: a power cut during the write can leave the camera dead (only a UART cable recovers it).")
}

var flashKeepRe = regexp.MustCompile(`(?i)^reflash_over_ssh:|error|fail|mismatch|md5|verif|retry|retries|fault|success|complete|refus`)
var flashProgRe = regexp.MustCompile(`^\s*([A-Za-z][A-Za-z +/-]*): \[[=> ]*\]\s*([0-9]{1,3})%`)

// flashSink writes every line to the log file and shows the filtered ones (cam_filter_flash_output).
func (a *App) flashSink(logPath string) (func(string), func()) {
	f, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		f = nil
	}
	out := func(line string) {
		if f != nil {
			fmt.Fprintln(f, line)
		}
		a.Log.Line("FLASH %s", line)
		l := strings.TrimPrefix(line, "camera: ")
		if m := flashProgRe.FindStringSubmatch(l); m != nil {
			p, _ := strconv.Atoi(m[2])
			a.UI.Progress(m[1], p)
		} else if flashKeepRe.MatchString(l) {
			a.UI.ProgressClose()
			a.UI.Say("    | " + l)
		}
	}
	return out, func() {
		a.UI.ProgressClose()
		if f != nil {
			f.Close()
		}
	}
}

func (a *App) dialFlash(ip string) func(int) (flash.Conn, error) {
	return func(port int) (flash.Conn, error) {
		c, err := a.dialPort(ip, port)
		if err != nil {
			return nil, err
		}
		return c, nil
	}
}

func (a *App) flashPlan(img, md5 string) flash.Plan {
	p := flash.Plan{Image: img, MD5: md5, FlashTool: a.flashToolPath(), ScriptsDir: a.flashScriptsDir(),
		Port: a.Env.PortSSH, SecondPort: a.Env.PortSSH2}
	if ms := envInt("AKAM_REFLASH_POLL_MS"); ms > 0 { // test hooks
		p.Poll = time.Duration(ms) * time.Millisecond
	}
	if ms := envInt("AKAM_REFLASH_RETRY_MS"); ms > 0 {
		p.RetrySleep = time.Duration(ms) * time.Millisecond
	}
	return p
}

func envInt(name string) int {
	n, _ := strconv.Atoi(testhook.Env(name)) // only the AKAM_REFLASH_* test hooks
	return n
}

// flashSSH is flash_ssh: the validated no-card path. Precheck (writes nothing), yes/no (default
// No), flash. It is the only caller of flash.Flash.
func (a *App) flashSSH(ip, img, md5, label string) error {
	if a.UI.Dry() {
		a.UI.Say("  WOULD run the precheck of the SSH reflash (writes nothing), ask a yes/no (default No), then:")
		a.UI.Say(fmt.Sprintf("    built-in SSH reflash (the steps of tools/flash_scripts/reflash_over_ssh.sh) --host %s --identity %s --image <image> --md5 <md5> --flash-tool <flash_tool> --i-understand-no-uart", ip, a.keyPath()))
		a.UI.Say("  and relay its progress (the progress bars are filtered out). It never reboots the camera.")
		_, _ = a.UI.Danger("Write the camera flash", "Write the camera flash now?", "<camera ip>", "<image md5>")
		return nil
	}
	ft := a.flashToolPath()
	if st, err := os.Stat(ft); err != nil || st.Size() == 0 {
		return a.UI.Fail("flash_tool not found: "+ft, fmt.Sprintf("run %s get-kit again (or %s build).", Prog, Prog))
	}
	c, err := a.connect(ip, fmt.Sprintf("SSH to root@%s with the key %s does not work.", ip, a.keyPath()),
		"check the key (--identity FILE), the address, and that the camera has finished booting.")
	if err != nil {
		return err
	}
	c.Close()
	plan := a.flashPlan(img, md5)
	deps := func(out func(string)) flash.Deps {
		return flash.Deps{Dial: a.dialFlash(ip), Out: out, Ctx: a.Ctx}
	}
	a.UI.Say("  Precheck: uploads two small scripts to the camera's RAM and checks it; nothing is written to flash.")
	a.UI.Say("  Running: built-in SSH reflash precheck")
	logPre := filepath.Join(a.St.Dir, "reflash-precheck.log")
	a.UI.Say("  (full output: " + logPre + ")")
	out, done := a.flashSink(logPre)
	a.Log.Line("CMD flash precheck %s", ip)
	err = flash.Precheck(plan, deps(out))
	done()
	if err != nil {
		return a.UI.Fail("the precheck failed (see above).", "nothing was written to the camera.")
	}
	a.UI.Blank()
	a.UI.Warn(label + " over SSH has NO UART safety net. If the power is cut or the write is interrupted, the APP partition")
	a.UI.Warn("is half written and only a UART cable can recover it. Keep the camera powered and do not touch it.")
	a.UI.Say("  Image md5 (checked on this PC): " + md5)
	conf, err := flash.NewConfirmation(md5, func() (bool, error) { return a.flashConfirm(label, ip, md5, "SSH") })
	if err != nil {
		return a.UI.Fail("not confirmed.", "nothing was written.")
	}
	logFl := filepath.Join(a.St.Dir, "reflash.log")
	a.UI.Say("  Running: built-in SSH reflash")
	a.UI.Say("  (full output: " + logFl + ")")
	out, done = a.flashSink(logFl)
	a.Log.Line("CMD flash %s md5=%s", ip, md5)
	err = flash.Flash(plan, conf, deps(out))
	done()
	a.Log.Line("RC flash err=%v", err)
	if err != nil {
		return a.UI.Fail("the SSH reflash failed or could not be verified.",
			fmt.Sprintf("DO NOT power-cycle yet. Read %s and docs/akam_v1_flash_and_restore.md (Reflashing over SSH).", logFl))
	}
	a.UI.Good(fmt.Sprintf("  The camera confirmed the written image (read-back md5 = %s).", md5))
	return nil
}

func (a *App) afterFlash() {
	a.UI.Blank()
	a.UI.Good("  Flash done and verified. The wizard NEVER power-cycles the camera.")
	a.UI.Say("  Now power-cycle it BY HAND: unplug the USB-C power, wait 5 seconds, plug it back.")
	a.UI.Say(fmt.Sprintf("  Keep the card in for now; then run: %s step verify", Prog))
}

// ---------------------------------------------------------------- update

func (a *App) cmdUpdate() error {
	a.banner("Update Akam with a new image (SSH, no card)")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	img := a.Opt.Image
	if img == "" {
		img = a.St.Get("image_path")
	}
	if a.UI.Dry() {
		if img == "" {
			img = "<image>"
		}
		return a.flashSSH(ip, img, "dry-run", "Updating")
	}
	if img == "" {
		return a.UI.Fail("no image given.", fmt.Sprintf("build one with %s step image or pass --image FILE.", Prog))
	}
	md5, err := a.imageGateChecked(img)
	if err != nil {
		return err
	}
	// the backup gate, for the command as for the flow: re-hashed now, before anything is sent
	if dir, why := a.backupGate(); dir == "" {
		return a.UI.Fail("no verified backup: "+why+".", fmt.Sprintf("make one first (%s backup, or Back up / restore); nothing was written.", Prog))
	}
	if err := a.flashSSH(ip, img, md5, "Updating"); err != nil {
		return err
	}
	_ = a.St.Set("flash_md5", md5)
	_ = a.St.ClearDone("verify")
	a.afterFlash()
	return nil
}

// ---------------------------------------------------------------- restore

func (a *App) cmdRestore() error {
	a.banner("Restore the original (stock) APP from your backup")
	a.UI.Say("  Why: puts the original application partition back. UBOOT, kernel, ROOTFS and CONFIG are never touched.")
	a.UI.Say("  Risk: WRITES the camera flash; a power cut can brick it.")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	bk := a.Opt.BackupDir
	if bk == "" {
		bk = a.St.Get("backup_dir")
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD check %s/mtdblock7.bin (size %d, sha256 and md5), choose SSH (Akam running) or the", bk, appPartitionBytes))
		a.UI.Say("  maintenance telnet, and ask a yes/no (default No). Same gates as flash; no reboot is ever sent.")
		return a.flashSSH(ip, "<image>", "dry-run", "Restoring")
	}
	app := filepath.Join(bk, "mtdblock7.bin")
	if bk == "" || !exists(app) {
		return a.UI.Fail("no backup found.", "pass --backup-dir DIR (it must hold mtdblock7.bin).")
	}
	// restore puts the ORIGINAL APP back: the folder is re-hashed now, and a backup made over SSH from an
	// Akam camera (kind=akam) is refused, it holds the Akam APP
	if p := stockBackupProblem(bk); p != "" {
		return a.UI.Fail("the backup does not verify: "+p+".", "restore needs the backup of the stock firmware (made with the card): use it or your second copy. Nothing was written.")
	}
	md5, err := a.imageGateChecked(app)
	if err != nil {
		return err
	}
	a.UI.Say("  Stock APP sha256: " + fileSHA(app))
	route := "telnet"
	if a.St.Get("camera_kind") == "akam" {
		if c, derr := a.dial(ip); derr == nil {
			c.Close()
			a.UI.Say("  Akam runs and SSH works.")
			if ok, perr := a.UI.AskYN("  Restore over SSH (no card needed)? [no = use the maintenance telnet and the card]"); perr == nil && ok {
				route = "ssh"
			}
		}
	}
	a.UI.Say("  Route: " + route)
	if route == "telnet" {
		if err := a.flashTelnet(ip, app, md5, "Restoring"); err != nil {
			return err
		}
	} else {
		if err := a.flashSSH(ip, app, md5, "Restoring"); err != nil {
			return err
		}
	}
	_ = a.St.ClearDone("flash")
	_ = a.St.ClearDone("verify")
	a.UI.Blank()
	a.UI.Good(fmt.Sprintf("  Restore written and verified (read-back md5 = %s).", md5))
	a.UI.Say("  Power-cycle the camera BY HAND (unplug the USB-C power, wait 5 seconds, plug it back).")
	a.UI.Say("  CONFIG was not touched, so the Wi-Fi settings stay. Details: docs/akam_v1_flash_and_restore.md section 5.")
	return nil
}

// ---------------------------------------------------------------- verify

// waitPort is cam_wait_port: wait until the camera answers on the port.
func (a *App) waitPort(ip string, port, max int, what string) bool {
	a.UI.Say(fmt.Sprintf("  Waiting for %s: %s port %d (up to %d seconds) ...", what, ip, port, max))
	t0 := time.Now()
	lastp := -100
	for {
		el := int(time.Since(t0).Seconds())
		if el-lastp >= 15 {
			if el > 0 {
				a.UI.Say(fmt.Sprintf("  ... still waiting for %s (%d of %d s)", what, el, max))
			}
			lastp = el
		}
		if len(a.probe(ip, port)) > 0 {
			a.UI.Say(fmt.Sprintf("  Port %d answered after %d s.", port, int(time.Since(t0).Seconds())))
			return true
		}
		if el >= max {
			a.UI.Say(fmt.Sprintf("  No answer on port %d within %d s.", port, max))
			return false
		}
		select {
		case <-a.Ctx.Done():
			return false
		case <-time.After(time.Second):
		}
	}
}

var hwLineRe = regexp.MustCompile(`^(PASS|FAIL|WARN|UNKNOWN)|^INFO +RTSP auth`)

// hwcheck is cam_hwcheck: copy akam_hwcheck.sh to the camera's RAM and run it.
func (a *App) hwcheck(ip string) bool {
	body, err := a.embeddedScript("akam_hwcheck.sh")
	if err != nil {
		a.UI.Fail("akam_hwcheck.sh is not available: "+err.Error(), "")
		return false
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return false
	}
	defer c.Close()
	a.UI.Say("  Copying akam_hwcheck.sh to the camera's RAM (/tmp, nothing is installed) ...")
	res, err := a.run(c, remoteHwcheckPut, strings.NewReader(string(body)), "'cat > /tmp/akam_hwcheck.sh' < akam_hwcheck.sh")
	if err != nil || res.Status != 0 {
		a.UI.Fail("cannot copy akam_hwcheck.sh over SSH.", "")
		return false
	}
	a.UI.Say("  Running it (read-only) ...")
	res, err = a.run(c, remoteHwcheckRun, nil, "")
	if err != nil {
		a.UI.Err(err.Error())
		return false
	}
	for _, l := range strings.Split(res.Combined(), "\n") {
		if hwLineRe.MatchString(l) {
			a.UI.Say("    " + l)
		}
	}
	return res.Status == 0
}

// onvifFlag is cam_onvif_flag: "enabled=1|0" and "port=N" written by akam_onvif.sh; absent = off.
func (a *App) onvifFlag(c *camssh.Client) (on bool, port int) {
	port = 80
	res, err := a.run(c, remoteOnvifFlag, nil, "")
	if err != nil {
		return false, port
	}
	for _, l := range strings.Split(string(res.Stdout), "\n") {
		l = strings.TrimSuffix(l, "\r")
		switch {
		case l == "enabled=1":
			on = true
		case strings.HasPrefix(l, "port="):
			p := strings.TrimPrefix(l, "port=")
			if n, err := strconv.Atoi(p); err == nil && allDigits(p) && len(p) <= 5 && n >= 1 && n <= 65535 {
				port = n
			}
		}
	}
	return on, port
}

const onvifFlagFile = "/etc/config/akam/onvif"

// portsCheck is cam_ports_check: open = 22 554 8091 only (24 allowed while the card is in; the ONVIF
// HTTP port only when ONVIF is on).
func (a *App) portsCheck(ip string, expectTelnet string) bool {
	c, err := a.connectTools(ip)
	if err != nil {
		return false
	}
	on, oport := a.onvifFlag(c)
	c.Close()
	ftp, tel23 := a.Env.PortFTP, a.Env.PortTelnet23
	http := a.Env.PortHTTP
	if http == 0 {
		if on {
			http = oport
		} else {
			http = 80
		}
	}
	open := a.probe(ip, a.Env.PortSSH, a.Env.PortRTSP, a.Env.PortCTL, a.Env.PortTelnet, ftp, tel23, http)
	a.UI.Say("  Open ports: " + orNone(strings.TrimSpace(joinPorts(open))))
	isOpen := func(p int) bool {
		for _, o := range open {
			if o == p {
				return true
			}
		}
		return false
	}
	bad := false
	for _, p := range []int{a.Env.PortSSH, a.Env.PortRTSP, a.Env.PortCTL} {
		if !isOpen(p) {
			a.UI.Err(fmt.Sprintf("port %d should be open and is not.", p))
			bad = true
		}
	}
	for _, p := range []int{a.Env.PortTelnet, ftp, tel23} {
		if isOpen(p) && expectTelnet == "closed" {
			a.UI.Err(fmt.Sprintf("port %d should be closed (telnet/FTP must be off without the card) and is open.", p))
			bad = true
		}
	}
	switch {
	case isOpen(http) && on:
		a.UI.Say(fmt.Sprintf("  ONVIF is on: port %d is open, as expected (UDP 3702 is checked on the camera).", http))
	case isOpen(http):
		a.UI.Err(fmt.Sprintf("port %d should be closed (ONVIF is off) and is open.", http))
		bad = true
	case on:
		a.UI.Err(fmt.Sprintf("ONVIF is on but port %d is not open: ak_rtsp may not have started it (%s onvif status).", http, Prog))
		bad = true
	}
	return !bad
}

// wifiVerdict is cam_wifi_verdict: the WIFI= line of preflight_check.sh --ensure-wifi. True = the
// camera keeps its network without the card.
func (a *App) wifiVerdict(out string) bool {
	code := ""
	for _, l := range strings.Split(out, "\n") {
		if m := regexp.MustCompile(`^WIFI=([A-Z_]*)`).FindStringSubmatch(l); m != nil {
			code = m[1]
		}
	}
	switch code {
	case "PRESENT":
		a.UI.Say("  /etc/config/wpa_supplicant.conf holds a Wi-Fi network: the camera keeps its network without the card.")
		return true
	case "CREATED":
		a.UI.Good("  /etc/config/wpa_supplicant.conf was missing (the camera was never paired with the app): it was created ON the camera from the card's Wi-Fi settings.")
		a.UI.Say("  (the password never passed through this PC's command lines or logs.)")
		return true
	case "NO_CARD_CONFIG":
		a.UI.Fail("the camera has no saved Wi-Fi (/etc/config/wpa_supplicant.conf) and the card holds no usable custom/configs/wifi.conf to create it from.",
			fmt.Sprintf("without it the camera has NO network once the card is removed. Prepare the flash card again (%s step prepare-flash-sd), or create the file by hand: docs/akam_v1_install.md (Troubleshooting: no network after removing the card).", Prog))
	case "BAD_CHARS":
		a.UI.Fail("the Wi-Fi name or password on the card holds a double quote, a backslash or a line break: the camera's wpa_supplicant.conf cannot be written safely from it.",
			"create /etc/config/wpa_supplicant.conf by hand (docs/akam_v1_install.md, Troubleshooting), or use a Wi-Fi password without those characters.")
	case "BAD_PSK_LENGTH":
		a.UI.Fail("the Wi-Fi password on the card is not 8 to 63 characters long, so it is not a WPA password.",
			fmt.Sprintf("check the password, then prepare the card again (%s step prepare-flash-sd).", Prog))
	case "UNSUPPORTED_SECURITY":
		a.UI.Fail("the card's Wi-Fi settings use a security type other than WPA/WPA2-PSK (SECURITY=3) or open (SECURITY=1).",
			"create /etc/config/wpa_supplicant.conf by hand for your network (docs/akam_v1_install.md, Troubleshooting).")
	case "WRITE_FAILED":
		a.UI.Fail("the camera could not write /etc/config/wpa_supplicant.conf.", "do not remove the card. Create the file by hand (docs/akam_v1_install.md, Troubleshooting).")
	default:
		a.UI.Fail("could not check the camera's saved Wi-Fi (no answer from preflight_check.sh --ensure-wifi).", "do not remove the card; run this step again.")
	}
	return false
}

// wifiGate is cam_wifi_gate_ssh: the script is sent through stdin into /tmp (RAM).
func (a *App) wifiGate(ip string) bool {
	body, err := a.embeddedScript("preflight_check.sh")
	if err != nil {
		a.UI.Fail("preflight_check.sh is not available: "+err.Error(), "")
		return false
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return false
	}
	defer c.Close()
	a.UI.Say("  Checking that the camera will keep its Wi-Fi after the card is removed ...")
	res, err := a.run(c, remotePreflightPut, strings.NewReader(string(body)), "'cat > /tmp/akam_preflight.sh' < preflight_check.sh")
	if err != nil || res.Status != 0 {
		a.UI.Fail("cannot copy preflight_check.sh over SSH.", "do not remove the card yet.")
		return false
	}
	res, err = a.run(c, remotePreflightRun, nil, "")
	if err != nil {
		a.UI.Err(err.Error())
		return false
	}
	return a.wifiVerdict(res.Combined())
}

func (a *App) cmdVerify() error {
	if err := a.stepNeed("verify"); err != nil {
		return err
	}
	a.stepBanner("verify")
	if a.UI.Dry() {
		a.UI.Say("  WOULD wait for the camera, check SSH with your key, copy and run akam_hwcheck.sh (expecting no FAIL),")
		a.UI.Say("  check that the open ports are 22, 554 and 8091 (24 allowed while the card is in; 80 only if ONVIF is on), make sure the camera has a saved")
		a.UI.Say("  Wi-Fi network (preflight_check.sh --ensure-wifi on the camera; it is created from the card's wifi.conf when missing,")
		a.UI.Say("  and you are never asked to remove the card before that is done), then ask you to")
		a.UI.Say("  unplug the power, take the card out and plug the power back in BY HAND, and repeat the checks:")
		a.UI.Say("  telnet 24 and FTP 21 must then be closed. Only then is the step marked done.")
		_ = a.St.MarkDone("verify")
		a.verifyHostname(nil, "")
		a.rtspAuthOffer("")
		a.wipeOffer()
		a.onvifOffer("")
		a.ntpOffer("")
		return nil
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	wait := a.Env.WaitSecs
	if !a.waitPort(ip, a.Env.PortSSH, wait, "the camera to answer on SSH") {
		return a.UI.Fail("the camera does not answer on SSH.", "it may still be starting (wait a minute and run this step again), the address may have changed (check your router), or the camera may have no network: see docs/akam_v1_flash_and_restore.md (Recovery). The step is NOT done.")
	}
	c, err := a.dial(ip)
	if err != nil {
		var hk *camssh.HostKeyChangedError
		if errors.As(err, &hk) || errors.Is(err, camssh.ErrHostKeyRefused) {
			return a.dialFailure(ip, err, "", "")
		}
		return a.UI.Fail(fmt.Sprintf("SSH with %s was refused.", a.keyPath()), "is this the key you put on the flash card (--identity FILE)? The step is NOT done: do not remove the card.")
	}
	c.Close()
	a.UI.Good("  SSH with your key works.")
	if !a.hwcheck(ip) {
		return a.UI.Fail("akam_hwcheck.sh reported a failure (lines above).", "do not remove the card yet; read docs/akam_v1_test_plan.md.")
	}
	if !a.portsCheck(ip, "card") {
		return a.UI.Fail("unexpected port state.", "do not remove the card yet.")
	}
	a.UI.Good("  Checks passed with the card in.")
	if !a.wifiGate(ip) {
		a.UI.Say("  The step is NOT done: do not remove the card.")
		return ui.ErrFailed
	}
	a.UI.Blank()
	a.UI.Say("  Now the real test: without the card. By hand:")
	a.UI.Say("    1. unplug the camera's USB-C power  2. take the microSD card out  3. plug the power back in.")
	ans, err := a.UI.Prompt("  Press Enter when the camera is powered again (or q to stop):", "")
	if err != nil {
		return a.UI.Fail("no input.", fmt.Sprintf("run %s step verify again.", Prog))
	}
	if ans == "q" {
		return a.UI.Fail("stopped.", fmt.Sprintf("run %s step verify again when ready.", Prog))
	}
	if !a.waitPort(ip, a.Env.PortSSH, wait, "the camera to boot without the card and answer on SSH") {
		return a.UI.Fail(fmt.Sprintf("the camera is not on the network %d seconds after the power-up without the card.", wait),
			"most likely its Wi-Fi is not saved on the camera (no /etc/config/wpa_supplicant.conf): put the card back, power-cycle, run this step again (it creates the file) and see docs/akam_v1_install.md (Troubleshooting: no network after removing the card). Also check the address in your router: it may have changed.")
	}
	if !a.portsCheck(ip, "closed") {
		return a.UI.Fail("port check failed without the card.", "see docs/akam_v1_ssh_and_lockdown.md.")
	}
	if !a.hwcheck(ip) {
		return a.UI.Fail("akam_hwcheck.sh reported a failure without the card.", "")
	}
	_ = a.St.MarkDone("verify")
	a.UI.Good("  Verified: SSH works, only 22, 554 and 8091 are open, telnet and FTP are off.")
	a.verifyHostname(nil, ip)
	a.rtspAuthOffer(ip)
	a.wipeOffer()
	a.onvifOffer(ip)
	a.ntpOffer(ip)
	return nil
}

// verifyHostname gives the camera the network name chosen for it (never fails the step).
func (a *App) verifyHostname(_ *camssh.Client, ip string) {
	if a.Cams.Name == "default" {
		if a.UI.Dry() {
			a.UI.Say("  WOULD also set the camera's network name over SSH when you gave the camera a name (./akam --camera NAME).")
		}
		return
	}
	if a.UI.Dry() {
		_ = a.hostnameApply(nil, a.Cams.Name)
		return
	}
	c, err := a.connectTools(ip)
	if err == nil {
		defer c.Close()
		err = a.hostnameApply(c, a.Cams.Name)
	}
	if err != nil {
		a.UI.Say(fmt.Sprintf("  You can try again later with: %s hostname %s", Prog, a.Cams.Name))
	}
}

func (a *App) rtspAuthOffer(ip string) {
	a.UI.Blank()
	disp := ip
	if disp == "" {
		disp = "<camera-ip>"
	}
	a.UI.Say("  Optional: protect the video stream with a password.")
	a.UI.Say(fmt.Sprintf("  Without one, anyone on your network can watch the camera at rtsp://%s:554/.", disp))
	a.UI.Say("  With one, players ask for a user name and password (Digest login: VLC, ffmpeg, go2rtc and Frigate support it)")
	a.UI.Say(fmt.Sprintf("  You can change your mind at any time: %s rtsp-auth set | disable | status", Prog))
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask: Protect the video stream with a username/password? [y/N] (default: No)")
		_ = a.rtspAuthSet(nil, disp)
		return
	}
	if !a.UI.Interactive() {
		a.UI.Say(fmt.Sprintf("  No terminal: the stream stays open. Run %s rtsp-auth set later.", Prog))
		return
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return
	}
	defer c.Close()
	state, text := a.rtspStatus(c)
	if state == "enabled" {
		a.UI.Say(fmt.Sprintf("  Already on: %s. Change it with %s rtsp-auth set.", text, Prog))
		return
	}
	if ok, _ := a.UI.AskYN("  Protect the video stream with a username/password?"); ok {
		if a.rtspAuthSet(c, ip) != nil {
			a.UI.Warn(fmt.Sprintf("the stream password was not set (the verification above still stands). Run %s rtsp-auth set later.", Prog))
		}
	} else {
		a.UI.Say(fmt.Sprintf("  Stream left open. Run %s rtsp-auth set whenever you want a password.", Prog))
	}
}

func (a *App) wipeOffer() {
	if a.St.Get("backup_sd_device") == "" && a.St.Get("flash_sd_device") == "" {
		return
	}
	a.UI.Blank()
	a.UI.Warn("your microSD card(s) still hold your Wi-Fi password in clear text (wifi.conf).")
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD offer: wipe them now (a yes/no, default No), or later with %s wipe-sd", Prog))
		return
	}
	// Wiping a card is phase P3 of the native program: only the pointer to the bash wizard.
	a.UI.Say(fmt.Sprintf("  Wipe them with the bash wizard: %s wipe-sd", Prog))
}

func (a *App) onvifOffer(ip string) {
	a.UI.Blank()
	a.UI.Say("  Optional: make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager).")
	a.UI.Say("  Off by default. Turning it on opens port 80 (TCP) and UDP 3702 on your network and needs the stream password.")
	a.UI.Say(fmt.Sprintf("  You can change your mind at any time: %s onvif enable | disable | status", Prog))
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask: Make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager)? [y/N] (default: No)")
		a.UI.Say(fmt.Sprintf("  WOULD, on yes and without a stream password, offer to run %s rtsp-auth set first", Prog))
		a.UI.Say("  WOULD run over SSH on the camera: " + onvifCmd + " enable")
		return
	}
	if !a.UI.Interactive() {
		a.UI.Say(fmt.Sprintf("  No terminal: ONVIF stays off. Run %s onvif enable later.", Prog))
		return
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return
	}
	defer c.Close()
	if on, port := a.onvifFlag(c); on {
		a.UI.Say(fmt.Sprintf("  Already on (port %d). Turn it off with %s onvif disable.", port, Prog))
		return
	}
	if ok, _ := a.UI.AskYN("  Make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager)?"); !ok {
		a.UI.Say(fmt.Sprintf("  ONVIF left off. Run %s onvif enable whenever you want it.", Prog))
		return
	}
	state, text := a.rtspStatus(c)
	if (state == "enabled" && strings.Contains(text, "not ONVIF-ready")) || state == "disabled" || state == "invalid" || state == "unknown" {
		a.UI.Say("  ONVIF needs the stream password (the camera refuses to turn it on without one).")
		if ok, _ := a.UI.AskYN("  Set the stream password now?"); ok {
			if a.rtspAuthSet(c, ip) != nil {
				a.UI.Warn(fmt.Sprintf("no password was set: ONVIF stays off. Run %s rtsp-auth set, then %s onvif enable.", Prog, Prog))
				return
			}
		} else {
			a.UI.Say(fmt.Sprintf("  ONVIF left off. Run %s rtsp-auth set, then %s onvif enable.", Prog, Prog))
			return
		}
	}
	if a.onvifRemote(c, "enable") == nil {
		a.UI.Good("  ONVIF is on. In your app, add the camera by its address (port 80), with your stream user and password.")
	} else {
		a.UI.Warn(fmt.Sprintf("ONVIF was not turned on (the verification above still stands). Run %s onvif enable later.", Prog))
	}
}
