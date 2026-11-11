//go:build unix

package state

import (
	"os"
	"path/filepath"
	"syscall"
)

// Lock is an exclusive advisory lock on Base/.akam.lock: two runs of the binary never work on the
// same state at once. The bash wizard does not take it (documented: do not run both together).
type Lock struct{ f *os.File }

// Acquire takes the lock without waiting; ok is false when another run holds it.
func Acquire(dir string) (l *Lock, ok bool, err error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, false, err
	}
	f, err := os.OpenFile(filepath.Join(dir, ".akam.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, false, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		if err == syscall.EWOULDBLOCK {
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
	_ = syscall.Flock(int(l.f.Fd()), syscall.LOCK_UN)
	l.f.Close()
	l.f = nil
}
