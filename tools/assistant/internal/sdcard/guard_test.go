package sdcard

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakeSys builds a /sys, /proc/self/mountinfo and /dev in a temp directory.
type fakeSys struct {
	t   *testing.T
	Sys Sys
	dir string
}

func newFakeSys(t *testing.T) *fakeSys {
	d := t.TempDir()
	f := &fakeSys{t: t, dir: d, Sys: Sys{SysRoot: filepath.Join(d, "sys"), MountInfo: filepath.Join(d, "mountinfo"),
		Swaps: filepath.Join(d, "swaps"), DevDir: filepath.Join(d, "dev")}}
	for _, p := range []string{"sys/class/block", "dev", "sys/devices"} {
		os.MkdirAll(filepath.Join(d, p), 0o755)
	}
	os.WriteFile(f.Sys.MountInfo, nil, 0o644)
	os.WriteFile(f.Sys.Swaps, []byte("Filename Type Size Used Priority\n"), 0o644)
	return f
}

func (f *fakeSys) put(p, v string) {
	fp := filepath.Join(f.dir, p)
	os.MkdirAll(filepath.Dir(fp), 0o755)
	if err := os.WriteFile(fp, []byte(v+"\n"), 0o644); err != nil {
		f.t.Fatal(err)
	}
}

// disk adds a disk: bus "usb" puts it below a /usb path (the class link points there).
func (f *fakeSys) disk(name string, sizeBytes int64, removable string, bus string, model string, majmin string) {
	real := filepath.Join(f.dir, "sys/devices/pci0", bus, name)
	os.MkdirAll(real, 0o755)
	if err := os.Symlink(real, filepath.Join(f.dir, "sys/class/block", name)); err != nil {
		f.t.Fatal(err)
	}
	base := filepath.Join("sys/devices/pci0", bus, name)
	f.put(base+"/size", fmt.Sprint(sizeBytes/512))
	f.put(base+"/removable", removable)
	f.put(base+"/dev", majmin)
	f.put(base+"/device/model", model)
	f.put(base+"/device/vendor", "Generic")
	f.put(base+"/device/serial", "S-"+name)
	if strings.HasPrefix(name, "mmcblk") {
		f.put(base+"/device/type", "SD")
	}
	// the /dev node is a regular file here (a fake)
	os.WriteFile(filepath.Join(f.dir, "dev", name), nil, 0o644)
}

func (f *fakeSys) part(disk, name, majmin string) {
	// find real dir of the disk through the class link
	real, _ := filepath.EvalSymlinks(filepath.Join(f.dir, "sys/class/block", disk))
	os.MkdirAll(filepath.Join(real, name), 0o755)
	os.WriteFile(filepath.Join(real, name, "partition"), []byte("1\n"), 0o644)
	os.WriteFile(filepath.Join(real, name, "dev"), []byte(majmin+"\n"), 0o644)
}

func (f *fakeSys) mounts(lines ...string) {
	os.WriteFile(f.Sys.MountInfo, []byte(strings.Join(lines, "\n")+"\n"), 0o644)
}

func (f *fakeSys) dev(name string) string { return filepath.Join(f.dir, "dev", name) }

const gib = 1 << 30

func TestGuardMatrix(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "SD Reader  ", "8:16")
	f.disk("sdc", 32*gib, "0", "usb1", "Hard disk", "8:32")  // not removable
	f.disk("sdd", 32*gib, "1", "ata1", "Odd SATA", "8:48")   // removable flag but not USB
	f.disk("sde", 64*1024*1024, "1", "usb2", "Tiny", "8:64") // too small
	f.disk("sdf", 256*gib, "1", "usb3", "Huge", "8:80")      // too big
	f.disk("sdg", 16*gib, "1", "usb4", "Sys", "8:96")        // carries /home
	f.part("sdg", "sdg1", "8:97")
	f.disk("sdh", 16*gib, "1", "usb5", "Swap", "8:112")
	f.part("sdh", "sdh1", "8:113")
	f.disk("sdi", 16*gib, "1", "usb6", "Stack", "8:128")
	os.MkdirAll(filepath.Join(f.dir, "sys/devices/pci0/usb6/sdi/holders/dm-0"), 0o755)
	f.disk("sdj", 16*gib, "1", "usb7", "Repo", "8:144") // carries the working directory
	f.part("sdj", "sdj1", "8:145")
	f.disk("mmcblk0", 16*gib, "0", "mmc", "SD16G", "179:0")
	f.disk("mmcblk1", 16*gib, "0", "mmc", "eMMC", "179:8")
	f.put("sys/devices/pci0/mmc/mmcblk1/device/type", "MMC")
	f.mounts(
		"20 1 8:97 / /home rw - ext4 /dev/sdg1 rw",
		"21 1 8:200 / / rw - ext4 /dev/sda2 rw",
		"22 1 8:145 / /work/repo rw - ext4 /dev/sdj1 rw",
		"23 1 0:50 /sub /mnt/card rw - btrfs /dev/sdb rw", // a mount of the card itself (not a system path)
	)
	os.WriteFile(f.Sys.Swaps, []byte("Filename Type Size Used Priority\n/dev/sdh1 partition 1000 0 -2\n"), 0o644)
	f.Sys.GuardPaths = []string{"/work/repo/src"}

	ok := []string{"sdb", "mmcblk0"}
	for _, n := range ok {
		c, err := f.Sys.Check(f.dev(n))
		if err != nil {
			t.Errorf("%s refused: %v", n, err)
			continue
		}
		if c.Size <= 0 || c.Ident == "" {
			t.Errorf("%s: %+v", n, c)
		}
	}
	c, _ := f.Sys.Check(f.dev("sdb"))
	if c.Model != "SD Reader" || c.Tran != "usb" || len(c.Mounts) != 1 || c.Mounts[0].Point != "/mnt/card" {
		t.Errorf("candidate: %+v", c)
	}
	refuse := map[string]string{
		"sdc": "not removable", "sdd": "not USB", "sde": "outside 128 MiB", "sdf": "outside 128 MiB",
		"sdg": "system path /home", "sdh": "swap", "sdi": "storage stack", "sdj": "working or the temp directory",
		"mmcblk1": "not an SD card",
	}
	for n, why := range refuse {
		_, err := f.Sys.Check(f.dev(n))
		if err == nil || !strings.Contains(err.Error(), why) {
			t.Errorf("%s: want refusal %q, got %v", n, why, err)
		}
	}
	// names that are never whole USB/SD disks
	for _, n := range []string{"sdb1", "nvme0n1", "loop0", "zram0", "mmcblk0p1", "sda"} {
		if _, err := f.Sys.Check(f.dev(n)); err == nil {
			t.Errorf("%s accepted", n)
		}
	}
	if _, err := f.Sys.Check("/dev/sdb"); err == nil {
		t.Error("a path outside the device directory was accepted")
	}
	// the list only has the eligible ones
	var names []string
	for _, c := range f.Sys.List() {
		names = append(names, c.Name)
	}
	if fmt.Sprint(names) != "[mmcblk0 sdb]" {
		t.Errorf("list %v", names)
	}
}

func TestRootDiskIsRefused(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "USB root", "8:16")
	f.part("sdb", "sdb1", "8:17")
	f.mounts("21 1 8:17 / / rw - ext4 /dev/sdb1 rw")
	if _, err := f.Sys.Check(f.dev("sdb")); err == nil || !strings.Contains(err.Error(), "system path /") {
		t.Errorf("got %v", err)
	}
}

func TestRecheckDetectsAChangedCard(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "Card A", "8:16")
	c, err := f.Sys.Check(f.dev("sdb"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.Sys.Recheck(f.dev("sdb"), c.Ident); err != nil {
		t.Fatal(err)
	}
	f.put("sys/devices/pci0/usb1/sdb/device/model", "Card B")
	if _, err := f.Sys.Recheck(f.dev("sdb"), c.Ident); err == nil || !strings.Contains(err.Error(), "changed") {
		t.Errorf("got %v", err)
	}
}

func TestSymlinkedDeviceIsRefusedByTheCheck(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "Card", "8:16")
	link := filepath.Join(f.dir, "dev", "sdz")
	if err := os.Symlink(f.dev("sdb"), link); err != nil {
		t.Fatal(err)
	}
	if _, err := f.Sys.Check(link); err == nil {
		t.Error("a link was accepted")
	}
}
