//go:build darwin

package sdcard

import (
	"errors"
	"io/fs"
	"net"
	"syscall"

	"golang.org/x/sys/unix"
)

// xucredVersion is XUCRED_VERSION of <sys/ucred.h>.
const xucredVersion = 0

// peerUIDOS returns the uid of the process at the other end of a connected Unix socket, from LOCAL_PEERCRED
// (the macOS spelling of getpeereid). An unreadable or unversioned credential is an error: callers refuse.
func peerUIDOS(c *net.UnixConn) (uint32, error) {
	rc, err := c.SyscallConn()
	if err != nil {
		return 0, err
	}
	var cred *unix.Xucred
	var cerr error
	if err := rc.Control(func(fd uintptr) {
		cred, cerr = unix.GetsockoptXucred(int(fd), unix.SOL_LOCAL, unix.LOCAL_PEERCRED)
	}); err != nil {
		return 0, err
	}
	if cerr != nil {
		return 0, cerr
	}
	if cred == nil || cred.Version != xucredVersion {
		return 0, errors.New("unexpected peer credential version")
	}
	return cred.Uid, nil
}

// ownerUID is the owner of a file.
func ownerUID(fi fs.FileInfo) (uint32, bool) {
	s, ok := fi.Sys().(*syscall.Stat_t)
	if !ok {
		return 0, false
	}
	return s.Uid, true
}
