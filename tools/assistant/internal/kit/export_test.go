package kit

import "io"

// Test seams of verify.go, visible to the external test package only.

// SetSymlink makes os.Symlink fail (or be replaced) for this verification.
func (o *Options) SetSymlink(f func(oldname, newname string) error) { o.symlink = f }

// SetIsLocal replaces filepath.IsLocal (a fake for Windows rules).
func (o *Options) SetIsLocal(f func(string) bool) { o.isLocal = f }

// SetModesMatter switches the mode comparison of directory mode and returns the previous value.
func SetModesMatter(v bool) bool { old := modesMatter; modesMatter = v; return old }

// NewLimitReader is the 1 GiB backstop with a small limit.
func NewLimitReader(r io.Reader, left int64) io.Reader { return &limitReader{r: r, left: left} }
