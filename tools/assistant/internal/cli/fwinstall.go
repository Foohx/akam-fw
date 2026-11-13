package cli

import "github.com/Foohx/akam-fw/tools/assistant/internal/ui"

// installFirmware applies the firmware of the first install, chosen on the command line (--version, --kit,
// --image) or in the "Which firmware?" list (asked when the kit or the image step is still to do). The kit of
// a release or a kit file is downloaded or opened, verified and installed right here, before any step runs:
// it replaces the recorded kit only when it verified (so a failed download leaves the recorded kit and its
// done mark alone), and an image file replaces the recorded image. The options are put back by the returned
// function, so a later action of the same session starts clean. proceed is false when the user went back or
// the kit could not be used.
func (a *App) installFirmware() (restore func(), proceed bool) {
	sv, sk, si, sb := a.Opt.Version, a.Opt.Kit, a.Opt.Image, a.Opt.Builder
	restore = func() { a.Opt.Version, a.Opt.Kit, a.Opt.Image, a.Opt.Builder = sv, sk, si, sb }
	if a.cliFW.Kind == fwStock {
		_ = a.UI.Fail("--stock puts the original firmware back on an Akam camera: it belongs to the update.", "choose 'Update this camera', or run: "+Prog+" --stock update")
		return restore, false
	}
	if a.UI.Dry() {
		return restore, true // a rehearsal asks nothing and downloads nothing
	}
	src := a.cliFW
	fromList := false
	if src.Kind == "" {
		if a.St.Get("mode") == "source" || (a.installState("get-kit") == "done" && a.installState("image") == "done") {
			return restore, true // nothing to choose: a source checkout has its own kit, both steps are done
		}
		var ok bool
		if src, ok = a.pickFirmware(fwInstall, "", ""); !ok {
			a.UI.Say("  No firmware chosen. Nothing was changed.")
			return restore, false
		}
		fromList = true
	}
	switch src.Kind {
	case fwRelease:
		return restore, a.installKit(src.Version, "")
	case fwKit:
		return restore, a.installKit("", src.Path)
	case fwImage:
		a.Opt.Image = src.Path
		a.Opt.Builder = "" // the chosen image wins over --builder native
		if a.St.Get("image_path") != src.Path {
			_ = a.St.ClearDone("image")
		}
		if a.Opt.Kit != "" || a.Opt.Version != "" {
			return restore, a.installKit(a.Opt.Version, a.Opt.Kit) // --image with --version or --kit
		}
		if fromList && a.installState("get-kit") != "done" {
			a.UI.Say("  The image is written with the flash tools of a kit.")
			ks, ok := a.pickFirmware(fwKitOnly, "", "")
			if !ok {
				a.UI.Say("  No kit chosen. Nothing was changed.")
				return restore, false
			}
			return restore, a.installKit(ks.Version, ks.Path)
		}
	}
	return restore, true
}

// installKit stages the kit of a version or a file now (after the doctor step it needs), asks the imported-image
// question when the image on record was imported, and installs it. The kit is verified again whatever is
// recorded; the recorded kit stays until the new one verified and the question was answered.
func (a *App) installKit(version, path string) bool {
	if a.stepStatus("doctor") != "done" {
		if err := a.cmdDoctor(); err != nil {
			a.reportFlowError(err)
			return false
		}
	}
	s, err := a.stageKit(version, path)
	if err != nil {
		a.reportFlowError(err)
		a.UI.Say("  Nothing was changed: the previous kit stays in use. Choose 'Set up a new camera' again.")
		return false
	}
	if s == nil {
		return true
	}
	a.staged = s
	defer a.discardStaged()
	if a.stepStatus("image") == "done" && a.importedImageOnRecord() {
		a.UI.Warn("the image on record was imported, not built from Akam " + ui.Sanitize(s.version) + ": it would be kept for this kit.")
		if ok, qerr := a.UI.AskYN("  Keep using that imported image?"); qerr != nil || !ok {
			_ = a.UI.Fail("the imported image on record is not used.", "choose an image file again, or build the image of this version from your backup. Nothing was changed.")
			return false
		}
	}
	if err := a.commitKit(s); err != nil {
		_ = a.UI.Fail(ui.Sanitize(err.Error())+".", "nothing was changed: the previous kit stays in use.")
		return false
	}
	return true
}
