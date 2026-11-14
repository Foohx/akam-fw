package cli

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
)

// A kit is installed in two halves so that a failure never needs a rollback:
//
//	getKitStage  download to downloads/<ver>/.staging, verify, unpack to kit.new. Nothing recorded changes.
//	commitKit    only when everything else succeeded (downgrade question, image check or build, the write
//	             itself for an update): a marker (kit.swap) is written, kit -> kit.old, kit.new -> kit, the staged
//	             downloads replace the recorded ones, the kit keys of the state are written in ONE atomic
//	             write, kit.old and the marker are removed.
//
// An interruption anywhere before commitKit leaves the previous kit, its downloaded files and its state as
// they were; a crash inside commitKit is repaired at the next start (recoverKit) from the marker alone. A
// rehearsal (--dry-run) never reaches commitKit: it installs nothing.

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
	model      string // the camera model the kit is built for (MANIFEST model=)
	unsigned   bool
	undo       func() // puts back what building the image changed in the state (a refused or failed choice)
}

func (a *App) kitRealDir() string  { return filepath.Join(a.Cams.Base, "kit") }
func (a *App) kitNewDir() string   { return filepath.Join(a.Cams.Base, "kit.new") }
func (a *App) kitOldDir() string   { return filepath.Join(a.Cams.Base, "kit.old") }
func (a *App) kitSwapFile() string { return filepath.Join(a.Cams.Base, "kit.swap") }

var errRehearsal = errors.New("a rehearsal never installs a kit")

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

// kitFacts is what the kit of the camera is: the staged one while it waits to be installed (so the record of a
// write describes the kit that went with it), else the recorded one.
func (a *App) kitFacts() (version, path string, unsigned, kitMode bool) {
	if s := a.staged; s != nil {
		return s.version, s.path, s.unsigned, true
	}
	return a.St.Get("kit_version"), a.St.Get("kit_path"), a.St.Get("kit_unsigned_ok") == "1", a.St.Get("mode") == "kit"
}

// discardKit throws a staged kit away (never touches the installed one) and undoes what an image build did.
func (a *App) discardKit(s *stagedKit) {
	if s == nil {
		return
	}
	if s.undo != nil {
		s.undo()
		s.undo = nil
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

// kitPreflight runs, before anything is asked or written, what could make the final swap fail: a leftover of an
// interrupted swap that must be repaired, a kit.old that cannot be cleared, a state folder that cannot be written.
func (a *App) kitPreflight() error {
	if a.UI.Dry() {
		return errRehearsal
	}
	a.recoverKit(false)
	kd, old := a.kitRealDir(), a.kitOldDir()
	if _, err := os.Stat(old); err == nil {
		if _, kerr := os.Stat(kd); kerr != nil {
			return fmt.Errorf("a previous kit is waiting in %s and cannot be restored: move it to %s by hand", old, kd)
		}
		if err := os.RemoveAll(old); err != nil {
			return fmt.Errorf("cannot clear %s: %v", old, err)
		}
	}
	probe := filepath.Join(a.Cams.Base, ".kit.probe")
	if err := os.WriteFile(probe, nil, 0o600); err != nil {
		return fmt.Errorf("cannot write in %s: %v", a.Cams.Base, err)
	}
	_ = os.Remove(probe)
	return nil
}

// commitKit installs a staged kit. If the previous kit cannot be set aside nothing is touched; if a later
// step fails the previous kit folder is put back.
func (a *App) commitKit(s *stagedKit) error {
	if a.UI.Dry() {
		a.discardKit(s)
		return errRehearsal
	}
	fail := func(msg string) error {
		a.discardKit(s)
		return errors.New(msg)
	}
	if err := a.kitPreflight(); err != nil {
		return fail(err.Error() + ".")
	}
	kd, old := a.kitRealDir(), a.kitOldDir()
	_, statErr := os.Stat(kd)
	haveOld := statErr == nil
	marker := fmt.Sprintf("camera=%s\nnew_sha=%s\nnew_version=%s\nold_sha=%s\nold_version=%s\n",
		a.Cams.Name, s.sha, stateLine(s.version), a.St.Get("kit_sha"), stateLine(a.St.Get("kit_version")))
	if err := fsutil.AtomicWrite(a.kitSwapFile(), []byte(marker), 0o600); err != nil {
		return fail("cannot write the swap marker (" + err.Error() + ")")
	}
	dropMarker := func() {
		if err := os.Remove(a.kitSwapFile()); err != nil && !os.IsNotExist(err) {
			a.UI.Warn("could not remove " + a.kitSwapFile() + ": " + err.Error() + " (it is repaired at the next start).")
		}
	}
	if haveOld {
		if err := os.Rename(kd, old); err != nil {
			dropMarker()
			return fail("cannot set the previous kit aside (" + err.Error() + ")")
		}
	}
	undo := func() bool {
		if !haveOld {
			return true
		}
		if err := os.Rename(old, kd); err != nil {
			a.UI.Warn("could not put the previous kit back from " + old + ": " + err.Error() + ". Move it to " + kd + " by hand; it is not deleted.")
			return false
		}
		return true
	}
	if err := os.Rename(s.newDir, kd); err != nil {
		if undo() {
			dropMarker()
		}
		return fail("cannot install the new kit (" + err.Error() + ")")
	}
	back := func() { // the new kit goes back to kit.new, the old one to kit
		if err := os.Rename(kd, s.newDir); err == nil && undo() {
			dropMarker()
		}
	}
	var restoreDL, finishDL func()
	if s.staging != "" {
		var err error
		if restoreDL, finishDL, err = a.moveDownloads(s); err != nil {
			back()
			return fail("cannot keep the downloaded kit (" + err.Error() + ")")
		}
	}
	if err := a.St.SetMany([][2]string{{"mode", "kit"}, {"kit_path", s.path}, {"kit_sha", s.sha}, {"kit_format", s.format},
		{"kit_unsigned_ok", map[bool]string{true: "1", false: "0"}[s.unsigned]}, {"kit_version", s.version}, {"kit_model", s.model},
		{"step_get_kit_done", time.Now().UTC().Format("2006-01-02T15:04:05Z")}}); err != nil {
		if restoreDL != nil {
			restoreDL()
		}
		back()
		return fail("cannot record the kit (" + err.Error() + ")")
	}
	if finishDL != nil {
		finishDL()
	}
	if haveOld {
		if err := os.RemoveAll(old); err != nil {
			a.UI.Warn("the previous kit could not be removed (" + err.Error() + "): it stays in " + old + " and is cleared at the next start.")
		}
	}
	dropMarker()
	if a.staged == s {
		a.staged = nil
	}
	s.undo = nil
	return nil
}

// moveDownloads puts the staged downloads in place of the recorded ones, all or nothing: the recorded files are
// set aside first and come back if anything fails. A signature that the new release does not have removes the
// old one. It returns the function that undoes it.
func (a *App) moveDownloads(s *stagedKit) (restore, finish func(), err error) {
	if a.UI.Dry() {
		return nil, nil, errRehearsal
	}
	if err := os.MkdirAll(s.finalDL, 0o700); err != nil {
		return nil, nil, err
	}
	names := []string{"SHA256SUMS", "SHA256SUMS.sig", filepath.Base(s.path)}
	var aside, placed []string
	restore = func() {
		for _, n := range placed {
			_ = os.Remove(filepath.Join(s.finalDL, n))
		}
		for _, n := range aside {
			_ = os.Rename(filepath.Join(s.finalDL, n+".bak"), filepath.Join(s.finalDL, n))
		}
	}
	for _, n := range names {
		if _, err := os.Stat(filepath.Join(s.finalDL, n)); err == nil {
			if err := os.Rename(filepath.Join(s.finalDL, n), filepath.Join(s.finalDL, n+".bak")); err != nil {
				restore()
				return nil, nil, err
			}
			aside = append(aside, n)
		}
	}
	for _, n := range names {
		src := filepath.Join(s.staging, n)
		if _, err := os.Stat(src); err != nil {
			if n == "SHA256SUMS.sig" {
				continue // this release has no signature file: the old one is not kept
			}
			restore()
			return nil, nil, err
		}
		if err := os.Rename(src, filepath.Join(s.finalDL, n)); err != nil {
			restore()
			return nil, nil, err
		}
		placed = append(placed, n)
	}
	// the recorded files stay set aside (.bak) until the whole install succeeded: finish drops them
	return restore, func() {
		for _, n := range aside {
			_ = os.Remove(filepath.Join(s.finalDL, n+".bak"))
		}
		_ = os.RemoveAll(s.staging)
	}, nil
}

// recoverKit repairs an interrupted kit swap; it runs at start (startup), under the process lock, and before a
// swap. The shared kit folder is judged from the swap marker (kit.swap) alone:
//   - marker, kit.old present and kit absent: the swap stopped between the renames; the old kit comes back;
//   - marker, kit.old and kit present: the new kit is in place; it stays when the camera that started the swap
//     recorded it (its state has the new sha), otherwise the old one is put back;
//   - marker without kit.old: nothing was swapped; the marker goes;
//   - no marker: a kit.old (or the older kit.keep) is restored when the kit is absent, deleted when it is not.
//
// A kit.old is never deleted while the kit folder is absent or a put-back failed. At start a kit.new that no
// marker explains and every downloads/*/.staging is an unfinished unpack and goes.
func (a *App) recoverKit(startup bool) {
	if a.UI.Dry() {
		return
	}
	kd, nw, old, mk := a.kitRealDir(), a.kitNewDir(), a.kitOldDir(), a.kitSwapFile()
	exists := func(p string) bool { _, err := os.Stat(p); return err == nil }
	say := func(s string) {
		a.Log.Line("RECOVER %s", s)
		a.UI.Say("  " + s)
	}
	if bad := kd + ".bad"; exists(bad) { // an older version of this repair left it
		if exists(kd) {
			_ = os.RemoveAll(bad)
		} else if os.Rename(bad, kd) == nil {
			say("the kit was restored from " + bad + ".")
		}
	}
	if b, err := os.ReadFile(mk); err == nil {
		m := map[string]string{}
		for _, l := range strings.Split(string(b), "\n") {
			if k, v, ok := strings.Cut(l, "="); ok {
				m[k] = v
			}
		}
		switch {
		case exists(old) && !exists(kd):
			if err := os.Rename(old, kd); err != nil {
				a.Log.Line("WARN cannot restore %s: %v", old, err)
				return // keep the marker and the old kit
			}
			_ = os.RemoveAll(nw)
			say("an interrupted kit swap was undone: the previous kit is back.")
		case exists(old):
			if a.stateKeyOf(m["camera"], "kit_sha") == m["new_sha"] && m["new_sha"] != "" {
				if err := os.RemoveAll(old); err != nil {
					a.Log.Line("WARN cannot remove %s: %v", old, err)
					return
				}
				say("an interrupted kit swap was completed: the new kit is kept.")
			} else {
				if err := os.Rename(kd, nw); err != nil {
					a.Log.Line("WARN cannot set %s aside: %v", kd, err)
					return
				}
				if err := os.Rename(old, kd); err != nil {
					_ = os.Rename(nw, kd) // back to what it was; nothing is deleted
					a.Log.Line("WARN cannot restore %s: %v", old, err)
					return
				}
				_ = os.RemoveAll(nw)
				say("an interrupted kit swap was undone: the previous kit is back.")
			}
		default:
			if startup {
				_ = os.RemoveAll(nw)
			}
		}
		if err := os.Remove(mk); err != nil && !os.IsNotExist(err) {
			a.Log.Line("WARN cannot remove %s: %v", mk, err)
		}
	} else if startup && exists(nw) {
		_ = os.RemoveAll(nw) // an unfinished unpack
	}
	for _, o := range []string{old, kd + ".keep"} {
		if !exists(o) {
			continue
		}
		if !exists(kd) {
			if err := os.Rename(o, kd); err != nil {
				a.Log.Line("WARN cannot restore %s: %v", o, err)
			} else {
				say("the kit was restored from " + filepath.Base(o) + ".")
			}
			continue // never deleted while the kit is absent
		}
		if !exists(mk) {
			if err := os.RemoveAll(o); err != nil {
				a.Log.Line("WARN cannot remove %s: %v", o, err)
			}
		}
	}
	if startup {
		if ents, err := os.ReadDir(filepath.Join(a.Cams.Base, "downloads")); err == nil {
			for _, e := range ents {
				_ = os.RemoveAll(filepath.Join(a.Cams.Base, "downloads", e.Name(), ".staging"))
			}
		}
	}
}

// stateKeyOf reads a key of another camera's state file ("" when unknown).
func (a *App) stateKeyOf(camera, key string) string {
	if camera == "" {
		return ""
	}
	b, err := os.ReadFile(filepath.Join(a.Cams.DirOf(camera), "state"))
	if err != nil {
		return ""
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, key+"="); ok {
			return v
		}
	}
	return ""
}
