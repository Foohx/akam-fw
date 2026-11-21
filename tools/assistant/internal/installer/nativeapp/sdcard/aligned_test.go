package sdcard

import (
	"bytes"
	"io"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/Foohx/akam-fw/tools/assistant/internal/blockio"
)

// strictDev is an in-memory raw device that answers EINVAL to anything unaligned.
type strictDev struct{ data []byte }

const strictSS = 512

func (d *strictDev) SectorSize() int { return strictSS }
func (d *strictDev) bad(p []byte, off int64) bool {
	return off%strictSS != 0 || len(p)%strictSS != 0 || (len(p) > 0 && uintptr(unsafe.Pointer(&p[0]))%blockio.BufferAlign != 0)
}
func (d *strictDev) ReadAt(p []byte, off int64) (int, error) {
	if d.bad(p, off) {
		return 0, syscall.EINVAL
	}
	if off >= int64(len(d.data)) {
		return 0, io.EOF
	}
	return copy(p, d.data[off:]), nil
}
func (d *strictDev) WriteAt(p []byte, off int64) (int, error) {
	if d.bad(p, off) {
		return 0, syscall.EINVAL
	}
	return copy(d.data[off:], p), nil
}
func (d *strictDev) Sync() error { return nil }

// The whole MBR + FAT32 writer through blockio.Aligned on an alignment-enforcing device gives the same bytes as
// the direct write on a plain file.
func TestWritersThroughAlignedEqualTheDirectWrite(t *testing.T) {
	const size = 48 << 20
	files := sampleFiles()
	opts := WriteOptions{Now: time.Date(2026, 1, 2, 3, 4, 6, 0, time.UTC), VolumeID: 0x12345678}

	f, p := sparse(t, size)
	off, psize, err := FormatMBR(f, size)
	if err != nil {
		t.Fatal(err)
	}
	if err := WriteFAT32(f, off, psize, files, opts); err != nil {
		t.Fatal(err)
	}
	direct := make([]byte, size)
	if _, err := f.ReadAt(direct, 0); err != nil {
		t.Fatal(err)
	}
	_ = p

	raw := &strictDev{data: make([]byte, size)}
	a, err := blockio.New(raw)
	if err != nil {
		t.Fatal(err)
	}
	off2, psize2, err := FormatMBR(a, size)
	if err != nil {
		t.Fatal(err)
	}
	if off2 != off || psize2 != psize {
		t.Fatalf("partition %d/%d vs %d/%d", off2, psize2, off, psize)
	}
	if err := WriteFAT32(a, off2, psize2, files, opts); err != nil {
		t.Fatal(err)
	}
	// the MBR disk signature is random: compare without it
	for _, b := range [][]byte{direct, raw.data} {
		for i := 440; i < 444; i++ {
			b[i] = 0
		}
	}
	if !bytes.Equal(direct, raw.data) {
		t.Fatal("the write through Aligned differs from the direct write")
	}
	card, err := OpenCard(a)
	if err != nil {
		t.Fatal(err)
	}
	for _, x := range files {
		got, err := card.ReadFile(x.Path)
		if err != nil || !bytes.Equal(got, x.Data) {
			t.Errorf("%s: read-back through Aligned: %v", x.Path, err)
		}
	}
}
