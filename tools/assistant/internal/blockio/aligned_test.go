package blockio

import (
	"bytes"
	"io"
	"math/rand"
	"syscall"
	"testing"
	"unsafe"
)

// strictRaw is an in-memory device that fails with EINVAL on any unaligned offset, length or buffer, like
// a Windows physical drive or a macOS raw disk.
type strictRaw struct {
	data  []byte
	ss    int
	calls int
}

func (r *strictRaw) SectorSize() int { return r.ss }

func (r *strictRaw) check(p []byte, off int64) error {
	r.calls++
	if off%int64(r.ss) != 0 || len(p)%r.ss != 0 || (len(p) > 0 && uintptr(unsafe.Pointer(&p[0]))%BufferAlign != 0) {
		return syscall.EINVAL
	}
	return nil
}

func (r *strictRaw) ReadAt(p []byte, off int64) (int, error) {
	if err := r.check(p, off); err != nil {
		return 0, err
	}
	if off >= int64(len(r.data)) {
		return 0, io.EOF
	}
	n := copy(p, r.data[off:])
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

func (r *strictRaw) WriteAt(p []byte, off int64) (int, error) {
	if err := r.check(p, off); err != nil {
		return 0, err
	}
	if off+int64(len(p)) > int64(len(r.data)) {
		return 0, syscall.ENOSPC
	}
	return copy(r.data[off:], p), nil
}

func (r *strictRaw) Sync() error { return nil }

func TestStrictFakeRejectsUnaligned(t *testing.T) {
	r := &strictRaw{data: make([]byte, 8192), ss: 512}
	if _, err := r.WriteAt(make([]byte, 100), 0); err != syscall.EINVAL {
		t.Errorf("short write: %v", err)
	}
	if _, err := r.ReadAt(alignedBuf(512), 3); err != syscall.EINVAL {
		t.Errorf("odd offset: %v", err)
	}
}

func TestAlignedMatchesAPlainSlice(t *testing.T) {
	for _, ss := range []int{512, 4096} {
		raw := &strictRaw{data: make([]byte, 64*1024), ss: ss}
		a, err := New(raw)
		if err != nil {
			t.Fatal(err)
		}
		want := make([]byte, len(raw.data))
		rng := rand.New(rand.NewSource(7))
		for i := 0; i < 300; i++ {
			n := rng.Intn(6000) + 1
			off := rng.Intn(len(want) - n)
			p := make([]byte, n) // a plain (unaligned) buffer
			rng.Read(p)
			if got, err := a.WriteAt(p, int64(off)); err != nil || got != n {
				t.Fatalf("ss=%d write %d@%d: %d, %v", ss, n, off, got, err)
			}
			copy(want[off:], p)
		}
		if !bytes.Equal(raw.data, want) {
			t.Fatalf("ss=%d: device differs from the reference", ss)
		}
		for i := 0; i < 300; i++ {
			n := rng.Intn(6000) + 1
			off := rng.Intn(len(want) - n)
			p := make([]byte, n)
			if got, err := a.ReadAt(p, int64(off)); err != nil || got != n || !bytes.Equal(p, want[off:off+n]) {
				t.Fatalf("ss=%d read %d@%d: %d, %v", ss, n, off, got, err)
			}
		}
	}
}

func TestAlignedWholeSectorsAreNotRead(t *testing.T) {
	raw := &strictRaw{data: make([]byte, 8192), ss: 512}
	a, _ := New(raw)
	if _, err := a.WriteAt(bytes.Repeat([]byte{1}, 1024), 512); err != nil {
		t.Fatal(err)
	}
	if raw.calls != 1 {
		t.Errorf("a whole-sector write took %d raw calls", raw.calls)
	}
}

func TestAlignedReadPastTheEnd(t *testing.T) {
	raw := &strictRaw{data: bytes.Repeat([]byte{9}, 1024), ss: 512}
	a, _ := New(raw)
	p := make([]byte, 100)
	n, err := a.ReadAt(p, 1000)
	if n != 24 || err != io.EOF {
		t.Errorf("got %d, %v", n, err)
	}
	if n, err := a.ReadAt(p, 2000); n != 0 || err != io.EOF {
		t.Errorf("past the end: %d, %v", n, err)
	}
}

func TestNewRefusesAWeirdSectorSize(t *testing.T) {
	if _, err := New(&strictRaw{ss: 500}); err == nil {
		t.Error("accepted")
	}
}
