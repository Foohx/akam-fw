//go:build unix

package platform

import (
	"path/filepath"
	"syscall"
)

// DocumentsKnownFolder: only Windows has one; elsewhere backuploc derives it from the home folder.
func DocumentsKnownFolder() string { return "" }

// FreeSpace is the number of bytes a normal user can still write under dir (or its nearest existing
// parent); -1 when unknown.
func FreeSpace(dir string) int64 {
	d := dir
	for {
		var st syscall.Statfs_t
		if syscall.Statfs(d, &st) == nil {
			return int64(st.Bavail) * int64(st.Bsize)
		}
		parent := filepath.Dir(d)
		if parent == d {
			return -1
		}
		d = parent
	}
}
