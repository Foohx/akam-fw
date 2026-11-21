//go:build linux

package sdcard

import (
	"os"
	"strings"
	"testing"
)

func TestRdevMajMin(t *testing.T) {
	// makedev(8,16) = 0x810 on the 64-bit dev_t
	maj, min := rdevMajMin(0x810)
	if maj != 8 || min != 16 {
		t.Errorf("0x810 -> %d:%d", maj, min)
	}
	// 179:0 (mmcblk0) = (179 << 8)
	maj, min = rdevMajMin(179 << 8)
	if maj != 179 || min != 0 {
		t.Errorf("179:0 -> %d:%d", maj, min)
	}
	// a minor above 255: 8:300 -> low byte 44, high bits shifted by 12
	rdev := uint64(8<<8) | uint64(300&0xff) | uint64(300&^0xff)<<12
	maj, min = rdevMajMin(rdev)
	if maj != 8 || min != 300 {
		t.Errorf("8:300 -> %d:%d", maj, min)
	}
}

func TestMatchRdev(t *testing.T) {
	tok := "34359738368|SD Reader|S-sdb||Generic|8:16"
	if err := matchRdev(0x810, tok, "/dev/sdb"); err != nil {
		t.Errorf("same device refused: %v", err)
	}
	for name, rdev := range map[string]uint64{"other minor": 0x820, "other major": 9<<8 | 16} {
		if err := matchRdev(rdev, tok, "/dev/sdb"); err == nil || !strings.Contains(err.Error(), "not 8:16") {
			t.Errorf("%s: got %v", name, err)
		}
	}
	if err := matchRdev(0x810, "no-device-number", "/dev/sdb"); err == nil {
		t.Error("a token without major:minor was accepted")
	}
}

// What the opener opened must be a block device: a character device (/dev/null) is refused after the open.
func TestCheckOpenedRefusesNonBlockNodes(t *testing.T) {
	f, err := os.OpenFile("/dev/null", os.O_RDWR, 0)
	if err != nil {
		t.Skip("no /dev/null")
	}
	defer f.Close()
	if err := checkOpened(f, "1|x|||y|1:3"); err == nil {
		t.Error("a character device was accepted")
	}
	g, _ := os.CreateTemp(t.TempDir(), "x")
	defer g.Close()
	if err := checkOpened(g, "1|x|||y|1:3"); err == nil {
		t.Error("a regular file was accepted")
	}
}
