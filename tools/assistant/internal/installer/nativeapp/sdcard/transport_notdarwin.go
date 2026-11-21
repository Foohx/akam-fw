//go:build !darwin

package sdcard

import (
	"errors"
	"io/fs"
	"net"
)

// The peer-credential check exists on macOS only: elsewhere the osascript transport refuses.
func peerUIDOS(c *net.UnixConn) (uint32, error) {
	return 0, errors.New("peer credentials are only read on macOS")
}

// ownerUID is only meaningful for the socket directory check, which runs on macOS.
func ownerUID(fs.FileInfo) (uint32, bool) { return 0, false }
