package cli

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// cameras, rename-camera, register and the first-run questions (cameras.sh).

func (a *App) askCameraName() (string, bool) {
	a.UI.Say(fmt.Sprintf("  Give this camera a short name, for example 'salon' (lowercase letters, digits and '-', at most %d).", state.NameMax))
	a.UI.Say("  It is used for the backup and image folders and as the camera's network name. Several cameras can be set up, each with its own progress.")
	for {
		v, err := a.UI.Prompt("  Camera name [default]:", "default")
		if err != nil {
			return "", false
		}
		if state.NameValid(v) {
			return v, true
		}
		a.UI.Say(fmt.Sprintf("  '%s' is not valid: use lowercase letters, digits and '-' (at most %d, not starting or ending with '-').", v, state.NameMax))
	}
}

// firstRunName: on the very first run of the interactive menu, ask for the camera's name.
func (a *App) firstRunName() bool {
	if !a.Cams.Fresh || a.Opt.DryRun || a.Opt.Camera != "" || !a.UI.Interactive() {
		return false
	}
	n, ok := a.askCameraName()
	if !ok {
		return false
	}
	a.Cams.Name = n
	return true
}

// firstRunLegacy: progress saved by an earlier version belongs to one camera: ask its name.
func (a *App) firstRunLegacy() bool {
	if a.Opt.DryRun || a.Opt.Camera != "" || !a.UI.Interactive() || !a.Cams.HasLegacy() {
		return false
	}
	a.UI.Say("  The progress saved by an earlier version belongs to one camera. What is its name?")
	a.UI.Say("  (Enter keeps 'default'. Choose the name you want to see in the menu and in the folder names.)")
	n, ok := a.askCameraName()
	if !ok {
		return false
	}
	if n != "default" {
		if err := a.Cams.MoveFiles("default", n); err != nil {
			a.UI.Warn(fmt.Sprintf("a camera called '%s' already has progress: keeping the name 'default'.", n))
			return false
		}
	}
	a.Cams.Name = n
	return true
}

func (a *App) cmdCameras() error {
	a.UI.Bold(fmt.Sprintf("Cameras (state in %s)", a.Cams.Base))
	for _, n := range a.Cams.Known() {
		mark := " "
		if n == a.Cams.Name {
			mark = "*"
		}
		fmt.Fprintf(a.out(), "  %s %-20s %s\n", mark, n, a.Cams.Summary(n))
	}
	a.UI.Say(fmt.Sprintf("  Switch with: %s --camera NAME   (a new name starts a new camera)", Prog))
	return nil
}

// reopen selects the state and log of the current camera again (after a rename).
func (a *App) reopen() error {
	dir := a.Cams.DirOf(a.Cams.Name)
	st, err := state.Open(dir, a.Opt.DryRun, a.Secrets)
	if err != nil {
		return err
	}
	lg, err := state.NewLog(dir, a.Secrets)
	if err != nil {
		return err
	}
	a.St, a.Log = st, lg
	a.Log.Line("CAMERA %s", a.Cams.Name)
	return nil
}

func (a *App) cmdRenameCamera(nw string) error {
	old := a.Cams.Name
	if !state.NameValid(nw) {
		return a.UI.Fail(fmt.Sprintf("'%s' is not a valid camera name.", nw),
			fmt.Sprintf("use lowercase letters, digits and '-' (at most %d, not starting or ending with '-').", state.NameMax))
	}
	if nw == old {
		a.UI.Say(fmt.Sprintf("  This camera is already called %s.", nw))
		return nil
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD rename the camera '%s' to '%s' (its state and logs move to %s).", old, nw, a.Cams.DirOf(nw)))
		return nil
	}
	if _, err := os.Stat(filepath.Join(a.Cams.DirOf(nw), "state")); err == nil {
		return a.UI.Fail(fmt.Sprintf("a camera called '%s' already exists.", nw), fmt.Sprintf("pick another name (%s cameras lists them).", Prog))
	}
	if err := a.Cams.MoveFiles(old, nw); err != nil {
		a.UI.Warn(err.Error())
		return ui.ErrFailed
	}
	a.Cams.Name = nw
	if err := a.reopen(); err != nil {
		return err
	}
	_ = a.Cams.Remember(nw)
	a.UI.Good(fmt.Sprintf("  Camera '%s' is now called '%s'. Backup and image folders already made keep their names.", old, nw))
	return nil
}

// ---- backup folder checks (backup_check in steps_host.sh) ----

const appPartitionBytes = 5242880

var mtdExtraRe = regexp.MustCompile(`^mtdblock[0-9]+.*\.bin$`)

// backupCheck checks the layout of a backup folder; it shows its findings and returns false when
// the layout is wrong.
func (a *App) backupCheck(dir string) bool {
	for i := 0; i < 8; i++ {
		f := fmt.Sprintf("mtdblock%d.bin", i)
		if st, err := os.Stat(filepath.Join(dir, f)); err != nil || st.IsDir() {
			a.UI.Err(fmt.Sprintf("missing %s in %s", f, dir))
			return false
		}
	}
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		n := e.Name()
		if strings.HasPrefix(n, "mtdblock") && strings.HasSuffix(n, ".bin") && len(n) > 8 && n[8] >= '0' && n[8] <= '9' {
			if !(len(n) == len("mtdblock0.bin") && n[8] >= '0' && n[8] <= '7') {
				a.UI.Err(fmt.Sprintf("unexpected file %s: this backup has more than 8 partitions (different MTD layout).", n))
				return false
			}
		}
	}
	if st, err := os.Stat(filepath.Join(dir, "mtdblock7.bin")); err != nil || st.Size() != appPartitionBytes {
		a.UI.Err(fmt.Sprintf("mtdblock7.bin is not %d bytes: the APP partition layout differs from the supported one.", appPartitionBytes))
		return false
	}
	sums := filepath.Join(dir, "SHA256SUMS")
	if _, err := os.Stat(sums); err == nil {
		if !sha256sumCheck(dir, sums) {
			a.UI.Err(fmt.Sprintf("SHA256SUMS does not match the files in %s", dir))
			return false
		}
		a.UI.Say("  SHA256SUMS verified.")
	} else {
		a.UI.Warn(fmt.Sprintf("no SHA256SUMS in %s: integrity of the copy from the camera was not checked.", dir))
	}
	return true
}

// sha256sumCheck is `sha256sum -c SHA256SUMS` run in dir.
func sha256sumCheck(dir, sums string) bool {
	f, err := os.Open(sums)
	if err != nil {
		return false
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	n := 0
	for sc.Scan() {
		l := strings.TrimRight(sc.Text(), "\r")
		if strings.TrimSpace(l) == "" {
			continue
		}
		if len(l) < 66 {
			return false
		}
		want, name := l[:64], strings.TrimPrefix(strings.TrimLeft(l[64:], " "), "*")
		if strings.HasPrefix(l[64:], "  ") {
			name = l[66:]
		}
		if !filepath.IsLocal(name) {
			return false
		}
		h, err := hashFile(filepath.Join(dir, name))
		if err != nil || !strings.EqualFold(h, want) {
			return false
		}
		n++
	}
	return n > 0
}

func hashFile(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// ---- register ----

var regHostRe = regexp.MustCompile(`[^A-Za-z0-9._-]`)

const emptyMD5 = "d41d8cd98f00b204e9800998ecf8427e"

func (a *App) cmdRegister() error {
	a.banner("Register a camera that already runs Akam")
	a.UI.Say("  Why: use update, the video password and the network name on a camera that is already installed. Risk: none (only this PC's notes change).")
	if a.Opt.IP == "" {
		return a.UI.Fail("register needs the camera's address.", fmt.Sprintf("run: %s --camera NAME --ip ADDRESS register [--backup-dir DIR]", Prog))
	}
	ip := a.Opt.IP
	a.camIP = ip
	if !a.UI.Dry() && !validHost(ip) {
		return a.UI.Fail(fmt.Sprintf("'%s' is not a valid address.", ip), "use the camera's IPv4 address.")
	}
	p := a.ports()
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD probe %s (ports %d, %d, %d open and %d closed = Akam), check the backup folder given with --backup-dir, log in over SSH and read /dev/mtd7 back,", ip, p.SSH, p.RTSP, p.CTL, p.Telnet))
		a.UI.Say(fmt.Sprintf("  then record the address and mark find-camera, flash and verify (and backup, with a backup folder) as done for camera '%s'.", a.Cams.Name))
		return nil
	}
	open := a.probe(ip, p.SSH, p.RTSP, p.CTL, p.Telnet)
	a.UI.Say("  Open ports: " + orNone(joinPorts(open)))
	for _, port := range []int{p.SSH, p.RTSP, p.CTL} {
		if !containsInt(open, port) {
			return a.UI.Fail(fmt.Sprintf("%s does not look like a camera running Akam (SSH, video and control ports must answer; port %d does not).", ip, port),
				"check the address in your router, and that the camera has finished booting.")
		}
	}
	if containsInt(open, p.Telnet) {
		return a.UI.Fail(fmt.Sprintf("port %d (maintenance telnet) is open: this camera still runs the toolkit card.", p.Telnet),
			fmt.Sprintf("finish with %s step verify instead, or take the card out and restart it.", Prog))
	}
	bdir := ""
	if a.Opt.BackupDir != "" {
		st, err := os.Stat(a.Opt.BackupDir)
		if err != nil || !st.IsDir() {
			return a.UI.Fail("not a folder: "+a.Opt.BackupDir, "")
		}
		bdir, _ = filepath.Abs(a.Opt.BackupDir)
		if !a.backupCheck(bdir) {
			return ui.ErrFailed
		}
	} else {
		a.UI.Warn(fmt.Sprintf("no --backup-dir: restore and the hardware check need your backup folder. Register it later with: %s --camera %s backup --backup-dir DIR", Prog, a.Cams.Name))
	}
	c, err := a.connect(ip,
		fmt.Sprintf("SSH to root@%s with the key %s does not work, so this camera cannot be checked: nothing was recorded.", ip, a.keyPath()),
		"pass the private key you put on the camera with --identity FILE (and check the address).")
	if err != nil {
		return err
	}
	defer c.Close()
	a.UI.Say(fmt.Sprintf("  SSH with %s works. Reading the camera's name and its APP partition (/dev/mtd7) ...", a.keyPath()))
	res, err := a.run(c, remoteRegister(), nil, "")
	out := ""
	if err == nil && res.Status == 0 {
		out = string(res.Stdout)
	}
	md5, host := "", ""
	for _, l := range strings.Split(out, "\n") {
		if v, ok := strings.CutPrefix(l, "REG_MTD7_MD5="); ok {
			md5 = strings.TrimSpace(v)
		}
		if v, ok := strings.CutPrefix(l, "REG_HOSTNAME="); ok {
			host = regHostRe.ReplaceAllString(v, "")
		}
	}
	if !isLowerHex(md5) {
		md5 = ""
	}
	if len(md5) != 32 || md5 == emptyMD5 {
		return a.UI.Fail("could not read /dev/mtd7 back from the camera, so it cannot be told that it runs Akam: nothing was recorded.",
			"check that the camera has finished booting and that SSH gives a root shell, then try again.")
	}
	hn := host
	if hn == "" {
		hn = "unknown"
	}
	a.UI.Say(fmt.Sprintf("  Camera network name: %s. APP partition (mtd7) md5: %s", hn, md5))
	set := func(k, v string) {
		if err := a.St.Set(k, v); err != nil {
			a.UI.Warn(err.Error())
		}
	}
	set("camera_ip", ip)
	set("flash_md5", md5)
	set("camera_hostname", hn)
	set("camera_kind", "akam")
	set("camera_telnet", "0")
	set("camera_registered", "1")
	_ = a.St.MarkDone("find-camera")
	if bdir != "" {
		set("backup_dir", bdir)
		set("backup_sha", fileSHA(filepath.Join(bdir, "mtdblock7.bin")))
		_ = a.St.MarkDone("backup")
	}
	_ = a.St.MarkDone("flash")
	_ = a.St.MarkDone("verify")
	a.UI.Good(fmt.Sprintf("  Camera '%s' registered at %s as already installed. Use the maintenance tools of the menu: update, video password, network name.", a.Cams.Name, ip))
	return nil
}

func isLowerHex(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f') {
			return false
		}
	}
	return true
}

func containsInt(l []int, v int) bool {
	for _, x := range l {
		if x == v {
			return true
		}
	}
	return false
}

var _ = discover.Akam
