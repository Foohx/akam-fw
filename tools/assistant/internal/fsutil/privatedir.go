package fsutil

import (
	"fmt"
	"os"
	"runtime"
)

// RestrictDir makes an existing directory private to the current user (chmod 0700 on Unix, a
// protected ACL on Windows) and returns the failure, for a caller that must tell the user
// (an accepted --backup-dir may live on a medium that has no permissions).
func RestrictDir(dir string) error {
	if runtime.GOOS != "windows" {
		if err := os.Chmod(dir, 0o700); err != nil {
			return fmt.Errorf("could not restrict access to %s: %w", dir, err)
		}
	}
	if err := restrictDir(dir); err != nil {
		return fmt.Errorf("could not restrict access to %s: %w", dir, err)
	}
	return nil
}

// IsPrivateDir reports whether a directory is closed to other accounts. On Unix it reads the mode
// back (a FAT or exFAT medium reports 0777 whatever chmod was asked); on Windows a successful
// RestrictDir is the evidence, so the caller passes its result there.
func IsPrivateDir(dir string) bool {
	if runtime.GOOS == "windows" {
		return true
	}
	st, err := os.Stat(dir)
	return err == nil && st.Mode().Perm()&0o077 == 0
}
