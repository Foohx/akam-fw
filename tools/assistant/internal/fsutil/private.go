package fsutil

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
)

// PrivateMkdirNew creates dir, whose parent must exist, and fails when dir already exists (so a
// folder planted by someone else is never taken over). It is private before anything is put in it:
// 0700 at creation, then the protected ACL on Windows.
func PrivateMkdirNew(dir string) error {
	if err := os.Mkdir(dir, 0o700); err != nil {
		return err
	}
	return RestrictDir(dir)
}

// PrivateMkdirChain makes the folders in order (each the parent of the next; the parent of the
// first is created if missing) private to the user: a missing one is created 0700, an existing one
// is restricted.
func PrivateMkdirChain(dirs ...string) error {
	for i, d := range dirs {
		if i == 0 {
			if err := os.MkdirAll(filepath.Dir(d), 0o700); err != nil {
				return err
			}
		}
		if err := os.Mkdir(d, 0o700); err != nil && !errors.Is(err, fs.ErrExist) {
			return err
		}
		if err := RestrictDir(d); err != nil {
			return err
		}
	}
	return nil
}

// WritePrivateFile writes data to a file that is private from its first byte: an existing file is
// removed first (WriteFile would keep its old, maybe world-readable, mode), the new one is created
// 0600 with O_EXCL (a symlink or a file made in the meantime is refused), whatever the umask.
func WritePrivateFile(path string, data []byte) error {
	if st, err := os.Lstat(path); err == nil && !st.IsDir() {
		if err := os.Remove(path); err != nil {
			return err
		}
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	if runtime.GOOS == "windows" {
		_ = restrictFile(path) // the folder's ACL is inherited anyway; this is the belt
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(path)
		return err
	}
	if err := f.Sync(); err != nil { // on disk before anything is renamed over a good file
		f.Close()
		os.Remove(path)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(path)
		return err
	}
	return nil
}

// IsPrivateFile reports whether a file is closed to other accounts (Unix: the mode read back).
func IsPrivateFile(path string) bool {
	if runtime.GOOS == "windows" {
		return true
	}
	st, err := os.Stat(path)
	return err == nil && st.Mode().Perm()&0o077 == 0
}
