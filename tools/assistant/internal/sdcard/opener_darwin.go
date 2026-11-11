//go:build darwin

package sdcard

import (
	"errors"
	"os"
	"syscall"
)

// realRaw is an *os.File on /dev/rdiskN.
type realRaw struct{ f *os.File }

func openRawDarwin(path string) (macRawFile, error) {
	st, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	// the raw node is a character device; anything else (a link, a file) is refused
	if st.Mode()&os.ModeDevice == 0 || st.Mode()&os.ModeCharDevice == 0 {
		return nil, errors.New("refusing: " + path + " is not a raw disk node")
	}
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	return realRaw{f}, nil
}

func (r realRaw) ReadAt(p []byte, off int64) (int, error)  { return r.f.ReadAt(p, off) }
func (r realRaw) WriteAt(p []byte, off int64) (int, error) { return r.f.WriteAt(p, off) }
func (r realRaw) Close() error                             { return r.f.Close() }

// Sync flushes the device. Go uses F_FULLFSYNC on macOS; a raw node may answer that it is not supported, in
// which case a plain fsync is the best the node offers.
func (r realRaw) Sync() error {
	err := r.f.Sync()
	if errors.Is(err, syscall.ENOTTY) || errors.Is(err, syscall.EINVAL) || errors.Is(err, syscall.ENOTSUP) {
		return syscall.Fsync(int(r.f.Fd()))
	}
	return err
}

func (r realRaw) DevNum() (string, error) {
	var st syscall.Stat_t
	if err := syscall.Fstat(int(r.f.Fd()), &st); err != nil {
		return "", err
	}
	if st.Mode&syscall.S_IFMT != syscall.S_IFCHR {
		return "", errors.New("not a character device")
	}
	return fmtDevNum(uint32(st.Rdev)), nil
}
