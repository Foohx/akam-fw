//go:build darwin

package sdcard

import (
	"fmt"
	"syscall"
)

// devNumDarwin is "major:minor" of the raw node of /dev/diskN, from stat (part of the identity token). The raw
// node, not the block node, is what is written and what Open checks again with fstat.
func devNumDarwin(dev string) (string, error) {
	var st syscall.Stat_t
	if err := syscall.Stat(rawPath(dev), &st); err != nil {
		return "", err
	}
	return fmtDevNum(uint32(st.Rdev)), nil
}

// fmtDevNum splits a darwin dev_t: major in the top 8 bits, minor in the low 24.
func fmtDevNum(rdev uint32) string { return fmt.Sprintf("%d:%d", rdev>>24, rdev&0xffffff) }
