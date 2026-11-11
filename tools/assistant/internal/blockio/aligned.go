// Package blockio adapts a raw sector device (a Windows physical drive, a macOS raw disk) to the byte-offset
// interface the card writers use. Raw nodes only accept whole, aligned sectors in aligned buffers.
package blockio

import (
	"errors"
	"io"
	"sync"
	"unsafe"
)

// BufferAlign is the alignment of every buffer handed to the OS (a page).
const BufferAlign = 4096

// Raw is a device that only takes sector-aligned offsets, lengths and (page-)aligned buffers.
type Raw interface {
	ReadAt(p []byte, off int64) (int, error)
	WriteAt(p []byte, off int64) (int, error)
	Sync() error
	// SectorSize is the logical sector size in bytes (a power of two).
	SectorSize() int
}

// Aligned implements ReadAt, WriteAt and Sync with arbitrary offsets and lengths over a Raw: whole sectors
// pass through, partial sectors at either end are read, patched and written back.
type Aligned struct {
	raw Raw
	ss  int64
	mu  sync.Mutex // serialises the read-modify-write cycles
}

// New wraps raw. It fails when the sector size is not a power of two.
func New(raw Raw) (*Aligned, error) {
	ss := raw.SectorSize()
	if ss <= 0 || ss&(ss-1) != 0 {
		return nil, errors.New("blockio: sector size is not a power of two")
	}
	return &Aligned{raw: raw, ss: int64(ss)}, nil
}

// alignedBuf returns a zeroed n-byte slice whose address is a multiple of BufferAlign.
func alignedBuf(n int64) []byte {
	b := make([]byte, n+BufferAlign)
	skew := int(uintptr(unsafe.Pointer(&b[0])) % BufferAlign)
	off := 0
	if skew != 0 {
		off = BufferAlign - skew
	}
	return b[off : off+int(n) : off+int(n)]
}

func (a *Aligned) down(x int64) int64 { return x &^ (a.ss - 1) }
func (a *Aligned) up(x int64) int64   { return (x + a.ss - 1) &^ (a.ss - 1) }

// ReadAt reads len(p) bytes at off. A short read at the end of the device returns io.EOF like os.File.
func (a *Aligned) ReadAt(p []byte, off int64) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if off < 0 {
		return 0, errors.New("blockio: negative offset")
	}
	start, end := a.down(off), a.up(off+int64(len(p)))
	buf := alignedBuf(end - start)
	n, err := a.raw.ReadAt(buf, start)
	have := int64(n) - (off - start)
	if have < 0 {
		have = 0
	}
	if have > int64(len(p)) {
		have = int64(len(p))
	}
	copy(p, buf[off-start:off-start+have])
	if have == int64(len(p)) {
		return len(p), nil
	}
	if err == nil {
		err = io.ErrUnexpectedEOF
	}
	return int(have), err
}

// WriteAt writes p at off, preserving the bytes of the partial sectors around it.
func (a *Aligned) WriteAt(p []byte, off int64) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if off < 0 {
		return 0, errors.New("blockio: negative offset")
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	start, end := a.down(off), a.up(off+int64(len(p)))
	buf := alignedBuf(end - start)
	if off != start { // partial first sector
		if err := a.readSector(buf[:a.ss], start); err != nil {
			return 0, err
		}
	}
	if last := end - a.ss; off+int64(len(p)) != end && (last != start || off == start) { // partial last sector
		if err := a.readSector(buf[last-start:last-start+a.ss], last); err != nil {
			return 0, err
		}
	}
	copy(buf[off-start:], p)
	n, err := a.raw.WriteAt(buf, start)
	if err != nil {
		return 0, err
	}
	if int64(n) != int64(len(buf)) {
		return 0, io.ErrShortWrite
	}
	return len(p), nil
}

// readSector reads one sector; a sector past the end of the device reads as zeros.
func (a *Aligned) readSector(b []byte, off int64) error {
	tmp := alignedBuf(a.ss) // b may start in the middle of a page: the OS needs an aligned buffer
	_, err := a.raw.ReadAt(tmp, off)
	if err != nil && !errors.Is(err, io.EOF) {
		return err
	}
	copy(b, tmp)
	return nil
}

// Sync flushes the device.
func (a *Aligned) Sync() error { return a.raw.Sync() }
