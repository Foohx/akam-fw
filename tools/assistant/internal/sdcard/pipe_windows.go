//go:build windows

package sdcard

import (
	"errors"
	"io"
	"os"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// The named pipe of the UAC transport (uac.go decides, this file calls the system). Both ends use overlapped I/O
// so that every wait has a deadline and Close ends a blocked call.

const waitTimeout = 258 // WAIT_TIMEOUT

var errPipeTimeout = errors.New("the pipe did not answer in time")

type pipeConn struct {
	h        windows.Handle
	deadline atomic.Int64 // unix nanoseconds, 0 = none
	once     sync.Once
}

func (c *pipeConn) SetDeadline(t time.Time) error {
	if t.IsZero() {
		c.deadline.Store(0)
	} else {
		c.deadline.Store(t.UnixNano())
	}
	return nil
}

// wait returns the milliseconds left before the deadline (INFINITE when there is none).
func (c *pipeConn) wait() (uint32, bool) {
	d := c.deadline.Load()
	if d == 0 {
		return windows.INFINITE, true
	}
	left := time.Until(time.Unix(0, d))
	if left <= 0 {
		return 0, false
	}
	return uint32(left/time.Millisecond) + 1, true
}

// do runs one overlapped read or write of p.
func (c *pipeConn) do(p []byte, write bool) (int, error) {
	ms, ok := c.wait()
	if !ok {
		return 0, errPipeTimeout
	}
	ev, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return 0, err
	}
	defer windows.CloseHandle(ev)
	ov := &windows.Overlapped{HEvent: ev}
	var n uint32
	if write {
		err = windows.WriteFile(c.h, p, &n, ov)
	} else {
		err = windows.ReadFile(c.h, p, &n, ov)
	}
	if err == windows.ERROR_IO_PENDING {
		r, werr := windows.WaitForSingleObject(ev, ms)
		if werr != nil || r == waitTimeout {
			_ = windows.CancelIoEx(c.h, ov)
			_ = windows.GetOverlappedResult(c.h, ov, &n, true) // the buffer is the system's until it returns
			if werr == nil {
				werr = errPipeTimeout
			}
			return int(n), werr
		}
		err = windows.GetOverlappedResult(c.h, ov, &n, false)
	}
	switch err {
	case nil:
		return int(n), nil
	case windows.ERROR_BROKEN_PIPE, windows.ERROR_PIPE_NOT_CONNECTED:
		return int(n), io.EOF
	}
	return int(n), err
}

func (c *pipeConn) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	n, err := c.do(p, false)
	if n == 0 && err == nil {
		return 0, io.EOF
	}
	return n, err
}

func (c *pipeConn) Write(p []byte) (int, error) {
	total := 0
	for len(p) > 0 {
		chunk := p
		if len(chunk) > 1<<20 {
			chunk = chunk[:1<<20]
		}
		n, err := c.do(chunk, true)
		total += n
		if err != nil {
			return total, err
		}
		p = p[n:]
	}
	return total, nil
}

func (c *pipeConn) Close() error {
	c.once.Do(func() {
		_ = windows.CancelIoEx(c.h, nil)
		windows.CloseHandle(c.h)
	})
	return nil
}

// pipeListener owns the single instance until Accept hands it to a pipeConn.
type pipeListener struct {
	h windows.Handle
	// mu guards the three flags: the handle is closed exactly once, by whoever finds it unowned.
	mu        sync.Mutex
	accepting bool
	taken     bool
	closed    bool
}

// createPipeListener creates the pipe: this process is the first (and only) creator of the name, one instance,
// byte mode, no remote clients, and a DACL that names the current user only.
func createPipeListener(name string) (uacListener, error) {
	tok := windows.GetCurrentProcessToken()
	u, err := tok.GetTokenUser()
	if err != nil {
		return nil, err
	}
	sddl, err := pipeSDDL(u.User.Sid.String())
	if err != nil {
		return nil, err
	}
	sd, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return nil, err
	}
	sa := &windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})), SecurityDescriptor: sd}
	np, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	h, err := windows.CreateNamedPipe(np,
		windows.PIPE_ACCESS_DUPLEX|windows.FILE_FLAG_FIRST_PIPE_INSTANCE|windows.FILE_FLAG_OVERLAPPED,
		windows.PIPE_TYPE_BYTE|windows.PIPE_READMODE_BYTE|windows.PIPE_WAIT|windows.PIPE_REJECT_REMOTE_CLIENTS,
		1, 64<<10, 64<<10, 0, sa)
	if err != nil {
		return nil, err
	}
	return &pipeListener{h: h}, nil
}

func (l *pipeListener) Accept(timeout time.Duration) (uacConn, error) {
	l.mu.Lock()
	if l.closed || l.taken {
		l.mu.Unlock()
		return nil, os.ErrClosed
	}
	l.accepting = true
	l.mu.Unlock()
	err := l.connect(timeout)
	l.mu.Lock()
	defer l.mu.Unlock()
	l.accepting = false
	if err != nil || l.closed {
		if !l.closed {
			l.closed = true
		}
		windows.CloseHandle(l.h)
		if err == nil {
			err = os.ErrClosed
		}
		return nil, err
	}
	l.taken = true
	return &pipeConn{h: l.h}, nil
}

func (l *pipeListener) connect(timeout time.Duration) error {
	ev, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return err
	}
	defer windows.CloseHandle(ev)
	ov := &windows.Overlapped{HEvent: ev}
	err = windows.ConnectNamedPipe(l.h, ov)
	switch err {
	case windows.ERROR_PIPE_CONNECTED:
		return nil // the client came before this call
	case windows.ERROR_IO_PENDING:
	default:
		return err
	}
	r, werr := windows.WaitForSingleObject(ev, uint32(timeout/time.Millisecond))
	if werr != nil || r == waitTimeout {
		_ = windows.CancelIoEx(l.h, ov)
		var n uint32
		_ = windows.GetOverlappedResult(l.h, ov, &n, true)
		if werr == nil {
			werr = errPipeTimeout
		}
		return werr
	}
	var n uint32
	return windows.GetOverlappedResult(l.h, ov, &n, false)
}

func (l *pipeListener) ClientPID(uacConn) (uint32, error) {
	var pid uint32
	if err := windows.GetNamedPipeClientProcessId(l.h, &pid); err != nil {
		return 0, err
	}
	return pid, nil
}

func (l *pipeListener) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.taken || l.closed {
		return nil // the connection owns the handle, or Accept already closed it
	}
	if l.accepting {
		l.closed = true
		_ = windows.CancelIoEx(l.h, nil) // Accept sees the flag and closes the handle
		return nil
	}
	l.closed = true
	return windows.CloseHandle(l.h)
}

// dialPipeOS is the helper side: open the pipe (retrying while it does not exist yet or is busy) at the
// identification impersonation level, so that whoever serves the pipe can never act as the elevated helper.
func dialPipeOS(name string, wait time.Duration) (uacConn, error) {
	np, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	end := time.Now().Add(wait)
	for {
		h, err := windows.CreateFile(np, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, nil, windows.OPEN_EXISTING,
			windows.FILE_FLAG_OVERLAPPED|windows.SECURITY_SQOS_PRESENT|windows.SECURITY_IDENTIFICATION, 0)
		if err == nil {
			return &pipeConn{h: h}, nil
		}
		if (err != windows.ERROR_FILE_NOT_FOUND && err != windows.ERROR_PIPE_BUSY) || time.Now().After(end) {
			return nil, err
		}
		time.Sleep(100 * time.Millisecond)
	}
}
