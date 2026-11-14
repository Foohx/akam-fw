package cli

// The stock-camera commands over the maintenance telnet (port 24): backup and the telnet + card route
// of flash and restore. A port of tools/wizard/lib/camsh.sh, transfer.sh and the matching parts of
// steps_camera.sh: the same commands are sent, the same checks made, the same messages shown.
// Nothing here reboots or power-cycles the camera, kills a process or touches the watchdog; the "YES"
// to the camera's flash script is sent only after the human's explicit yes (flash.Confirmation).

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/telnet"
	"github.com/Foohx/akam-fw/tools/assistant/internal/flash"
	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

const camLayout = `mtd0: 00032000 00001000 "UBOOT"
mtd1: 00001000 00001000 "ENV"
mtd2: 00001000 00001000 "ENVBK"
mtd3: 00010000 00001000 "DTB"
mtd4: 00180000 00001000 "KERNEL"
mtd5: 000fc000 00001000 "ROOTFS"
mtd6: 00040000 00001000 "CONFIG"
mtd7: 00500000 00001000 "APP"`

var camMtdSizes = []int64{204800, 4096, 4096, 65536, 1572864, 1032192, 262144, 5242880}

// ---------------------------------------------------------------- manual mode

type manualUI struct{ a *App }

func (m manualUI) Show(host string, port int, cmd string) {
	u := m.a.UI
	u.Blank()
	u.Bold(fmt.Sprintf("MANUAL STEP - in your own telnet session to the camera (%s port %d)", host, port))
	u.Say("  Run exactly this ONE command (copy it, paste it, press Enter):")
	u.Say("")
	u.Say("      " + cmd)
	u.Say("")
}

func (m manualUI) Collect() (string, bool, error) {
	u := m.a.UI
	u.Say("  Paste the output here (nothing to paste? skip this), then type END on a line of its own.")
	var out strings.Builder
	for {
		l, err := u.Prompt("  >", "")
		if err != nil {
			u.Err("no input: cannot continue in manual mode.")
			return "", false, err
		}
		l = strings.TrimRight(l, "\r")
		if l == "END" {
			break
		}
		out.WriteString(l + "\n")
	}
	ok, err := u.AskYN("  Did the command succeed (no error message)?")
	if err != nil {
		return "", false, err
	}
	return out.String(), ok, nil
}

func (m manualUI) Wait(text string) error {
	u := m.a.UI
	u.Say("  " + text)
	stopped, err := m.a.handsDone("Do this by hand", []string{text}, "  Press Enter when done (or type q to stop):")
	if err != nil || stopped {
		return errors.New("stopped")
	}
	return nil
}

// ---------------------------------------------------------------- session and transfers

func (a *App) camshTimeout() time.Duration {
	if n := envInt("AKAM_CAMSH_TIMEOUT"); n > 0 {
		return time.Duration(n) * time.Second
	}
	if ms := envInt("AKAM_CAMSH_TIMEOUT_MS"); ms > 0 { // test hook (milliseconds)
		return time.Duration(ms) * time.Millisecond
	}
	return telnet.DefTimeout
}

func (a *App) newTelnet(ip string) *telnet.Session {
	return telnet.New(telnet.Config{
		Host: ip, Port: a.Env.PortTelnet, Timeout: a.camshTimeout(),
		Manual: manualUI{a}, ForceManual: a.Opt.ManualTelnet,
		Negotiate: a.Opt.TelnetNegotiate || testhook.Env("AKAM_TELNET_NEGOTIATE") == "1",
		Warn:      a.UI.Warn, Log: a.Log.Line,
	})
}

// camSessionOpen is cam_session_open.
func (a *App) camSessionOpen(ip string) error {
	a.tn = a.newTelnet(ip)
	if err := a.tn.Open(); err != nil {
		return a.UI.Fail(fmt.Sprintf("cannot connect to %s port %d.", ip, a.Env.PortTelnet),
			"the maintenance telnet is only open while the toolkit card is in the camera. Insert the card, power the camera and wait about 2 minutes, or pass --manual-telnet.")
	}
	return nil
}

func (a *App) camClose() {
	if a.tn != nil {
		a.tn.Close()
	}
}

func (a *App) xfer() *telnet.Xfer {
	x := &telnet.Xfer{S: a.tn, Rep: a.UI,
		PortIn: envPortHook("AKAM_PORT_XFER_IN", 8765), PortOut: envPortHook("AKAM_PORT_XFER_OUT", 8766),
		Tries: envInt("AKAM_XFER_TRIES"), ConnectTries: envInt("AKAM_XFER_CONNECT_TRIES")}
	if ms := envInt("AKAM_XFER_CONNECT_MS"); ms > 0 {
		x.ConnectEvery = time.Duration(ms) * time.Millisecond
	}
	return x
}

func envPortHook(name string, def int) int {
	if n := envInt(name); n > 0 && n < 65536 {
		return n
	}
	return def
}

// confirm is ui_confirm: a rehearsal only shows the question; otherwise a yes/no (default No).
func (a *App) confirm(q string) bool {
	if a.UI.Dry() {
		a.UI.Say("WOULD ask: " + q + " [y/N]")
		return true
	}
	ok, err := a.UI.AskYN(q)
	return err == nil && ok
}

// ---------------------------------------------------------------- hardware list

// hwVerdict is hw_verdict: "status|device" for a stock APP hash (unknown: untested).
func hwVerdict(appSHA string) (status, device string) {
	b, _ := assets.Script("supported_hardware.txt")
	for _, l := range strings.Split(string(b), "\n") {
		if strings.TrimSpace(l) == "" || strings.HasPrefix(strings.TrimSpace(l), "#") {
			continue
		}
		f := strings.Split(l, "|")
		if len(f) >= 7 && f[6] == appSHA {
			return f[0], f[1]
		}
	}
	return "untested", "not in the supported hardware list (unknown stock APP)"
}

// camConfirmUntested is cam_confirm_untested.
func (a *App) camConfirmUntested(appSHA, what string) bool {
	vstat, vdev := hwVerdict(appSHA)
	a.UI.Say(fmt.Sprintf("  Hardware verdict for this stock APP: %s - %s", vstat, vdev))
	a.UI.Say("  Stock APP sha256: " + appSHA)
	if vstat == "validated" {
		return true
	}
	ok, _ := a.UI.Danger("Untested firmware", what,
		"This stock firmware is NOT on the validated list: Akam may not work and the camera may need a restore.",
		fmt.Sprintf("Hardware verdict: %s - %s", vstat, vdev), "Stock APP sha256: "+appSHA)
	if !ok {
		_ = a.UI.Fail("untested firmware not confirmed.", "nothing was changed.")
	}
	return ok
}

// ---------------------------------------------------------------- find-camera: telnet detection

// identifyTelnet is cam_identify_telnet: opens the shell once (no fallback here); returns stock or akam.
func (a *App) identifyTelnet(ip string) (string, error) {
	if a.Opt.ManualTelnet {
		a.UI.Say("  Manual telnet mode: not opening the shell; assuming stock firmware with the toolkit.")
		return "stock", nil
	}
	s := a.newTelnet(ip)
	if err := s.Open(); err != nil {
		return "", a.UI.Fail(fmt.Sprintf("port %d is open but the connection failed.", a.Env.PortTelnet), "try again in a moment.")
	}
	defer s.Close()
	_ = s.Start("cat /proc/version; [ -d /usr/local/akam ] && echo AKAM_PRESENT || echo AKAM_ABSENT")
	if err := s.Finish(15 * time.Second); err != nil {
		return "", a.UI.Fail(fmt.Sprintf("port %d is open but no shell answered (it may ask for a login: that is another service, not the toolkit shell).", a.Env.PortTelnet),
			"stop here. The toolkit shell has no login. Check the card and the address.")
	}
	ver, marker := "", ""
	for _, l := range strings.Split(s.Out, "\n") {
		if ver == "" && strings.HasPrefix(l, "Linux version") {
			ver = l
		}
		if marker == "" && strings.HasPrefix(l, "AKAM_") {
			marker = l
		}
	}
	if ver == "" {
		return "", a.UI.Fail(fmt.Sprintf("the shell on port %d did not look like the camera (no Linux version line).", a.Env.PortTelnet), "stop here and check the address.")
	}
	a.UI.Say("  Shell answered: " + ver)
	if marker == "AKAM_PRESENT" {
		return "akam", nil
	}
	return "stock", nil
}

// ---------------------------------------------------------------- backup from the camera

func (a *App) bkLayoutCheck() error {
	if err := a.tn.OK("cat /proc/mtd", 0); err != nil {
		return a.UI.Fail("cannot read /proc/mtd on the camera.", "check the telnet session.")
	}
	var keep []string
	for _, l := range strings.Split(a.tn.Out, "\n") {
		l = strings.TrimRight(l, "\r")
		if strings.HasPrefix(l, "dev:") || strings.TrimSpace(l) == "" {
			continue
		}
		keep = append(keep, l)
	}
	now := strings.Join(keep, "\n")
	if now != camLayout {
		a.UI.Err("the partition table of this camera is not the supported 8-partition layout:")
		for _, l := range keep {
			a.UI.Say("      " + l)
		}
		a.UI.Hint("do NOT continue: this camera model or firmware layout is not supported. Nothing was changed.")
		return ui.ErrFailed
	}
	a.UI.Say("  Partition table: LAYOUT_OK (8 partitions, APP 00500000).")
	return nil
}

var sdMountRe = regexp.MustCompile(`^/[A-Za-z0-9_./-]+$`)

func (a *App) bkFindSD() (string, error) {
	if err := a.tn.OK(`mount | grep mmcblk | sed -n "s/^[^ ]* on \([^ ]*\) .*/\1/p" | head -1`, 0); err != nil {
		return "", a.UI.Fail("cannot list the mounts on the camera.", "")
	}
	sd := strings.TrimSpace(a.tn.Line(1))
	if sd == "" || !sdMountRe.MatchString(sd) {
		return "", a.UI.Fail("the microSD card does not seem to be mounted on the camera.", "power-cycle the camera with the backup card inserted, then run this step again.")
	}
	a.UI.Say(fmt.Sprintf("  The card is mounted at %s on the camera.", sd))
	return sd, nil
}

func (a *App) bkFreeSpace(sd string) error {
	if err := a.tn.OK("df -k "+sd, 0); err != nil {
		return a.UI.Fail("cannot read the free space of the card.", "")
	}
	ls := strings.Split(a.tn.Out, "\n")
	var f []string
	if len(ls) > 1 {
		f = strings.Fields(strings.Join(ls[1:], " "))
	}
	if len(f) < 4 {
		return a.UI.Fail("cannot parse the free space of the card.", "")
	}
	avail, err := strconv.Atoi(f[3])
	if err != nil {
		return a.UI.Fail("cannot parse the free space of the card.", "")
	}
	if avail < 17000 {
		return a.UI.Fail(fmt.Sprintf("the card has only %d KiB free, 17000 KiB are needed for the two dumps.", avail), "use a card with free space.")
	}
	a.UI.Say(fmt.Sprintf("  Free space on the card: %d KiB.", avail))
	return nil
}

func (a *App) bkDumpOne(sd string, r, n int) error {
	if err := a.tn.OK(fmt.Sprintf("dd if=/dev/mtdblock%d of=%s/dump%d/mtdblock%d.bin bs=64k 2>/dev/null", n, sd, r, n), 300*time.Second); err != nil {
		return a.UI.Fail(fmt.Sprintf("reading mtdblock%d failed on the camera (run %d).", n, r), "nothing was changed on the camera; try again.")
	}
	return nil
}

func (a *App) bkDumpRun(sd string, r int) error {
	if err := a.tn.OK(fmt.Sprintf("mkdir -p %s/dump%d", sd, r), 0); err != nil {
		return ui.ErrFailed
	}
	for n := 0; n < 8; n++ {
		a.UI.Say(fmt.Sprintf("  Reading mtdblock%d (run %d of 2) ...", n, r))
		if err := a.bkDumpOne(sd, r, n); err != nil {
			return err
		}
	}
	if err := a.tn.OK("sync", 120*time.Second); err != nil {
		return ui.ErrFailed
	}
	return nil
}

func (a *App) bkSizesCheck(sd string) error {
	if err := a.tn.OK(fmt.Sprintf(`for f in %s/dump1/mtdblock*.bin %s/dump2/mtdblock*.bin; do echo "$(stat -c%%s $f) $f"; done`, sd, sd), 0); err != nil {
		return a.UI.Fail("cannot read the dump sizes on the camera.", "")
	}
	for r := 1; r <= 2; r++ {
		for n, want := range camMtdSizes {
			re := regexp.MustCompile(fmt.Sprintf(`(?m)^([0-9]*) .*/dump%d/mtdblock%d\.bin\r?$`, r, n))
			got := ""
			if m := re.FindStringSubmatch(a.tn.Out); m != nil {
				got = m[1]
			}
			if got != strconv.FormatInt(want, 10) {
				return a.UI.Fail(fmt.Sprintf("dump%d mtdblock%d is %s bytes, expected %d.", r, n, got, want), "the read is not usable; stop and try again.")
			}
		}
	}
	a.UI.Say("  Sizes of the 16 dump files are exact.")
	return nil
}

type bkHashes [3][8]string

func (a *App) bkHashRun(x *telnet.Xfer, sd string, r int, h *bkHashes) error {
	a.UI.Say(fmt.Sprintf("  Hashing dump%d on the camera (%s, this takes a moment) ...", r, x.HT))
	if err := a.tn.OK(fmt.Sprintf("cd %s/dump%d && %s mtdblock*.bin | tee %s/sums_run%d.%s", sd, r, x.HT, sd, r, x.HT), 600*time.Second); err != nil {
		return a.UI.Fail(fmt.Sprintf("hashing dump%d failed on the camera.", r), "")
	}
	re := regexp.MustCompile(`^([0-9a-f]+)\s+\*?mtdblock([0-7])\.bin$`)
	for _, l := range strings.Split(a.tn.Out, "\n") {
		if m := re.FindStringSubmatch(strings.TrimSpace(l)); m != nil {
			n, _ := strconv.Atoi(m[2])
			h[r][n] = m[1]
		}
	}
	return nil
}

// bkCompare: the two runs must be identical. mtdblock6 (CONFIG, mounted read/write on the camera) may
// differ: it is dumped again, twice at most. Any other difference is fatal.
func (a *App) bkCompare(x *telnet.Xfer, sd string, h *bkHashes) (configDiffers bool, err error) {
	redo := 0
	for {
		var bad []int
		for n := 0; n < 8; n++ {
			if h[1][n] == "" || h[1][n] != h[2][n] {
				bad = append(bad, n)
			}
		}
		if len(bad) == 0 {
			a.UI.Good("  READS_IDENTICAL: both dumps match on all 8 partitions.")
			return configDiffers, nil
		}
		if len(bad) != 1 || bad[0] != 6 {
			return false, a.UI.Fail(fmt.Sprintf("the two reads differ on mtdblock%d (not CONFIG only): the read is not reliable.", bad[0]), "do not use these dumps; try again.")
		}
		if redo >= 2 {
			a.UI.Warn("mtdblock6 (CONFIG) still differs between the two reads. It is a live read/write filesystem and Akam never uses it: continuing with run 1.")
			return true, nil
		}
		redo++
		a.UI.Warn(fmt.Sprintf("only mtdblock6 (CONFIG, read/write while the camera runs) differs: reading it again (%d of 2).", redo))
		if err := a.bkDumpOne(sd, 1, 6); err != nil {
			return false, err
		}
		if err := a.bkDumpOne(sd, 2, 6); err != nil {
			return false, err
		}
		if err := a.bkHashRun(x, sd, 1, h); err != nil {
			return false, err
		}
		if err := a.bkHashRun(x, sd, 2, h); err != nil {
			return false, err
		}
	}
}

// bkFingerprint: kernel version against the supported list (information; the APP hash decides later).
func (a *App) bkFingerprint() string {
	if err := a.tn.OK("uname -r; uname -v", 0); err != nil {
		a.UI.Warn("could not read the kernel version.")
		return ""
	}
	kfp := strings.TrimRight(a.tn.Line(1), "\r") + " " + strings.TrimRight(a.tn.Line(2), "\r")
	a.UI.Say("  Kernel: " + kfp)
	b, _ := assets.Script("supported_hardware.txt")
	known := false
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(strings.TrimSpace(l), "#") {
			continue
		}
		f := strings.Split(l, "|")
		if len(f) >= 4 && f[0] == "validated" && f[3] == kfp {
			known = true
		}
	}
	if known {
		a.UI.Say("  The kernel build matches a validated entry of the supported hardware list.")
	} else {
		a.UI.Warn("this kernel build is not on the validated list (tools/release/supported_hardware.txt).")
	}
	return kfp
}

// bkPCDir is bk_pc_dir: a new folder <Documents>/Akam backups/<camera>/<date>[-N] (specs/installer.md)
// and the folders above it that must be private too.
// With AKAM_BACKUP_ROOT (test hook) it is <root>/akam-backup-<name>-<date>[-N], the legacy layout.
func (a *App) bkPCDir() (string, []string) {
	base, cloud, fold, parents := a.backupBase(time.Now())
	if cloud != "" {
		a.UI.Warn(fmt.Sprintf("your Documents folder is inside %s, which copies files to the cloud and the backup holds your Wi-Fi password.", cloud))
		a.UI.Say("  The suggested folder is in your home folder instead.")
	}
	exists := func(p string) bool { _, err := os.Stat(p); return err == nil }
	if fold {
		exists = backuploc.FoldedSet(backupNames(filepath.Dir(base)))
	}
	return backuploc.NextFree(base, exists), parents
}

func (a *App) bkWritePCFiles(dir, ip, kernel, ht string, configDiffers bool) {
	var sums strings.Builder
	for n := 0; n < 8; n++ {
		f := fmt.Sprintf("mtdblock%d.bin", n)
		fmt.Fprintf(&sums, "%s  %s\n", fileSHA(filepath.Join(dir, f)), f)
	}
	_ = fsutil.WritePrivateFile(filepath.Join(dir, "SHA256SUMS"), []byte(sums.String()))
	readme := fmt.Sprintf("Akam backup of the camera at %s, made %s by %s backup.\n"+
		"mtdblock0..7.bin are the 8 flash partitions of the ORIGINAL firmware. This is the only way back.\n"+
		"Keep a second copy on a different medium. NEVER publish these files: they hold proprietary firmware.\n"+
		"Check them with: sha256sum -c SHA256SUMS\n", ip, time.Now().UTC().Format("2006-01-02T15:04:05Z"), Prog)
	_ = fsutil.WritePrivateFile(filepath.Join(dir, "README.txt"), []byte(readme))
	if kernel == "" {
		kernel = "unknown"
	}
	diff := "0"
	if configDiffers {
		diff = "1"
	}
	info := fmt.Sprintf("camera: %s\nkernel: %s\ncamera_hash_tool: %s\nconfig_partition_differs_between_reads: %s\n--- /proc/mtd\n%s\n", ip, kernel, ht, diff, camLayout)
	_ = fsutil.WritePrivateFile(filepath.Join(dir, "camera_info.txt"), []byte(info))
	a.backupSecure(dir, true)
}

func (a *App) bkManualFallbackHint() {
	a.UI.Say("  The automatic transfer did not work. You still have the dumps on the card:")
	a.UI.Say("  power the camera off, put the card in the PC, copy dump1/ to a folder, then run:")
	a.UI.Say(fmt.Sprintf("      %s backup --backup-dir THAT_FOLDER", Prog))
	a.UI.Say("  (docs/manual/backup-sd.md section 9 shows how to check the copy)")
}

// backupCamera is backup_camera.
func (a *App) backupCamera() error {
	if a.St.Get("camera_kind") == "akam" {
		return a.UI.Fail("this camera already runs Akam: its flash no longer holds the original firmware.",
			fmt.Sprintf("the stock backup must have been made before the first flash. Give it with: %s backup --backup-dir DIR", Prog))
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if err := a.camSessionOpen(ip); err != nil {
		return err
	}
	defer a.camClose()
	if err := a.bkLayoutCheck(); err != nil {
		return err
	}
	kernel := a.bkFingerprint()
	sd, err := a.bkFindSD()
	if err != nil {
		return err
	}
	if err := a.bkFreeSpace(sd); err != nil {
		return err
	}
	x := a.xfer()
	if err := x.HashTool(); err != nil {
		return a.UI.Fail("cannot tell which hash tool the camera has.", "")
	}
	_ = a.tn.Exec(fmt.Sprintf("mount > %s/state_mounts.txt; ls -la /etc/config > %s/state_etc_config.txt; ps > %s/state_ps.txt", sd, sd, sd), 0)
	for r := 1; r <= 2; r++ {
		if err := a.bkDumpRun(sd, r); err != nil {
			return err
		}
	}
	if err := a.bkSizesCheck(sd); err != nil {
		return err
	}
	var h bkHashes
	if err := a.bkHashRun(x, sd, 1, &h); err != nil {
		return err
	}
	if err := a.bkHashRun(x, sd, 2, &h); err != nil {
		return err
	}
	configDiffers, err := a.bkCompare(x, sd, &h)
	if err != nil {
		return err
	}
	pcdir, parents := a.bkPCDir()
	a.UI.Say("  Copying the verified dump to this PC: " + pcdir)
	if probs := a.backupValidate(pcdir); len(probs) > 0 {
		return a.UI.Fail("cannot use "+pcdir+": "+strings.Join(probs, "; ")+".", "free some space or choose another folder: "+Prog+" backup --backup-dir DIR after a manual copy.")
	}
	// the folders are private before any secret is written; the date folder is made with a plain
	// Mkdir, so one that appeared since the check is refused instead of taken over
	if len(parents) > 0 {
		if err := fsutil.PrivateMkdirChain(parents...); err != nil {
			return a.UI.Fail("cannot create "+filepath.Dir(pcdir)+": "+err.Error(), "")
		}
		if err := fsutil.PrivateMkdirNew(pcdir); err != nil && !os.IsExist(err) {
			return a.UI.Fail("cannot create "+pcdir+": "+err.Error(), "")
		} else if err != nil {
			return a.UI.Fail(pcdir+" appeared while the backup was running.", "run the backup again.")
		}
	} else if err := fsutil.PrivateMkdir(pcdir); err != nil {
		return a.UI.Fail("cannot create "+pcdir+": "+err.Error(), "")
	}
	if !a.backupSecure(pcdir, false) {
		a.UI.Say("  Private: no (this medium has no permissions)")
		if !a.confirm("  The backup holds your Wi-Fi password. Keep it on this medium anyway?") {
			_ = os.Remove(pcdir)
			return a.UI.Fail("the backup folder cannot be made private.", "choose a folder on a disk that has permissions.")
		}
	}
	for n := 0; n < 8; n++ {
		if err := x.Get(fmt.Sprintf("%s/dump1/mtdblock%d.bin", sd, n), filepath.Join(pcdir, fmt.Sprintf("mtdblock%d.bin", n)), camMtdSizes[n], h[1][n]); err != nil {
			a.bkManualFallbackHint()
			return ui.ErrFailed
		}
	}
	a.camClose()
	a.bkWritePCFiles(pcdir, ip, kernel, x.HT, configDiffers)
	a.UI.Good("  Backup copied and verified (every file matches the camera's two reads). SHA256SUMS written.")
	app := fileSHA(filepath.Join(pcdir, "mtdblock7.bin"))
	if !a.camConfirmUntested(app, "Continue with untested firmware?") {
		a.UI.Say(fmt.Sprintf("  The verified copy stays in %s (accept it later with: %s backup --backup-dir %s).", pcdir, Prog, pcdir))
		return ui.ErrFailed
	}
	return a.backupAccept(pcdir)
}

// backupAccept is backup_accept: final checks and the second-copy gate, then record it.
func (a *App) backupAccept(d string) error {
	st, err := os.Stat(d)
	if err != nil || !st.IsDir() {
		return a.UI.Fail("not a folder: "+d, "")
	}
	d, _ = filepath.Abs(d)
	if !a.backupCheck(d) {
		return ui.ErrFailed
	}
	private := a.backupSecure(d, hasOtherFiles(d))
	a.backupSecretsWarning(d, private)
	a.UI.Warn("COPY THIS FOLDER TO A SECOND MEDIUM (another disk or USB stick) BEFORE continuing.")
	a.UI.Warn("It is the only way back to the original firmware. Never publish it: it holds proprietary firmware.")
	if cp := a.Opt.BackupCopy; cp != "" {
		if err := a.backupCopyTo(d, cp); err != nil {
			return err
		}
		if !a.confirm(fmt.Sprintf("  Is %s on a different medium (another disk or USB stick)?", cp)) {
			return a.UI.Fail("second medium not confirmed.", "copy the folder to another disk, then run this step again.")
		}
	} else if !a.confirm("  Have you made a second copy on a different medium?") {
		return a.UI.Fail("second copy not confirmed.", "make the copy, then run this step again.")
	}
	_ = a.St.Set("backup_dir", d)
	_ = a.St.Set("backup_sha", fileSHA(filepath.Join(d, "mtdblock7.bin")))
	_ = a.St.MarkDone("backup")
	a.UI.Good("  Backup folder accepted: " + d)
	return nil
}

// hasOtherFiles reports whether the folder holds anything besides the backup's own files: such a
// folder is not chmod-ed (it may be a home folder or a medium's root), only the backup files are.
func hasOtherFiles(d string) bool {
	own := map[string]bool{"SHA256SUMS": true, "README.txt": true, "camera_info.txt": true}
	for n := 0; n < 8; n++ {
		own[fmt.Sprintf("mtdblock%d.bin", n)] = true
	}
	es, _ := os.ReadDir(d)
	for _, e := range es {
		if !own[e.Name()] {
			return true
		}
	}
	return false
}

// backupCopyTo copies the backup folder and checks the copy with SHA256SUMS.
func (a *App) backupCopyTo(src, dest string) error {
	if dest == src || strings.HasPrefix(dest, src+string(filepath.Separator)) {
		return a.UI.Fail("the copy must be outside "+src, "")
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD copy mtdblock0..7.bin and SHA256SUMS to %s", dest))
		return nil
	}
	_, derr := os.Stat(dest)
	pre := derr == nil
	if err := fsutil.PrivateMkdir(dest); err != nil {
		return a.UI.Fail("copying the backup failed.", "")
	}
	if !pre && !a.backupSecure(dest, false) { // private before the first byte goes in
		a.UI.Warn("the second copy's folder is NOT private (its medium has no permissions): keep that medium to yourself.")
	}
	names := []string{"SHA256SUMS"}
	for n := 0; n < 8; n++ {
		names = append(names, fmt.Sprintf("mtdblock%d.bin", n))
	}
	for _, n := range names {
		b, err := os.ReadFile(filepath.Join(src, n))
		if err != nil || fsutil.WritePrivateFile(filepath.Join(dest, n), b) != nil {
			return a.UI.Fail("copying the backup failed.", "")
		}
	}
	if !sha256sumCheck(dest, filepath.Join(dest, "SHA256SUMS")) {
		return a.UI.Fail(fmt.Sprintf("the copy in %s does not match SHA256SUMS.", dest), "copy it again to a different medium.")
	}
	a.backupSecure(dest, pre)
	a.UI.Good(fmt.Sprintf("  Copy checked: %s matches SHA256SUMS.", dest))
	return nil
}

// cmdBackup is cmd_backup.
func (a *App) cmdBackup() error {
	if a.Opt.BackupDir != "" {
		a.stepBanner("backup")
		return a.backupAccept(a.Opt.BackupDir)
	}
	if a.St.Get("camera_kind") == "akam" { // Akam runs: read it over SSH (T11.11)
		a.stepBanner("backup")
		return a.backupSSH()
	}
	if err := a.stepNeed("backup"); err != nil {
		return err
	}
	a.stepBanner("backup")
	if a.UI.Dry() {
		for _, l := range []string{
			"  WOULD open the telnet shell on the camera and: check /proc/mtd is the 8-partition layout (refuse otherwise),",
			"  read the kernel version, find the card and its free space, dd the 8 partitions twice,",
			"  check the 16 sizes, hash both runs on the camera and compare them (READS_IDENTICAL),",
			"  copy dump1 to <Documents>/Akam backups/<name>/<date>/ over nc with size+hash checks (3 tries each),",
			"  write SHA256SUMS, compare the stock APP hash with tools/release/supported_hardware.txt",
			"  (a yes/no in a red box, default No, unless validated) and ask for a second copy on another medium."} {
			a.UI.Say(l)
		}
		_ = a.St.Set("backup_dir", "<backup-dir>")
		_ = a.St.Set("backup_sha", "dry-run")
		_ = a.St.MarkDone("backup")
		return nil
	}
	return a.backupCamera()
}

// ---------------------------------------------------------------- flash over the maintenance telnet

var ftSkipRe = regexp.MustCompile(`^\s*$|^\s*[0-9]+%`)
var imgNameRe = regexp.MustCompile(`^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs$`)

func (a *App) ftFindCard() error {
	if err := a.tn.OK(`if [ -d /mnt/custom_firmware ]; then echo CARD_ON_MNT; elif [ -d /tmp/sd/custom_firmware ]; then echo CARD_ON_TMPSD; else echo CARD_NONE; fi`, 0); err != nil {
		return a.UI.Fail("cannot look for the flash card on the camera.", "")
	}
	switch strings.TrimSpace(a.tn.Line(1)) {
	case "CARD_ON_MNT":
		a.UI.Say("  The flash card files are already on /mnt.")
	case "CARD_ON_TMPSD":
		a.UI.Say("  The card is on /tmp/sd; binding it to /mnt (every script reads /mnt).")
		if err := a.tn.OK("mount --bind /tmp/sd /mnt", 0); err != nil {
			return a.UI.Fail("mount --bind /tmp/sd /mnt failed.", "")
		}
	default:
		return a.UI.Fail("no custom_firmware folder on the card in the camera.",
			fmt.Sprintf("put the flash card (%s step prepare-flash-sd) in the camera, power-cycle it and wait 2 minutes.", Prog))
	}
	return nil
}

// ftRemoteMD5 returns the md5 of a file on the camera ("" when missing).
func (a *App) ftRemoteMD5(path string) (string, error) {
	if err := a.tn.Exec(fmt.Sprintf("md5sum '%s' 2>/dev/null", path), 120*time.Second); err != nil {
		return "", err
	}
	if a.tn.RC == 0 {
		if f := strings.Fields(a.tn.Line(1)); len(f) > 0 {
			return f[0], nil
		}
	}
	return "", nil
}

func md5hex(b []byte) string { return fmt.Sprintf("%x", md5sum(b)) }

// ftEnsureFile: the file on the card must equal data; push it if not (after a yes).
func (a *App) ftEnsureFile(x *telnet.Xfer, data []byte, remote string, exec bool, label string) error {
	l := md5hex(data)
	r, err := a.ftRemoteMD5(remote)
	if err != nil {
		return ui.ErrFailed
	}
	if r == l {
		a.UI.Say(fmt.Sprintf("  %s on the card: md5 matches this PC.", label))
		return nil
	}
	if r == "" {
		a.UI.Warn(label + " is missing on the card.")
	} else {
		a.UI.Warn(label + " on the card is not the one from this PC.")
	}
	if !a.confirm(fmt.Sprintf("  Push %s from this PC to %s over the network?", label, remote)) {
		return a.UI.Fail(label+" on the card does not match.", fmt.Sprintf("prepare the flash card again (%s step prepare-flash-sd).", Prog))
	}
	if err := x.PutData(label, data, remote); err != nil {
		return err
	}
	if exec {
		if err := a.tn.OK(fmt.Sprintf("chmod +x '%s'", remote), 0); err != nil {
			return ui.ErrFailed
		}
	}
	return nil
}

func (a *App) ftEnsureImage(x *telnet.Xfer, imgPath string, img []byte, md5 string) error {
	if err := a.tn.OK(`for f in /mnt/custom_firmware/*; do if [ -e "$f" ]; then echo "${f##*/}"; fi; done`, 0); err != nil {
		return a.UI.Fail("cannot list /mnt/custom_firmware.", "")
	}
	var names []string
	for _, l := range strings.Split(a.tn.Out, "\n") {
		if l = strings.TrimSpace(l); l != "" {
			names = append(names, l)
		}
	}
	name := ""
	for _, n := range names {
		if imgNameRe.MatchString(n) {
			name = n
		}
	}
	why := "found: " + strings.Join(names, " ") + " "
	if len(names) == 1 && name != "" {
		r, err := a.ftRemoteMD5("/mnt/custom_firmware/" + name)
		if err != nil {
			return ui.ErrFailed
		}
		if r == md5 {
			a.UI.Say(fmt.Sprintf("  Image on the card: %s, md5 %s (same as this PC).", name, md5))
			return nil
		}
		if r == "" {
			r = "unreadable"
		}
		why = fmt.Sprintf("%s has md5 %s on the card, this PC's image has %s", name, r, md5)
	}
	a.UI.Warn(fmt.Sprintf("the card does not carry exactly the image you want to flash (%s).", why))
	if !a.confirm("  Replace the image on the card with this PC's image?") {
		return a.UI.Fail("the image on the card does not match.", fmt.Sprintf("prepare the flash card again (%s step prepare-flash-sd).", Prog))
	}
	name = filepath.Base(imgPath)
	if !imgNameRe.MatchString(name) {
		name = "ak_rtsp_firmware_" + time.Now().UTC().Format("20060102_150405") + ".squashfs"
	}
	if err := a.tn.OK("rm -f /mnt/custom_firmware/ak_rtsp_firmware_*.squashfs", 0); err != nil {
		return a.UI.Fail("cannot clear the old image on the card.", "")
	}
	return x.PutData(name, img, "/mnt/custom_firmware/"+name)
}

// flashTelnet is flash_telnet: from the stock or maintenance telnet, with the card on /mnt. img is the
// image file and md5 its md5 (already gated); label is Flashing or Restoring.
func (a *App) flashTelnet(ip, imgPath, md5, label string) error {
	if a.UI.Dry() {
		for _, l := range []string{
			"  WOULD open the telnet shell, bind the card on /mnt if it is on /tmp/sd, check that exactly one image is in",
			"  /mnt/custom_firmware with the same md5 as this PC (pushing files over nc only after your yes), then run",
			"  'sh /mnt/install_with_flash_tool.sh', show its md5 line and its prompt, and ask YOU a yes/no (default No); only after your yes does the wizard answer the script.",
			"  When flashing it also runs 'sh /mnt/preflight_check.sh --ensure-wifi' on the camera: if /etc/config/wpa_supplicant.conf has no",
			"  network it is created from the card's wifi.conf (on the camera; the password never passes through this PC).",
			"  After the write: dd if=/dev/mtd7 bs=64k | md5sum on the camera must equal the image md5. No reboot is ever sent."} {
			a.UI.Say(l)
		}
		a.UI.Say("WOULD ask: Write the camera flash now? [y/N] (default: No) -- Write the camera flash: <camera ip> <image md5>")
		return nil
	}
	ftPath := a.flashToolPath()
	tool, err := os.ReadFile(ftPath)
	if err != nil || len(tool) == 0 {
		return a.UI.Fail("flash_tool not found: "+ftPath, fmt.Sprintf("run %s get-kit again (or %s build).", Prog, Prog))
	}
	// everything sent to the card is read ONCE, here; the md5 compared and the bytes sent are the same
	img, err := os.ReadFile(imgPath)
	if err != nil || fmt.Sprintf("%x", md5sum(img)) != md5 {
		return a.UI.Fail("the image changed after the check: refusing.", "run the step again.")
	}
	if err := a.camSessionOpen(ip); err != nil {
		return err
	}
	closed := false
	defer func() {
		if !closed {
			a.camClose()
		}
	}()
	// Same layout gate as the backup: never aim /dev/mtd7 at a camera whose table is not the supported one.
	if err := a.bkLayoutCheck(); err != nil {
		return err
	}
	if err := a.ftFindCard(); err != nil {
		return err
	}
	x := a.xfer()
	if err := a.ftEnsureImage(x, imgPath, img, md5); err != nil {
		return err
	}
	if err := a.ftEnsureFile(x, tool, "/mnt/flash_tool", true, "flash_tool"); err != nil {
		return err
	}
	for _, n := range []string{"install_with_flash_tool.sh", "flash_guard.sh"} {
		b, err := a.embeddedScript(n)
		if err != nil {
			return a.UI.Fail(n+" is not available: "+err.Error(), "")
		}
		if err := a.ftEnsureFile(x, b, "/mnt/"+n, false, n); err != nil {
			return err
		}
	}
	if label == "Flashing" {
		// Akam does not bring Wi-Fi up by itself: the saved network must exist before the write (hard gate).
		b, err := a.embeddedScript("preflight_check.sh")
		if err != nil {
			return a.UI.Fail("preflight_check.sh is not available: "+err.Error(), "")
		}
		if err := a.ftEnsureFile(x, b, "/mnt/preflight_check.sh", false, "preflight_check.sh"); err != nil {
			return err
		}
		a.UI.Say("  Checking that the camera will keep its Wi-Fi after the card is removed ...")
		if err := a.tn.Exec("sh /mnt/preflight_check.sh --ensure-wifi", 60*time.Second); err != nil {
			a.camClose()
			closed = true
			a.UI.Say("  Nothing was written to the camera.")
			return a.UI.Fail("cannot run preflight_check.sh on the camera.", "check the telnet session.")
		}
		if !a.wifiVerdict(a.tn.Out) {
			a.camClose()
			closed = true
			a.UI.Say("  Nothing was written to the camera.")
			return ui.ErrFailed
		}
	}
	op := a.op()
	defer op.end()
	op.plan("Check the camera and the card (nothing is written)", "Confirm in the red box", "Write the flash", "Read the partition back")
	op.done(0, "nothing was written")
	op.start(1)
	a.UI.Blank()
	a.UI.Warn(label + " writes the camera's flash. If the power is cut while it writes, only a UART cable can recover the camera.")
	a.UI.Warn("Keep the camera powered and do not unplug anything until this step says it is finished.")
	a.UI.Say("  Image md5 (this PC and the card): " + md5)
	a.tn.StreamSet(func(l string) { a.UI.Say("    | " + l) }, ftSkipRe, func(lbl string, pct int) { a.UI.Progress(lbl, pct) })
	defer a.tn.StreamSet(nil, nil, nil)
	// HUP is ignored: if the telnet link drops while it writes, the kernel's hangup must not kill flash_tool.
	if err := a.tn.Start("trap '' HUP; sh /mnt/install_with_flash_tool.sh"); err != nil {
		return ui.ErrFailed
	}
	if a.tn.IsManual() {
		a.tn.StreamSet(nil, nil, nil)
		if err := a.flashTelnetManual(md5); err != nil {
			return err
		}
	} else {
		res := a.tn.WaitPrompt("Type YES to continue: ", 60*time.Second)
		a.tn.StreamSet(nil, nil, nil)
		if res != telnet.PromptSeen {
			a.camClose()
			closed = true
			return a.UI.Fail(fmt.Sprintf("the flash script did not reach its confirmation prompt (code %d).", int(res)), "nothing was written. Read the lines above.")
		}
		line := ""
		for _, l := range strings.Split(a.tn.Out, "\n") {
			if f := strings.Fields(l); len(f) > 0 && regexp.MustCompile(`^[0-9a-f]{32}$`).MatchString(f[0]) {
				line = f[0]
				break
			}
		}
		if line != md5 {
			a.UI.Err(fmt.Sprintf("the md5 printed by the camera ('%s') is not the image md5 (%s).", line, md5))
			_ = a.tn.SendLine("NO")
			_ = a.tn.Finish(15 * time.Second)
			return ui.ErrFailed
		}
		a.UI.Say("  The camera shows the same md5 as this PC. Its script now waits for a confirmation.")
		conf, cerr := flash.NewConfirmation(md5, func() (bool, error) { return a.flashConfirm(label, ip, md5, "the maintenance telnet") })
		if cerr != nil {
			a.UI.Err("not confirmed. Aborting; nothing was written.")
			_ = a.tn.SendLine("NO")
			_ = a.tn.Finish(15 * time.Second)
			return ui.ErrFailed
		}
		// The only place where text goes to the flash script's prompt: "YES", sent only after the explicit yes above.
		// The write lock goes on before the "YES" leaves: from that byte on the write can start.
		op.writing(true)
		stop := ignoreSignals()
		if err := a.sendYES(conf, md5); err != nil {
			stop()
			op.writing(false)
			return a.UI.Fail("lost the camera session.", "DO NOT power-cycle; check the camera.")
		}
		a.tn.StreamSet(func(l string) { a.UI.Say("    | " + l) }, ftSkipRe, func(lbl string, pct int) { a.UI.Progress(lbl, pct) })
		// The write is under way: Ctrl-C, hangup or kill of this wizard must not end it half way.
		op.done(1, "confirmed")
		op.start(2)
		op.status("camera script install_with_flash_tool.sh writes /dev/mtd7. It cannot be interrupted safely.")
		a.UI.Warn("Writing. Do NOT press Ctrl-C and do NOT unplug anything until this says it is finished.")
		ferr := a.tn.Finish(900 * time.Second)
		stop()
		a.tn.StreamSet(nil, nil, nil)
		a.UI.ProgressClose()
		op.writing(false)
		if ferr == nil {
			op.done(2, "the script finished")
		}
		if ferr != nil {
			_ = a.UI.Fail(fmt.Sprintf("the flash script did not finish (no end of output, %v).", ferr),
				"DO NOT power-cycle yet. Read the lines above; docs/manual/flash-and-restore.md (Recovery).")
			if errors.Is(ferr, telnet.ErrTimeout) {
				a.UI.Warn("no output for a long time: the write may STILL be running. The session is left open and nothing is sent again;")
				a.UI.Warn(fmt.Sprintf("wait several minutes, then read the partition back by hand: dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum (must be %s).", md5))
				closed = true // left open on purpose
			}
			return ui.ErrFailed
		}
		a.ftNoteScriptResult()
	}
	// The authoritative check is the partition itself, never the wording of the script's last line.
	op.start(3)
	op.status("dd if=/dev/mtd7 bs=64k | md5sum   (runs on the camera, read only)")
	a.UI.Say("  Reading the partition back to check it ...")
	if err := a.tn.Exec("dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum", 600*time.Second); err != nil {
		return a.UI.Fail("cannot read /dev/mtd7 back.", "DO NOT power-cycle.")
	}
	sums := ""
	if f := strings.Fields(a.tn.Line(1)); len(f) > 0 {
		sums = f[0]
	}
	a.camClose()
	closed = true
	if sums != md5 {
		return a.UI.Fail(fmt.Sprintf("the partition read back as md5 '%s', the image is %s.", sums, md5),
			"DO NOT power-cycle. Run the flash again from this step or restore (docs/manual/flash-and-restore.md).")
	}
	op.done(3, "md5 matches the image")
	a.UI.Good(fmt.Sprintf("  Read-back md5 of /dev/mtd7 = image md5 (%s).", md5))
	return nil
}

// sendYES types "YES" to the camera's flash script. It needs the human's Confirmation for this image.
func (a *App) sendYES(conf flash.Confirmation, md5 string) error {
	if !conf.For(md5) {
		return errors.New("no confirmation for this image")
	}
	return a.tn.SendLine("YES")
}

func (a *App) ftNoteScriptResult() {
	switch {
	case a.tn.OutHas("completed successfully") && a.tn.RC == 0:
		a.UI.Say("  The flash script reported: completed successfully.")
	case a.tn.RC == 0:
		a.UI.Warn("the flash script ended without its success line; the read-back below decides.")
	default:
		a.UI.Warn(fmt.Sprintf("the flash script exited with code %d; the read-back below decides.", a.tn.RC))
	}
}

// flashTelnetManual: manual mode: the user runs the script and types YES in their own window.
func (a *App) flashTelnetManual(md5 string) error {
	a.UI.Say("  MANUAL: in your telnet window run the command above, compare the md5 it prints with this one:")
	a.UI.Say("      " + md5)
	a.UI.Say("  If it matches, type YES yourself at its prompt (in your own window). Wait until it says 'completed successfully'.")
	a.UI.Say("  Do NOT unplug anything. Then come back and paste its last lines.")
	if err := a.tn.Finish(900 * time.Second); err != nil {
		return ui.ErrFailed
	}
	a.ftNoteScriptResult()
	return nil
}

// ---------------------------------------------------------------- flash command

func (a *App) camAfterFlash() {
	a.afterFlash()
}

// cmdFlash is cmd_flash.
func (a *App) cmdFlash() error {
	if err := a.stepNeed("flash"); err != nil {
		return err
	}
	a.stepBanner("flash")
	if a.UI.Dry() {
		ip, err := a.camNeedIP()
		if err != nil {
			return err
		}
		for _, l := range []string{
			fmt.Sprintf("  WOULD check the image (size %d, md5) and the stock APP hardware verdict (typed", appPartitionBytes),
			"  a yes/no in a red box unless validated), then choose the route:",
			"    - SSH reflash when Akam already runs and your key works (no card needed), or",
			"    - the maintenance telnet with the flash card (stock firmware, or Akam with the maintenance card)."} {
			a.UI.Say(l)
		}
		if err := a.flashTelnet(ip, "dry-run", "dry-run", "Flashing"); err != nil {
			return err
		}
		_ = a.St.Set("flash_md5", "dry-run")
		_ = a.St.MarkDone("flash")
		return nil
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	img := a.St.Get("image_path")
	md5, err := a.imageGateChecked(img)
	if err != nil {
		return err
	}
	if a.St.Get("image_sha") != fileSHA(img) {
		return a.UI.Fail("the image changed since it was built.", fmt.Sprintf("run %s step image again.", Prog))
	}
	bk := a.St.Get("backup_dir")
	app := fileSHA(filepath.Join(bk, "mtdblock7.bin"))
	if app == "" {
		return a.UI.Fail(fmt.Sprintf("the stock APP backup (%s/mtdblock7.bin) is not readable, so the hardware cannot be checked.", bk),
			fmt.Sprintf("run: %s backup --backup-dir DIR", Prog))
	}
	// the backup gate: the folder is re-hashed now, not trusted from the run that made it
	if p := stockBackupProblem(bk); p != "" {
		return a.UI.Fail("the stock backup does not verify: "+p+".", "this backup is damaged or not the original: use your second copy. Nothing was written.")
	}
	if !a.camConfirmUntested(app, "Flash with untested firmware?") {
		return ui.ErrFailed
	}
	route := "telnet"
	if a.St.Get("camera_kind") == "akam" || a.St.Get("camera_telnet") != "1" {
		if c, derr := a.dial(ip); derr == nil {
			c.Close()
			a.UI.Say("  Akam already runs and SSH works: the card is not needed.")
			if ok, _ := a.UI.AskYN("  Reflash over SSH (recommended)? [no = use the maintenance telnet and the card]"); ok {
				route = "ssh"
			}
		} else if a.St.Get("camera_telnet") != "1" {
			return a.UI.Fail(fmt.Sprintf("no way in: SSH with %s does not work and the maintenance telnet is closed.", a.keyPath()),
				fmt.Sprintf("check the key (--identity FILE), or put the first-boot card in the camera and run %s step find-camera.", Prog))
		}
	}
	a.UI.Say("  Route: " + route)
	if route == "ssh" {
		err = a.flashSSH(ip, img, md5, "Flashing")
	} else {
		err = a.flashTelnet(ip, img, md5, "Flashing")
	}
	if err != nil {
		return err
	}
	_ = a.St.Set("flash_md5", md5)
	_ = a.St.Set("flash_route", route)
	_ = a.St.ClearDone("verify")
	_ = a.St.MarkDone("flash")
	a.fwRecord(img)
	a.afterFlash()
	return nil
}
