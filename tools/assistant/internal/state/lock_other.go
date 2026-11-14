//go:build !unix && !windows

package state

// Lock is a no-op where flock is not available (plan 9 and other systems without flock or LockFileEx).
type Lock struct{}

// Acquire always succeeds.
func Acquire(dir string) (*Lock, bool, error) { return &Lock{}, true, nil }

// Release does nothing.
func (l *Lock) Release() {}
