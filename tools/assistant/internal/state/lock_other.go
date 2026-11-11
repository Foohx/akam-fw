//go:build !unix

package state

// Lock is a no-op where flock is not available (Windows support comes in a later phase).
type Lock struct{}

// Acquire always succeeds.
func Acquire(dir string) (*Lock, bool, error) { return &Lock{}, true, nil }

// Release does nothing.
func (l *Lock) Release() {}
