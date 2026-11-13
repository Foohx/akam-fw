package cli

import (
	"os"
	"path/filepath"
	"strings"
)

// A kit is installed in two halves so that a failure never needs a rollback:
//
//	getKitStage  download to downloads/<ver>/.staging, verify, unpack to kit.new. Nothing recorded changes.
//	commitKit    only when everything else succeeded (downgrade question, image check or build, the write
//	             itself for an update): kit -> kit.old, kit.new -> kit, the staged downloads replace the
//	             recorded ones, the state is set, kit.old is removed.
//
// An interruption anywhere before commitKit leaves the previous kit, its downloaded files and its state as
// they were; a crash inside commitKit is repaired at the next start (recoverKit).

// stagedKit is a kit that was verified and unpacked but is not installed yet.
type stagedKit struct {
	newDir     string // kit.new: the verified tree
	path       string // what kit_path will say (the final tarball, or the file or folder the user gave)
	stagedPath string // where the tarball is now, for a download ("" otherwise)
	staging    string // downloads/<ver>/.staging ("" for a local kit)
	finalDL    string // downloads/<ver>
	sha        string
	format     string
	version    string
	unsigned   bool
}

func (a *App) kitRealDir() string { return filepath.Join(a.Cams.Base, "kit") }
func (a *App) kitNewDir() string  { return filepath.Join(a.Cams.Base, "kit.new") }
func (a *App) kitOldDir() string  { return filepath.Join(a.Cams.Base, "kit.old") }

// kitInUse is the kit file or folder and its trust as the code that reads the kit must see them: the staged
// kit while one is waiting to be installed, else the recorded one.
func (a *App) kitInUse() (path string, unsigned bool) {
	if s := a.staged; s != nil {
		if s.stagedPath != "" {
			return s.stagedPath, s.unsigned
		}
		return s.path, s.unsigned
	}
	return a.St.Get("kit_path"), a.St.Get("kit_unsigned_ok") == "1"
}

// discardKit throws a staged kit away (never touches the installed one).
func (a *App) discardKit(s *stagedKit) {
	if s == nil {
		return
	}
	if err := os.RemoveAll(s.newDir); err != nil {
		a.Log.Line("WARN cannot remove %s: %v", s.newDir, err)
	}
	if s.staging != "" {
		if err := os.RemoveAll(s.staging); err != nil {
			a.Log.Line("WARN cannot remove %s: %v", s.staging, err)
		}
	}
	if a.staged == s {
		a.staged = nil
	}
}

// discardStaged is the deferred cleanup of a flow: whatever is still staged goes.
func (a *App) discardStaged() { a.discardKit(a.staged) }

// commitStaged installs the kit that is waiting (no-op when none is).
func (a *App) commitStaged() error {
	if a.staged == nil {
		return nil
	}
	return a.commitKit(a.staged)
}

// commitKit installs a staged kit. If the previous kit cannot be set aside nothing is touched; if a later
// step fails the previous kit folder is put back.
func (a *App) commitKit(s *stagedKit) error {
	kd, old := a.kitRealDir(), a.kitOldDir()
	a.recoverKit(false) // a kit.old from an interrupted run must be dealt with before it is overwritten
	if _, err := os.Stat(old); err == nil {
		if _, kerr := os.Stat(kd); kerr != nil {
			a.discardKit(s)
			return a.UI.Fail("a previous kit is waiting in "+old+" and cannot be restored.", "nothing was changed; move it to "+kd+" by hand and try again.")
		}
		if err := os.RemoveAll(old); err != nil {
			a.discardKit(s)
			return a.UI.Fail("cannot clear "+old+": "+err.Error()+".", "nothing was changed.")
		}
	}
	_, statErr := os.Stat(kd)
	haveOld := statErr == nil
	if haveOld {
		if err := os.Rename(kd, old); err != nil {
			a.discardKit(s)
			return a.UI.Fail("cannot set the previous kit aside ("+err.Error()+").", "nothing was changed: the previous kit stays in use.")
		}
	}
	undo := func() {
		if haveOld {
			if err := os.Rename(old, kd); err != nil {
				a.UI.Warn("could not put the previous kit back from " + old + ": " + err.Error() + ". Move it to " + kd + " by hand.")
			}
		}
	}
	if err := os.Rename(s.newDir, kd); err != nil {
		undo()
		a.discardKit(s)
		return a.UI.Fail("cannot install the new kit ("+err.Error()+").", "nothing was changed: the previous kit stays in use.")
	}
	if s.staging != "" {
		if err := a.moveDownloads(s); err != nil {
			_ = os.Rename(kd, s.newDir)
			undo()
			a.discardKit(s)
			return a.UI.Fail("cannot keep the downloaded kit ("+err.Error()+").", "nothing was changed.")
		}
	}
	_ = a.St.Set("mode", "kit")
	_ = a.St.Set("kit_path", s.path)
	_ = a.St.Set("kit_sha", s.sha)
	_ = a.St.Set("kit_format", s.format)
	u := "0"
	if s.unsigned {
		u = "1"
	}
	_ = a.St.Set("kit_unsigned_ok", u)
	_ = a.St.Set("kit_version", s.version)
	_ = a.St.MarkDone("get-kit")
	if haveOld {
		if err := os.RemoveAll(old); err != nil {
			a.UI.Warn("the previous kit could not be removed (" + err.Error() + "): it stays in " + old + " and is cleared at the next start.")
		}
	}
	if a.staged == s {
		a.staged = nil
	}
	return nil
}

// moveDownloads puts the staged downloads in place of the recorded ones (a signature that the new release
// does not have removes the old one). The tarball goes last.
func (a *App) moveDownloads(s *stagedKit) error {
	if err := os.MkdirAll(s.finalDL, 0o700); err != nil {
		return err
	}
	for _, n := range []string{"SHA256SUMS", "SHA256SUMS.sig", filepath.Base(s.path)} {
		src, dst := filepath.Join(s.staging, n), filepath.Join(s.finalDL, n)
		if _, err := os.Stat(src); err != nil {
			if n == "SHA256SUMS.sig" {
				if rerr := os.Remove(dst); rerr != nil && !os.IsNotExist(rerr) {
					return rerr
				}
				continue
			}
			return err
		}
		if err := os.Rename(src, dst); err != nil {
			return err
		}
	}
	_ = os.RemoveAll(s.staging)
	return nil
}

// recoverKit repairs an interrupted kit swap; it runs at start (startup), under the process lock, and before a
// swap (then a kit.new is the kit being installed and is left alone).
//   - at start a leftover kit.new is an unfinished unpack: deleted;
//   - kit absent and kit.old (or the older kit.keep) present: the swap stopped in the middle, the old kit comes back;
//   - both present: the swap finished but kit.old was not removed. The kit the state describes stays: when the
//     folder's MANIFEST version is not the recorded kit_version the old one is put back, else kit.old is deleted.
//
// A kit.old is never deleted while the kit folder is absent.
func (a *App) recoverKit(startup bool) {
	kd := a.kitRealDir()
	if _, err := os.Stat(a.kitNewDir()); err == nil && startup {
		if err := os.RemoveAll(a.kitNewDir()); err != nil {
			a.Log.Line("WARN cannot remove %s: %v", a.kitNewDir(), err)
		}
	}
	for _, o := range []string{a.kitOldDir(), kd + ".keep"} {
		if _, err := os.Stat(o); err != nil {
			continue
		}
		if _, err := os.Stat(kd); err != nil {
			if err := os.Rename(o, kd); err != nil {
				a.Log.Line("WARN cannot restore %s: %v", o, err)
			} else {
				a.Log.Line("RECOVER kit restored from %s", o)
			}
			continue
		}
		if want := a.St.Get("kit_version"); a.St.Get("mode") == "kit" && want != "" && manifestVersion(kd) != want {
			tmp := kd + ".bad"
			_ = os.RemoveAll(tmp)
			if os.Rename(kd, tmp) == nil {
				if os.Rename(o, kd) == nil {
					_ = os.RemoveAll(tmp)
					a.Log.Line("RECOVER kit put back from %s (the folder did not match the recorded version)", o)
					continue
				}
				_ = os.Rename(tmp, kd)
			}
		}
		if err := os.RemoveAll(o); err != nil {
			a.Log.Line("WARN cannot remove %s: %v", o, err)
		}
	}
}

// manifestVersion is the akam_version of the MANIFEST.txt of a kit folder ("" when unreadable).
func manifestVersion(dir string) string {
	b, err := os.ReadFile(filepath.Join(dir, "MANIFEST.txt"))
	if err != nil {
		return ""
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "akam_version="); ok {
			return v
		}
	}
	return ""
}
