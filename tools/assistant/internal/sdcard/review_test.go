package sdcard

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A mount whose source is a link name (/dev/disk/by-uuid/...) is still found by its device number.
func TestMountNamedByLinkIsOnTheDisk(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "USB root", "8:16")
	f.part("sdb", "sdb1", "8:17")
	f.mounts("21 1 8:17 / / rw - ext4 /dev/disk/by-uuid/1234-ABCD rw")
	if _, err := f.Sys.Check(f.dev("sdb")); err == nil || !strings.Contains(err.Error(), "system path /") {
		t.Errorf("got %v", err)
	}
}

// A loop device that uses the card itself as its backing device makes it a part of a stack.
func TestLoopBackedDiskIsRefused(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "Card", "8:16")
	f.put("sys/devices/virtual/block/loop3/loop/backing_file", "/dev/sdb")
	if err := os.Symlink(filepath.Join(f.dir, "sys/devices/virtual/block/loop3"), filepath.Join(f.dir, "sys/class/block/loop3")); err != nil {
		t.Fatal(err)
	}
	if _, err := f.Sys.Check(f.dev("sdb")); err == nil || !strings.Contains(err.Error(), "backing device") {
		t.Errorf("got %v", err)
	}
}

// Like wipefs -a: an old GPT (primary header and backup) must not survive the new MBR.
func TestFormatMBRClearsOldGPT(t *testing.T) {
	const size = 192 << 20
	f, _ := sparse(t, size)
	sig := []byte("EFI PART")
	f.WriteAt(sig, 512)
	f.WriteAt(sig, size-512)
	if _, _, err := FormatMBR(f, size); err != nil {
		t.Fatal(err)
	}
	b := make([]byte, 8)
	for _, at := range []int64{512, size - 512} {
		f.ReadAt(b, at)
		if string(b) == "EFI PART" {
			t.Errorf("GPT signature left at %d", at)
		}
	}
}
