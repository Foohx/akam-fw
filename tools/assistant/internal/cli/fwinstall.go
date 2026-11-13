package cli

// installFirmware asks "Which firmware?" at the start of the first install, when the kit or the image step
// is still to do and no firmware was named on the command line. The choice only sets what the steps use
// (the kit to download or open, the image to import); the returned function puts the options back, so a
// later action of the same session starts clean. proceed is false when the user went back.
func (a *App) installFirmware() (restore func(), proceed bool) {
	sv, sk, si := a.Opt.Version, a.Opt.Kit, a.Opt.Image
	restore = func() { a.Opt.Version, a.Opt.Kit, a.Opt.Image = sv, sk, si }
	if a.cliFW.Kind == fwStock {
		_ = a.UI.Fail("--stock puts the original firmware back on an Akam camera: it belongs to the update.", "choose 'Update this camera', or run: "+Prog+" --stock update")
		return restore, false
	}
	if a.cliFW.Kind != "" {
		a.fwPending(a.cliFW) // the steps already read --version, --kit and --image
		return restore, true
	}
	if a.UI.Dry() || a.St.Get("mode") == "source" || (a.installState("get-kit") == "done" && a.installState("image") == "done") {
		return restore, true // nothing to choose: a rehearsal asks nothing, a source checkout has its own kit, both steps are done
	}
	src, ok := a.pickFirmware(fwInstall, "", "")
	if !ok {
		a.UI.Say("  No firmware chosen. Nothing was changed.")
		return restore, false
	}
	a.fwPending(src)
	switch src.Kind {
	case fwRelease:
		a.Opt.Version, a.Opt.Kit = src.Version, ""
		if !(a.installState("get-kit") == "done" && a.St.Get("mode") == "kit" && a.St.Get("kit_version") == src.Version) {
			_ = a.St.ClearDone("get-kit")
		}
	case fwKit:
		a.Opt.Version, a.Opt.Kit = "", src.Path
		_ = a.St.ClearDone("get-kit")
	case fwImage:
		a.Opt.Image = src.Path
		if a.St.Get("image_path") != src.Path {
			_ = a.St.ClearDone("image")
		}
		if a.installState("get-kit") != "done" {
			a.UI.Say("  The image is written with the flash tools of a kit.")
			ks, ok := a.pickFirmware(fwKitOnly, "", "")
			if !ok {
				a.UI.Say("  No kit chosen. Nothing was changed.")
				return restore, false
			}
			if ks.Kind == fwRelease {
				a.Opt.Version, a.Opt.Kit = ks.Version, ""
			} else {
				a.Opt.Version, a.Opt.Kit = "", ks.Path
			}
		}
	}
	return restore, true
}
