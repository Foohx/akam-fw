//go:build linux

package sdcard

import (
	"os"
	"syscall"
)

func osUnmount(point string) error { return syscall.Unmount(point, 0) }

// osRescan asks the kernel to re-read the partition table (BLKRRPART); best effort.
func osRescan(f *os.File) {
	const blkrrpart, blkflsbuf = 0x125f, 0x1261
	// drop the cached blocks first, so that the read-back that follows comes from the card itself
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), blkflsbuf, 0)
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), blkrrpart, 0)
}
