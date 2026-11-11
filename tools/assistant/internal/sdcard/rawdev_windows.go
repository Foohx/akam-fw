//go:build windows

package sdcard

import (
	"errors"
	"fmt"
	"io"
	"sync"
	"syscall"

	"golang.org/x/sys/windows"
)

const (
	shareRW    = windows.FILE_SHARE_READ | windows.FILE_SHARE_WRITE
	maxIOChunk = 4 << 20 // one ReadFile/WriteFile call; a multiple of every sector size
)

// errno extracts the Windows error number of err (0xFFFFFFFF when err is not a system error).
func errno(err error) uint32 {
	var e syscall.Errno
	if errors.As(err, &e) {
		return uint32(e)
	}
	return 0xFFFFFFFF
}

// ioctl calls DeviceIoControl once. in and out may be nil.
func ioctl(h windows.Handle, code uint32, in, out []byte) (uint32, error) {
	var inp, outp *byte
	if len(in) > 0 {
		inp = &in[0]
	}
	if len(out) > 0 {
		outp = &out[0]
	}
	var ret uint32
	err := windows.DeviceIoControl(h, code, inp, uint32(len(in)), outp, uint32(len(out)), &ret, nil)
	return ret, err
}

// ioctlGrow calls an IOCTL whose answer has a size the caller cannot know: the output buffer starts at start
// bytes and doubles on ERROR_INSUFFICIENT_BUFFER or ERROR_MORE_DATA, up to maxWinBuffer.
func ioctlGrow(h windows.Handle, code uint32, in []byte, start int) ([]byte, error) {
	for size := start; size <= maxWinBuffer; size *= 2 {
		out := make([]byte, size)
		n, err := ioctl(h, code, in, out)
		switch {
		case err == nil:
			if int(n) > len(out) {
				return nil, errors.New("the system returned more bytes than the buffer holds")
			}
			return out[:n], nil
		case errors.Is(err, windows.ERROR_INSUFFICIENT_BUFFER), errors.Is(err, windows.ERROR_MORE_DATA):
			continue
		}
		return nil, err
	}
	return nil, fmt.Errorf("the answer is larger than %d bytes", maxWinBuffer)
}

// openPath opens a device or volume path. access 0 is enough for the query IOCTLs and needs no rights.
func openPath(path string, access, flags uint32) (windows.Handle, error) {
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return windows.InvalidHandle, err
	}
	return windows.CreateFile(p, access, shareRW, nil, windows.OPEN_EXISTING, flags, 0)
}

// rawDev is a physical drive handle with ReadFile/WriteFile at explicit offsets (OVERLAPPED offset on a
// synchronous handle), not an *os.File: no reliance on seek semantics of device handles. It implements
// blockio.Raw and is only ever used through blockio.Aligned.
type rawDev struct {
	mu sync.Mutex // the offset travels in each call, but ReadFile/WriteFile on a synchronous handle also move the pointer
	h  windows.Handle
	ss int
}

func (r *rawDev) SectorSize() int { return r.ss }

func ovAt(off int64) *windows.Overlapped {
	return &windows.Overlapped{Offset: uint32(off), OffsetHigh: uint32(off >> 32)}
}

func (r *rawDev) ReadAt(p []byte, off int64) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	done := 0
	for done < len(p) {
		chunk := p[done:]
		if len(chunk) > maxIOChunk {
			chunk = chunk[:maxIOChunk]
		}
		var n uint32
		err := windows.ReadFile(r.h, chunk, &n, ovAt(off+int64(done)))
		done += int(n)
		if err != nil {
			if errors.Is(err, windows.ERROR_HANDLE_EOF) {
				return done, io.EOF
			}
			return done, err
		}
		if n == 0 {
			return done, io.EOF
		}
	}
	return done, nil
}

func (r *rawDev) WriteAt(p []byte, off int64) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	done := 0
	for done < len(p) {
		chunk := p[done:]
		if len(chunk) > maxIOChunk {
			chunk = chunk[:maxIOChunk]
		}
		var n uint32
		err := windows.WriteFile(r.h, chunk, &n, ovAt(off+int64(done)))
		done += int(n)
		if err != nil {
			return done, err
		}
		if n == 0 {
			return done, io.ErrShortWrite
		}
	}
	return done, nil
}

func (r *rawDev) Sync() error { return windows.FlushFileBuffers(r.h) }

func (r *rawDev) Close() error {
	if r.h == 0 || r.h == windows.InvalidHandle {
		return nil
	}
	h := r.h
	r.h = windows.InvalidHandle
	return windows.CloseHandle(h)
}

// updateProperties asks the system to re-read the partition table of the disk (IOCTL_DISK_UPDATE_PROPERTIES).
func (r *rawDev) updateProperties() error {
	_, err := ioctl(r.h, ioctlDiskUpdateProperties, nil, nil)
	return err
}

// openRawWrite opens disk n for writing: read/write, share read|write, write-through, extended DASD I/O.
func openRawWrite(n, sectorSize int) (*rawDev, error) {
	h, err := openPath(WinPhysicalPath(n), windows.GENERIC_READ|windows.GENERIC_WRITE, windows.FILE_FLAG_WRITE_THROUGH)
	if err != nil {
		return nil, fmt.Errorf("cannot open %s for writing: %w", WinPhysicalPath(n), err)
	}
	// lets the handle address the whole disk, including the sectors past the last cylinder; a failure here
	// shows as a failed write, never as a wrong one
	_, _ = ioctl(h, fsctlAllowExtendedDasdIo, nil, nil)
	return &rawDev{h: h, ss: sectorSize}, nil
}

// openRawVerify opens disk n a second time, read-only and uncached (FILE_FLAG_NO_BUFFERING), for the read-back.
func openRawVerify(n, sectorSize int) (*rawDev, error) {
	h, err := openPath(WinPhysicalPath(n), windows.GENERIC_READ, windows.FILE_FLAG_NO_BUFFERING)
	if err != nil {
		return nil, fmt.Errorf("cannot open %s for the uncached read-back: %w", WinPhysicalPath(n), err)
	}
	_, _ = ioctl(h, fsctlAllowExtendedDasdIo, nil, nil)
	return &rawDev{h: h, ss: sectorSize}, nil
}
