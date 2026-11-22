package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// cmdBackupLocation is `backup-location [show | DIR | default]`: the folder that holds "Akam backups", without a
// menu. DIR gets the same checks as the menu (an existing folder, not inside a git tree, a cloud-synced folder needs a
// yes or --yes); "default" goes back to the suggestion (Documents, or the home folder when Documents is synced).
// The choice is the one of every backup of every camera on this computer's state (state key backup_root).
func (a *App) cmdBackupLocation(args []string) error {
	if len(args) > 1 {
		return usageErr("akam: backup-location takes one word: show, default, or the folder that will hold 'Akam backups'")
	}
	word := "show"
	if len(args) == 1 {
		word = args[0]
	}
	if word == "show" {
		a.showBackupLocation()
		return nil
	}
	if word != "default" && hasControl(word) {
		return a.UI.Fail("that is not a usable path (no control characters, no line breaks).", "")
	}
	if !a.setBackupRoot(word) {
		return a.UI.Fail("the backup folder was not changed.", "")
	}
	a.showBackupLocation()
	return nil
}

// showBackupLocation says where the next backup of this camera goes and why.
func (a *App) showBackupLocation() {
	s := a.backupSuggest(time.Now())
	chosen := a.St.Get("backup_root")
	switch {
	case testhook.Env("AKAM_BACKUP_ROOT") != "":
		a.UI.Say("  Backup folder: set by the AKAM_BACKUP_ROOT test hook (legacy layout).")
	case a.backupRootOverride() != "":
		a.UI.Say("  Backup folder: " + ui.Sanitize(a.backupRootOverride()) + "  (chosen by you; " + Prog + " backup-location default goes back to the suggestion)")
	case chosen != "":
		a.UI.Warn("the folder you chose (" + ui.Sanitize(chosen) + ") is not usable now (missing or not a folder): the suggestion is used.")
		a.UI.Say("  Backup folder: " + ui.Sanitize(filepath.Dir(filepath.Dir(s.Dir))) + "  (suggested)")
	default:
		a.UI.Say("  Backup folder: " + ui.Sanitize(filepath.Dir(filepath.Dir(s.Dir))) + "  (suggested: Documents, or your home folder when Documents is synced)")
	}
	if s.Cloud != "" {
		a.UI.Warn(fmt.Sprintf("your Documents folder is inside %s, which copies files to the cloud: the suggestion is in your home folder.", s.Cloud))
	}
	a.UI.Say("  Next backup of '" + a.Cams.Name + "': " + ui.Sanitize(s.Dir))
	if s.Free >= 0 {
		a.UI.Say("  Free space: " + gbText(platform.FreeSpace(filepath.Dir(s.Dir))))
	}
	if why := backuploc.CloudReason(s.Dir, osFacts()); why != "" {
		a.UI.Warn(fmt.Sprintf("this folder looks like part of %s: its content may be copied to the cloud, with your Wi-Fi password in it.", why))
	}
	if _, err := os.Stat(filepath.Dir(s.Dir)); err != nil {
		a.UI.Say("  (the folders are created, private to your user, by the next backup)")
	}
}
