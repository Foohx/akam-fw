//go:build linux

package sdcard

import (
	"fmt"
	"os"
	"strconv"
	"strings"
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

// linuxOpener writes /dev/sdX and /dev/mmcblkN opened O_RDWR|O_EXCL after umount2 of every mount point.
type linuxOpener struct{}

func (linuxOpener) CheckNode(dev string) error {
	st, err := os.Lstat(dev)
	if err != nil || st.Mode()&os.ModeDevice == 0 || st.Mode()&os.ModeCharDevice != 0 {
		return fmt.Errorf("refusing: %s is not a block device", dev)
	}
	return nil
}

func (linuxOpener) Unmount(point string) error { return osUnmount(point) }

func (linuxOpener) Open(dev string, c *Candidate) (Disk, error) {
	f, err := os.OpenFile(dev, os.O_RDWR|os.O_EXCL, 0)
	if err != nil {
		return nil, err
	}
	// what was opened must be the disk of the identity token (the path could have been swapped since the check)
	if err := checkOpened(f, c.Ident); err != nil {
		f.Close()
		return nil, err
	}
	return fileDisk{f: f}, nil
}

// checkOpened fstat()s the opened descriptor and compares its device number with the major:minor of the token.
func checkOpened(f *os.File, ident string) error {
	st, err := f.Stat()
	if err != nil {
		return fmt.Errorf("refusing: cannot stat the opened device: %v", err)
	}
	if st.Mode()&os.ModeDevice == 0 || st.Mode()&os.ModeCharDevice != 0 {
		return fmt.Errorf("refusing: the opened device %s is not a block device", f.Name())
	}
	sys, ok := st.Sys().(*syscall.Stat_t)
	if !ok {
		return fmt.Errorf("refusing: cannot read the device number of %s", f.Name())
	}
	return matchRdev(uint64(sys.Rdev), ident, f.Name())
}

// matchRdev compares an st_rdev with the major:minor that ends the identity token.
func matchRdev(rdev uint64, ident, name string) error {
	want := ident[strings.LastIndex(ident, "|")+1:]
	maj, min := rdevMajMin(rdev)
	parts := strings.SplitN(want, ":", 2)
	if len(parts) != 2 {
		return fmt.Errorf("refusing: the identity token has no device number to compare with %s", name)
	}
	wm, e1 := strconv.ParseUint(parts[0], 10, 32)
	wn, e2 := strconv.ParseUint(parts[1], 10, 32)
	if e1 != nil || e2 != nil || uint64(maj) != wm || uint64(min) != wn {
		return fmt.Errorf("refusing: the opened device %s is %d:%d, not %s as confirmed (nothing was written)", name, maj, min, want)
	}
	return nil
}

// rdevMajMin splits a Linux dev_t (the glibc/kernel encoding of st_rdev).
func rdevMajMin(rdev uint64) (major, minor uint32) {
	major = uint32(((rdev >> 8) & 0xfff) | ((rdev >> 32) &^ 0xfff))
	minor = uint32((rdev & 0xff) | ((rdev >> 12) &^ 0xff))
	return
}
