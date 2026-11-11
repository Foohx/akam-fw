package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Backup location, backup and restore screens, settings (spec 11, 3.2, 3.7, 3.8).

// backupSuggestion is the folder the next backup would use and what to tell about it.
type backupSuggestion struct {
	Dir   string
	Cloud string // non-empty: the Documents folder is synced, the suggestion moved to the home folder
	Root  string
	Free  int64
}

// backupSuggest computes the folder of the next backup (the date folder gets its -N suffix here).
func (a *App) backupSuggest(now time.Time) backupSuggestion {
	base, cloud, fold, _ := a.backupBase(now)
	exists := func(p string) bool { _, err := os.Stat(p); return err == nil }
	if fold {
		exists = backuploc.FoldedSet(backupNames(filepath.Dir(base)))
	}
	dir := backuploc.NextFree(base, exists)
	return backupSuggestion{Dir: dir, Cloud: cloud, Root: filepath.Dir(filepath.Dir(filepath.Dir(dir))), Free: platform.FreeSpace(filepath.Dir(dir))}
}

func gbText(n int64) string {
	if n < 0 {
		return "unknown"
	}
	return fmt.Sprintf("%.1f GB", float64(n)/1e9)
}

// backupLocationScreen proposes where the backup goes and lets the user keep it or choose another
// root folder (the "Akam backups/<camera>/<date>" part is always added). It returns false when the
// user cancels. The choice is kept in the state key backup_root.
func (a *App) backupLocationScreen() bool {
	for {
		s := a.backupSuggest(time.Now())
		a.UI.Blank()
		a.UI.Bold("Where should the backup of the camera be kept?")
		a.UI.Say("  The backup is the only way back to the original firmware, and it contains")
		a.UI.Say("  your Wi-Fi password in clear text. Keep it private and off cloud folders.")
		if s.Cloud != "" {
			a.UI.Warn(fmt.Sprintf("your Documents folder is inside %s, which copies files to the cloud: the suggestion is in your home folder instead.", s.Cloud))
		}
		a.UI.Say("")
		a.UI.Say("  Suggested:  " + s.Dir)
		a.UI.Say("  Free space: " + gbText(s.Free) + "   Private: the folders are made private to your account before anything is written")
		if a.UI.Dry() {
			a.UI.Say("  WOULD ask: use this folder, choose another folder, or cancel.")
			return true
		}
		i, err := a.UI.Pick("Backup folder:", []string{"Use this folder", "Choose another folder", "Cancel"})
		if err != nil || i == 2 {
			return false
		}
		if i == 0 {
			_ = a.St.Set("backup_root_ok", "1")
			return true
		}
		a.chooseBackupRoot()
	}
}

// chooseBackupRoot asks for another existing folder to hold "Akam backups".
func (a *App) chooseBackupRoot() {
	v, err := a.UI.Prompt("  Folder that will hold 'Akam backups' (it must exist; empty keeps the suggestion, 'default' goes back to it):", "")
	if err != nil || v == "" {
		return
	}
	if v == "default" {
		_ = a.St.Unset("backup_root")
		a.UI.Say("  Back to the suggested folder.")
		return
	}
	abs, err := filepath.Abs(v)
	if err != nil {
		a.UI.Say("  That is not a usable path.")
		return
	}
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		a.UI.Say("  " + ui.Sanitize(abs) + " is not an existing folder.")
		return
	}
	if inGitTree(abs) {
		a.UI.Say("  That folder is inside a git repository: a backup there could be committed. Choose another one.")
		return
	}
	why := backuploc.CloudReason(abs, osFacts())
	if why == "" {
		if r, err := filepath.EvalSymlinks(abs); err == nil {
			why = backuploc.CloudReason(r, osFacts())
		}
	}
	if why != "" {
		a.UI.Warn(fmt.Sprintf("this folder looks like part of %s: its content may be copied to the cloud, with your Wi-Fi password in it.", why))
		if ok, _ := a.UI.AskYN("  Use it anyway?"); !ok {
			return
		}
	}
	if err := a.St.Set("backup_root", abs); err != nil {
		a.UI.Warn("cannot remember the folder: " + err.Error())
		return
	}
	a.UI.Good("  The backups will be kept under " + filepath.Join(abs, backuploc.Folder) + ".")
}

// backupRootOverride is the root the user chose (state key backup_root), "" for the suggestion.
func (a *App) backupRootOverride() string {
	if testhook.Env("AKAM_BACKUP_ROOT") != "" || a.St == nil {
		return ""
	}
	r := a.St.Get("backup_root")
	if r == "" {
		return ""
	}
	if st, err := os.Stat(r); err != nil || !st.IsDir() {
		return ""
	}
	return r
}

// backupGate re-verifies the recorded backup NOW (SHA256SUMS re-hashed, not trusted from an earlier
// run) and returns its folder; "" with the reason when there is none that verifies.
func (a *App) backupGate() (dir, why string) {
	for _, key := range []string{"ssh_backup_dir", "backup_dir"} {
		d := a.St.Get(key)
		if d == "" {
			continue
		}
		app := filepath.Join(d, "mtdblock7.bin")
		sums := filepath.Join(d, "SHA256SUMS")
		switch {
		case !exists(app) || !exists(sums):
			why = d + " has no mtdblock7.bin or SHA256SUMS"
		case !sha256sumCheck(d, sums):
			why = "SHA256SUMS does not match the files in " + d
		default:
			return d, ""
		}
	}
	if why == "" {
		why = "no backup of this camera is recorded"
	}
	return "", why
}

// flowBackupMenu is "Back up / restore".
func (a *App) flowBackupMenu() error {
	a.banner("Back up / restore - " + a.Cams.Name)
	items := []string{
		"Back up the camera now",
		"Restore the original firmware from a backup (red box)",
		"Check a backup folder (SHA256SUMS)",
		"Choose where backups are kept",
		"Back",
	}
	i, err := a.UI.Pick("What do you want to do?", items)
	if err != nil || i == 4 {
		return nil
	}
	switch i {
	case 0:
		return a.flowBackup()
	case 1:
		if !a.needCamera() {
			return nil
		}
		return a.cmdRestore()
	case 2:
		d, perr := a.UI.Prompt("  Backup folder to check ["+a.St.Get("backup_dir")+"]:", a.St.Get("backup_dir"))
		if perr != nil || d == "" {
			return nil
		}
		return a.checkBackupFolder(d)
	case 3:
		a.backupLocationScreen()
	}
	return nil
}

// checkBackupFolder verifies a folder read-only: the layout and SHA256SUMS.
func (a *App) checkBackupFolder(d string) error {
	abs, _ := filepath.Abs(d)
	if st, err := os.Stat(abs); err != nil || !st.IsDir() {
		return a.UI.Fail("not a folder: "+ui.Sanitize(abs), "")
	}
	if !a.backupCheck(abs) {
		return ui.ErrFailed
	}
	if sums := filepath.Join(abs, "SHA256SUMS"); !exists(sums) || !sha256sumCheck(abs, sums) {
		return a.UI.Fail("SHA256SUMS is missing or does not match the files in "+abs+".", "this backup is damaged: use your second copy.")
	}
	a.UI.Good("  " + abs + ": 8 partitions present, SHA256SUMS matches.")
	return nil
}

// flowBackup picks the route from what the camera runs and shows the location first.
func (a *App) flowBackup() error {
	if !a.needCamera() {
		return nil
	}
	kind := a.St.Get("camera_kind")
	if kind == "akam" {
		a.UI.Say("  Route: SSH (this camera runs Akam). It only reads the flash and needs no card.")
		a.UI.Say("  This saves what is on the camera now; it cannot recreate the original (stock) firmware.")
	} else {
		a.UI.Say("  Route: the maintenance telnet and the backup card (stock firmware). The card must be in the camera.")
	}
	if a.St.Get("backup_root_ok") == "" && !a.backupLocationScreen() {
		a.UI.Say("  Cancelled. Nothing was done.")
		return nil
	}
	return a.cmdBackup()
}

// flowSettings is the settings screen: backup folder, state folder, SD helper mode, licences.
func (a *App) flowSettings() error {
	for {
		a.UI.Blank()
		a.UI.Bold("Settings")
		s := a.backupSuggest(time.Now())
		a.UI.Say("  Backup folder:  " + filepath.Dir(filepath.Dir(s.Dir)) + "  (backups go to <this>/<camera>/<date>)")
		a.UI.Say("  State folder:   " + a.Cams.Base + "  (read-only here)")
		el := os.Getenv("AKAM_SD_ELEVATE")
		if el == "" {
			el = "(not set: the default helper of this system)"
		}
		a.UI.Say("  SD helper mode: " + el + "  (AKAM_SD_ELEVATE, shown, not edited)")
		i, err := a.UI.Pick("Settings:", []string{"Change the backup folder", "Licences (the software built into this program)", "Back"})
		if err != nil || i == 2 {
			return nil
		}
		if i == 0 {
			a.backupLocationScreen()
			continue
		}
		fmt.Fprint(a.out(), assets.Notices())
	}
}
