//go:build windows

package state

import (
	"os"
	"path/filepath"

	"golang.org/x/sys/windows"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
)

// Lock is an exclusive lock (LockFileEx) on Base/.akam.lock: two runs of the binary never work on the
// same state at once. The system drops the lock when the process dies.
type Lock struct{ f *os.File }

// Acquire takes the lock without waiting; ok is false when another run holds it.
func Acquire(dir string) (l *Lock, ok bool, err error) {
	if err := fsutil.PrivateMkdir(dir); err != nil {
		return nil, false, err
	}
	f, err := os.OpenFile(filepath.Join(dir, ".akam.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, false, err
	}
	ol := new(windows.Overlapped)
	err = windows.LockFileEx(windows.Handle(f.Fd()), windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, ol)
	if err != nil {
		f.Close()
		if err == windows.ERROR_LOCK_VIOLATION {
			return nil, false, nil
		}
		return nil, false, err
	}
	return &Lock{f: f}, true, nil
}

// Release drops the lock.
func (l *Lock) Release() {
	if l == nil || l.f == nil {
		return
	}
	ol := new(windows.Overlapped)
	_ = windows.UnlockFileEx(windows.Handle(l.f.Fd()), 0, 1, 0, ol)
	l.f.Close()
	l.f = nil
}
