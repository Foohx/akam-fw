package sdcard

import (
	"bytes"
	"crypto/rand"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func sparse(t *testing.T, size int64) (*os.File, string) {
	t.Helper()
	p := filepath.Join(t.TempDir(), "card.img")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(size); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.Close() })
	return f, p
}

func rnd(n int) []byte { b := make([]byte, n); rand.Read(b); return b }

func sampleFiles() []File {
	return []File{
		{"hack.sh", []byte("#!/bin/sh\necho hi\n")},
		{"custom/configs/hack.conf", []byte("TELNET=1\n")},
		{"custom/configs/wifi.conf", []byte("MODE=sta\n")},
		{"custom/state/dump.done", []byte{}},
		{"custom_firmware/ak_rtsp_firmware_20251111_101010.squashfs", rnd(5 * 1024 * 1024)},
		{"flash_tool", rnd(300000)},
		{"original_firmware/mtdblock7.bin", rnd(5242880)},
		{"UPPER.TXT", []byte("short name only")},
		{"a name with spaces and ÉÀ.txt", []byte("unicode")},
		{"long_name_number_one_0001.txt", []byte("1")},
		{"long_name_number_one_0002.txt", []byte("2")},
	}
}

func TestFAT32RoundTripAndFsck(t *testing.T) {
	const size = 256 << 20
	f, p := sparse(t, size)
	off, psize, err := FormatMBR(f, size)
	if err != nil {
		t.Fatal(err)
	}
	files := sampleFiles()
	if err := WriteFAT32(f, off, psize, files, WriteOptions{}); err != nil {
		t.Fatal(err)
	}
	card, err := OpenCard(f)
	if err != nil {
		t.Fatal(err)
	}
	if card.Label() != FATLabel {
		t.Errorf("label %q", card.Label())
	}
	for _, x := range files {
		got, err := card.ReadFile(x.Path)
		if err != nil || !bytes.Equal(got, x.Data) {
			t.Errorf("%s: err=%v equal=%v", x.Path, err, bytes.Equal(got, x.Data))
		}
	}
	ls, _ := card.List("custom/configs")
	if fmt.Sprint(ls) != "[hack.conf wifi.conf]" {
		t.Errorf("list %v", ls)
	}
	if card.Exists("nope") || !card.Exists("custom") {
		t.Error("Exists")
	}
	if fsck, err := exec.LookPath("fsck.vfat"); err == nil {
		// the partition starts at 1 MiB: give fsck the offset through a copy of the partition
		part := filepath.Join(t.TempDir(), "part.img")
		pf, _ := os.Create(part)
		buf := make([]byte, psize)
		f.ReadAt(buf, off)
		pf.Write(buf)
		pf.Close()
		out, err := exec.Command(fsck, "-n", part).CombinedOutput()
		if err != nil {
			t.Errorf("fsck.vfat: %v\n%s", err, out)
		}
	} else {
		t.Log("fsck.vfat not installed: skipped")
	}
	_ = p
}

func TestMBR(t *testing.T) {
	const size = 256 << 20
	f, _ := sparse(t, size)
	if _, _, err := FormatMBR(f, size); err != nil {
		t.Fatal(err)
	}
	b := make([]byte, 512)
	f.ReadAt(b, 0)
	if b[510] != 0x55 || b[511] != 0xAA || b[446] != 0x80 || b[450] != 0x0C {
		t.Errorf("mbr % x", b[440:512])
	}
	if b[462] != 0 || b[478] != 0 || b[494] != 0 {
		t.Error("more than one partition")
	}
}

func TestTooSmall(t *testing.T) {
	f, _ := sparse(t, 16<<20)
	if _, _, err := FormatMBR(f, 16<<20); err != ErrTooSmall {
		t.Errorf("got %v", err)
	}
	const size = 128 << 20
	g, _ := sparse(t, size)
	off, ps, _ := FormatMBR(g, size)
	big := []File{{"big.bin", make([]byte, 140<<20)}}
	if err := WriteFAT32(g, off, ps, big, WriteOptions{}); err != ErrTooSmall {
		t.Errorf("got %v", err)
	}
}

func TestBadNames(t *testing.T) {
	f, _ := sparse(t, 128<<20)
	off, ps, _ := FormatMBR(f, 128<<20)
	for _, n := range []string{"../x", "/abs", "a:b", "a/../../b", "trail."} {
		if err := WriteFAT32(f, off, ps, []File{{n, nil}}, WriteOptions{}); err == nil {
			t.Errorf("%q accepted", n)
		}
	}
}
