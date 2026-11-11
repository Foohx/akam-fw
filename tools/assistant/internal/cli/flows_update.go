package cli

import (
	"fmt"
	"os"
	"runtime"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The guided flows that chain existing commands: update of an Akam camera (spec 11, 3.5) and first
// install from a stock camera (3.6). They only add the order, the gates and the screens; every write
// to the camera or to a card still goes through flash.Confirmation (the red box of flashConfirm) or the
// SD red box, exactly as the single commands do.

type flowStep struct {
	Title string
	State string // done | todo | next
}

// showFlowSteps prints the step list with a [done] / [todo] tag, like the mockups of the spec.
func (a *App) showFlowSteps(head string, steps []flowStep) {
	a.UI.Blank()
	a.UI.Bold(head)
	for i, s := range steps {
		tag := "[todo]"
		switch s.State {
		case "done":
			tag = "[done]"
		case "next":
			tag = "[next]"
		}
		a.UI.Say(fmt.Sprintf("  %s  %d  %s", tag, i+1, s.Title))
	}
	a.UI.Blank()
}

// akamKind makes sure the chosen camera is known to run Akam (a read-only probe of its ports when the
// state does not say).
func (a *App) akamKind(ip string) string {
	if k := a.St.Get("camera_kind"); k != "" {
		return k
	}
	if a.UI.Dry() {
		return "akam"
	}
	if cam, _, ok := a.probeCamera(ip); ok {
		switch cam.Kind {
		case discover.Akam:
			_ = a.St.Set("camera_kind", "akam")
			return "akam"
		case discover.Stock:
			_ = a.St.Set("camera_kind", "stock")
			return "stock"
		}
	}
	return ""
}

// flowUpdate is "Update this camera": kit, image, backup gate, pre-checks, the red box, the write, and
// the instruction to power-cycle by hand. Nothing here ever power-cycles the camera.
func (a *App) flowUpdate() error {
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	a.banner(fmt.Sprintf("Update %s (%s)", a.Cams.Name, ip))
	if a.UI.Dry() {
		a.showFlowSteps("Update - what will happen (nothing is changed in a rehearsal)", []flowStep{
			{"Kit downloaded and verified (signature, checksums)", "todo"},
			{"Image: --image FILE or the one built from your backup (exactly 5242880 bytes, md5 shown)", "todo"},
			{"Backup gate: a recorded backup whose SHA256SUMS verifies NOW; else a read-only backup over SSH is offered", "todo"},
			{"Pre-checks on the camera (writes nothing)", "todo"},
			{"Write the image: the red box with the md5, one yes/no (default No)  <- the only step that changes the camera", "todo"},
			{"Verify, then YOU power-cycle the camera", "todo"},
		})
		return a.cmdUpdate()
	}
	switch a.akamKind(ip) {
	case "akam":
	case "stock":
		return a.UI.Fail("this camera runs the stock firmware, not Akam: an update is Akam to Akam.", "choose 'Set up a new camera' (item 1) for the first install.")
	default:
		return a.UI.Fail(fmt.Sprintf("%s does not look like an Akam camera (ports %v).", ip, a.scanPorts()), "check the address, or wait a minute if it is starting.")
	}
	a.showFlowSteps("Update "+a.Cams.Name, []flowStep{
		{"Kit downloaded and verified", "next"}, {"Image checked", "todo"}, {"Backup checked", "todo"},
		{"Pre-checks on the camera (writes nothing)", "todo"}, {"Write the image (the red box)", "todo"}, {"Verify, then you power-cycle", "todo"}})

	// 1. kit
	if a.stepStatus("doctor") != "done" {
		if err := a.cmdDoctor(); err != nil {
			return err
		}
	}
	if a.stepStatus("get-kit") != "done" {
		if err := a.cmdGetKit(); err != nil {
			return err
		}
	}
	// 2. image
	img := a.Opt.Image
	if img == "" {
		img = a.St.Get("image_path")
	}
	if img == "" {
		return a.UI.Fail("no image to write.", "build it from your backup on Linux (./akam image from a source checkout) or pass --image FILE.")
	}
	md5, err := a.imageGate(img)
	if err != nil {
		return err
	}
	if err := a.imageSquashCheck(img); err != nil {
		return err
	}
	// 3. the backup gate
	dir, why := a.backupGate()
	if dir == "" {
		a.UI.Warn("no verified backup: " + why + ".")
		a.UI.Say("  A backup over SSH only reads the camera; it saves what is on it now (not the original firmware).")
		ok, _ := a.UI.AskYN("  Back up this camera over SSH now?")
		if !ok {
			return a.UI.Fail("the update needs a backup that verifies first.", "run it again and answer yes, or make a backup (Back up / restore).")
		}
		if err := a.flowBackup(); err != nil {
			return err
		}
		if dir, why = a.backupGate(); dir == "" {
			return a.UI.Fail("still no verified backup: "+why+".", "nothing was written to the camera.")
		}
	}
	a.UI.Good("  Backup checked: " + dir + " (SHA256SUMS ok, re-hashed just now).")
	// 4 to 6. pre-checks, the red box and the write are the update command's; it never power-cycles
	a.Opt.Image = img
	if err := a.cmdUpdate(); err != nil {
		a.UI.Warn("If the write had started: DO NOT power-cycle the camera; the worker keeps running on the camera. Wait, then check it (Check this computer / verify).")
		return err
	}
	a.UI.Say(fmt.Sprintf("  Image md5 written and read back: %s.", md5))
	if ok, _ := a.UI.AskYN("  Has the camera been power-cycled by hand and is it back on the network? Verify it now?"); ok {
		return a.cmdVerify()
	}
	a.UI.Say(fmt.Sprintf("  When it is back: %s verify", Prog))
	return nil
}

// installSteps is the list of the first install, in the order of the spec.
var installSteps = []struct{ step, title string }{
	{"doctor", "Check this computer"},
	{"get-kit", "Get and verify the kit"},
	{"backup-folder", "Choose the backup folder"},
	{"prepare-backup-sd", "Prepare the backup card (will be erased)"},
	{"find-camera", "Find the camera (card in, power on, wait 2 minutes)"},
	{"backup", "Back up the camera (8 partitions read twice, compared, copied here, verified)"},
	{"image", "Build the Akam image from your own backup"},
	{"prepare-flash-sd", "Prepare the flash card (will be erased; Wi-Fi name and password asked here)"},
	{"flash", "Write the camera flash (the red box, the md5, one yes/no)"},
	{"verify", "Check and power-cycle (you unplug and replug the camera yourself)"},
}

func (a *App) installState(step string) string {
	if step == "backup-folder" {
		if a.St.Get("backup_root_ok") != "" || a.St.IsMarked("backup") {
			return "done"
		}
		return "todo"
	}
	if a.stepStatus(step) == "done" {
		return "done"
	}
	return "todo"
}

func (a *App) installList(next string) []flowStep {
	var out []flowStep
	for _, s := range installSteps {
		st := a.installState(s.step)
		if st != "done" && s.step == next {
			st = "next"
		}
		out = append(out, flowStep{s.title, st})
	}
	return out
}

// flowInstall is "Set up a new camera": the nine steps of the state machine in order, resumable (a
// step that is done and still valid is skipped), stopping at the first failure.
func (a *App) flowInstall() error {
	a.banner("Set up a new camera - first install from the stock firmware")
	for {
		next := ""
		for _, s := range installSteps {
			if a.installState(s.step) != "done" {
				next = s.step
				break
			}
		}
		a.showFlowSteps("First install - "+a.Cams.Name, a.installList(next))
		if next == "" {
			a.UI.Good("  Every step is done: this camera runs Akam. Use 'Camera options' to tune it.")
			return nil
		}
		if a.UI.Dry() && a.St.IsMarked(next) {
			return nil
		}
		if err := a.installRun(next); err != nil {
			if err != ui.ErrFailed {
				a.reportFlowError(err)
			}
			a.UI.Say("  Stopped at '" + next + "'. Fix what is shown above and choose 'Set up a new camera' again: it resumes here.")
			return nil
		}
		if a.UI.Dry() && next == "verify" {
			return nil
		}
		if a.installState(next) != "done" {
			a.UI.Say("  '" + next + "' is not finished. Nothing else was started.")
			return nil
		}
	}
}

func (a *App) installRun(step string) error {
	switch step {
	case "backup-folder":
		if a.UI.Dry() {
			a.backupLocationScreen()
			_ = a.St.Set("backup_root_ok", "1")
			return nil
		}
		if !a.backupLocationScreen() {
			return a.UI.Fail("no backup folder chosen.", "the backup is required before anything is written to the camera.")
		}
		return nil
	case "find-camera":
		if !a.UI.Dry() && !a.haveCamera() && !a.needCamera() {
			return a.UI.Fail("no camera chosen.", "")
		}
		saved := a.Opt.IP
		if ip := a.St.Get("camera_ip"); a.Opt.IP == "" && ip != "" && !a.UI.Dry() {
			a.Opt.IP = ip // the chooser already picked it: no second question
		}
		err := a.cmdFindCamera()
		a.Opt.IP = saved
		return err
	case "image":
		return a.installImage()
	case "verify":
		a.UI.Say("  Now power-cycle the camera BY HAND: unplug the USB-C power, wait 5 seconds, plug it back. The assistant never does it.")
		if a.UI.Dry() {
			return a.cmdVerify()
		}
		if ok, _ := a.UI.AskYN("  Is the camera power-cycled and back on the network? Verify it now?"); !ok {
			a.UI.Say(fmt.Sprintf("  When it is back: %s verify (or choose 'Set up a new camera' again).", Prog))
			return nil
		}
		return a.cmdVerify()
	}
	return a.runStep(step)
}

// installImage is the image step. Until the native builder lands (T11.12 to T11.16) the image is built
// by the bash wizard on Linux, or imported with --image FILE (size exactly the APP partition; the red
// box of the flash step shows its md5).
func (a *App) installImage() error {
	if err := a.stepNeed("image"); err != nil {
		return err
	}
	a.stepBanner("image")
	if a.UI.Dry() {
		a.UI.Say("  WOULD import --image FILE (exactly 5242880 bytes, md5 shown) as the image of this camera; on Linux the bash wizard builds it from your backup.")
		_ = a.St.Set("image_path", "<image>")
		_ = a.St.Set("image_sha", "dry-run")
		_ = a.St.MarkDone("image")
		return nil
	}
	f := a.Opt.Image
	if f == "" {
		if runtime.GOOS == "linux" {
			return a.UI.Fail("the image is built from your backup by the bash wizard until the native builder lands.",
				fmt.Sprintf("from a source checkout run: ./akam step image; then choose 'Set up a new camera' again (or pass --image FILE)."))
		}
		return a.UI.Fail("this system cannot build the image yet: it needs an image built on Linux or WSL from your own backup.",
			fmt.Sprintf("build it there (./akam step image), copy the 5242880 byte file here and run: %s --image FILE image", Prog))
	}
	md5, err := a.imageGate(f)
	if err != nil {
		return err
	}
	if err := a.imageSquashCheck(f); err != nil {
		return err
	}
	abs := f
	_ = a.St.Set("image_path", abs)
	_ = a.St.Set("image_sha", fileSHA(f))
	_ = a.St.Set("image_md5", md5)
	_ = a.St.Set("image_kit_sha", a.kitCurrentSHA())
	_ = a.St.Set("image_backup_sha", a.backupCurrentSHA())
	_ = a.St.MarkDone("image")
	a.UI.Warn("this image was built elsewhere: it must come from YOUR backup of THIS camera. The red box of the flash step shows its md5; compare it with the one printed where you built it.")
	a.UI.Good("  Image recorded: " + strings.TrimSpace(abs))
	return nil
}

// imageSquashCheck is the trust check of an image that was not built by this run (--image FILE, the
// image of an earlier run): it must open as a squashfs 4.0 with the T11.12 reader (magic, version,
// bytes_used inside the APP partition, every table in range), be xz-compressed like the images of the
// builder, and decode completely (Verify). Anything else is refused before any card or camera is touched.
func (a *App) imageSquashCheck(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return a.UI.Fail("cannot read the image: "+err.Error(), "")
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return a.UI.Fail("cannot read the image: "+err.Error(), "")
	}
	fs, err := squashfs.Open(f, st.Size())
	if err != nil {
		return a.UI.Fail("this file is not a valid squashfs image: "+ui.Sanitize(err.Error())+".", "do not flash it; build the image again from your own backup.")
	}
	info := fs.Info()
	if info.CompressionName() != "xz" {
		return a.UI.Fail("the image is compressed with "+info.CompressionName()+", the camera image must be xz.", "do not flash it; build the image again from your own backup.")
	}
	if info.BytesUsed > appPartitionBytes {
		return a.UI.Fail(fmt.Sprintf("the image uses %d bytes, more than the APP partition (%d).", info.BytesUsed, appPartitionBytes), "do not flash it.")
	}
	if err := fs.Verify(); err != nil {
		return a.UI.Fail("the image does not decode cleanly: "+ui.Sanitize(err.Error())+".", "do not flash it; build the image again from your own backup.")
	}
	a.UI.Say(fmt.Sprintf("  Image structure checked: squashfs 4.0, xz, %d bytes used of %d, every block decodes.", info.BytesUsed, appPartitionBytes))
	return nil
}
