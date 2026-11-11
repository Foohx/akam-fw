//go:build linux

package sdcard

import (
	"os"
	"syscall"
)

func osUnmount(point string) error { return syscall.Unmount(point, 0) }

// osRescan asks the kernel to re-read the partition table (BLKRRPART); best effort.
func osRescan(f *os.File) {
	const blkrrpart = 0x125f
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), blkrrpart, 0)
}
