package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
)

// backupDumpBytes is the size of the 8 partitions (what the folder will hold).
func backupDumpBytes() int64 {
	var n int64
	for _, s := range camMtdSizes {
		n += int64(s)
	}
	return n
}

// osFacts are the real facts of this computer for backuploc.
func osFacts() backuploc.Facts {
	home, _ := os.UserHomeDir()
	return backuploc.Facts{
		GOOS:         runtime.GOOS,
		Home:         home,
		Getenv:       os.Getenv,
		ReadFile:     os.ReadFile,
		DirExists:    func(p string) bool { st, err := os.Stat(p); return err == nil && st.IsDir() },
		WinDocuments: platform.DocumentsKnownFolder(),
		EvalSymlinks: filepath.EvalSymlinks,
	}
}

// backupNames lists the entries of dir (for the case-folded -N suffix check).
func backupNames(dir string) []string {
	es, _ := os.ReadDir(dir)
	var out []string
	for _, e := range es {
		out = append(out, filepath.Join(dir, e.Name()))
	}
	return out
}

// backupBase is the folder to create for camera, before the -N suffix, and the reason shown with it.
func (a *App) backupBase(now time.Time) (base, cloudNote string, foldCase bool, parents []string) {
	if root := testhook.Env("AKAM_BACKUP_ROOT"); root != "" {
		// test hook and legacy layout: <root>/akam-backup-<name>-<date>
		return filepath.Join(root, "akam-backup-"+a.Cams.Slug()+now.Format("20060102")), "", false, nil
	}
	f := osFacts()
	s := backuploc.Suggest(f, a.Cams.Name, now)
	return s.Dir, s.Cloud, runtime.GOOS == "windows" || runtime.GOOS == "darwin", s.Parents
}

// backupSecure is backup_secure (G19): the CONFIG partition holds the Wi-Fi password, the ONVIF
// password and the RTSP hash, so the folder is private to this user (0700, files 0600; a protected
// ACL on Windows). filesOnly leaves the directory alone (an existing folder that holds other
// things). It returns false when the directory could not be restricted.
func (a *App) backupSecure(d string, filesOnly bool) bool {
	if a.UI.Dry() {
		return true
	}
	ok := true
	if !filesOnly {
		if err := fsutil.RestrictDir(d); err != nil {
			a.UI.Warn(fmt.Sprintf("  could not restrict %s to your user: %v. Do it by hand.", d, err))
			ok = false
		} else if !fsutil.IsPrivateDir(d) {
			ok = false
		}
	}
	names := []string{"SHA256SUMS", "README.txt", "camera_info.txt"}
	for n := 0; n < 8; n++ {
		names = append(names, fmt.Sprintf("mtdblock%d.bin", n))
	}
	// only the backup's own files: the folder may be an existing one holding other things
	for _, n := range names {
		p := filepath.Join(d, n)
		if st, err := os.Lstat(p); err == nil && st.Mode().IsRegular() {
			fsutil.PrivateFile(p)
			if !fsutil.IsPrivateFile(p) {
				ok = false
			}
		}
	}
	if filesOnly && ok {
		ok = fsutil.IsPrivateDir(d) // the folder itself was left alone: say so when it is open
	}
	return ok
}

// backupSecretsWarning is the warning of backup_accept.
func (a *App) backupSecretsWarning(d string, private bool) {
	a.UI.Warn("THIS BACKUP CONTAINS SECRETS: your Wi-Fi password, the ONVIF password (clear text) and the RTSP hash are")
	if private {
		a.UI.Warn("inside the CONFIG partition. The folder is now private to your user; keep the copy private too,")
	} else {
		a.UI.Warn("inside the CONFIG partition. This folder is NOT private (its medium has no permissions); keep the copy private,")
	}
	a.UI.Warn("never upload it or put it in a shared or cloud-synced folder.")
	why := backuploc.CloudReason(d, osFacts())
	if r, err := filepath.EvalSymlinks(d); why == "" && err == nil {
		why = backuploc.CloudReason(r, osFacts())
	}
	if why != "" {
		a.UI.Warn(fmt.Sprintf("this folder looks like part of %s: its content may be copied to the cloud. Move it to a local folder.", why))
	}
}

// backupValidate reports the problems of a new backup folder (not on the SD card guards: those
// are in the SD commands).
func (a *App) backupValidate(d string) []string {
	st := backuploc.State{FreeBytes: platform.FreeSpace(d), NeedBytes: backupDumpBytes()}
	st.InRepo = inGitTree(d)
	if fi, err := os.Stat(d); err == nil {
		st.Exists = true
		if es, err := os.ReadDir(d); err == nil && fi.IsDir() {
			st.Empty = len(es) == 0
		}
	}
	var out []string
	for _, p := range backuploc.Validate(d, osFacts(), st) {
		out = append(out, p.Msg)
	}
	return out
}

// inGitTree reports whether d is inside a git work tree (a backup there could be committed).
func inGitTree(d string) bool {
	p, err := filepath.Abs(d)
	if err != nil {
		return false
	}
	for {
		if _, err := os.Lstat(filepath.Join(p, ".git")); err == nil {
			return true
		}
		up := filepath.Dir(p)
		if up == p {
			return false
		}
		p = up
	}
}
