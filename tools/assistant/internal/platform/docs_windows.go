//go:build windows

package platform

import (
	"path/filepath"

	"golang.org/x/sys/windows"
)

// DocumentsKnownFolder is the Documents known folder (it follows a redirection to OneDrive).
func DocumentsKnownFolder() string {
	p, err := windows.KnownFolderPath(windows.FOLDERID_Documents, windows.KF_FLAG_DEFAULT)
	if err != nil {
		return ""
	}
	return p
}

// FreeSpace is the number of bytes the current user can still write under dir (or its nearest
// existing parent); -1 when unknown.
func FreeSpace(dir string) int64 {
	d := dir
	for {
		if p, err := windows.UTF16PtrFromString(d); err == nil {
			var avail, total, free uint64
			if windows.GetDiskFreeSpaceEx(p, &avail, &total, &free) == nil {
				return int64(avail)
			}
		}
		parent := filepath.Dir(d)
		if parent == d {
			return -1
		}
		d = parent
	}
}
