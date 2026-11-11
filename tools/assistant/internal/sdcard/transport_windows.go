//go:build windows

package sdcard

import (
	"fmt"
	"io"
	"runtime"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// The Windows transport: the decisions are in uac.go. NeedsElevation is simply "this process is not elevated":
// raw access to a physical drive needs the administrator token, whatever the disk.

type winTransport struct{ t *uacTransport }

func newTransport() Transport {
	return winTransport{newUACTransport(processElevated, createPipeListener, launchRunas, runInProcess)}
}

func processElevated() bool { return windows.GetCurrentProcessToken().IsElevated() }

func (winTransport) NeedsElevation(*Candidate) bool { return !processElevated() }

func (w winTransport) RunElevated(self, _ string, req Request, stderr io.Writer) (Response, error) {
	return w.t.run(self, req, stderr)
}

// runInProcess is the "Run as administrator" path: this process already holds the rights, so the helper runs in
// it: no prompt, no pipe.
func runInProcess(req Request, stderr io.Writer) (Response, error) {
	p := newPlatform()
	h := Helper{Inv: p.Inventory, Opener: p.Opener}
	if stderr != nil {
		h.Progress = progressToLines{stderr}
	}
	return h.Handle(req), nil
}

// progressToLines shows helper progress the way the pipe path does: one sanitized line each.
type progressToLines struct{ w io.Writer }

func (p progressToLines) Write(b []byte) (int, error) {
	fmt.Fprintln(p.w, sanitizeLine(string(b)))
	return len(b), nil
}

const (
	seeMaskNoCloseProcess = 0x00000040
	seeMaskNoAsync        = 0x00000100
	seeMaskFlagNoUI       = 0x00000400
)

// shellExecuteInfo is SHELLEXECUTEINFOW.
type shellExecuteInfo struct {
	cbSize       uint32
	fMask        uint32
	hwnd         uintptr
	lpVerb       *uint16
	lpFile       *uint16
	lpParameters *uint16
	lpDirectory  *uint16
	nShow        int32
	hInstApp     uintptr
	lpIDList     uintptr
	lpClass      *uint16
	hkeyClass    uintptr
	dwHotKey     uint32
	hIcon        uintptr
	hProcess     windows.Handle
}

var procShellExecuteExW = windows.NewLazySystemDLL("shell32.dll").NewProc("ShellExecuteExW")

// launchRunas starts `self params` elevated, hidden, through the UAC prompt, and returns the process (its handle
// is kept open: SEE_MASK_NOCLOSEPROCESS). A declined prompt is a uacLaunchError with ERROR_CANCELLED.
func launchRunas(self, params string) (uacProc, error) {
	// ShellExecuteEx wants a single-threaded COM apartment on the calling thread
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := windows.CoInitializeEx(0, windows.COINIT_APARTMENTTHREADED|windows.COINIT_DISABLE_OLE1DDE); err == nil {
		defer windows.CoUninitialize()
	}
	verb, err := windows.UTF16PtrFromString("runas")
	if err != nil {
		return nil, err
	}
	file, err := windows.UTF16PtrFromString(self)
	if err != nil {
		return nil, err
	}
	args, err := windows.UTF16PtrFromString(params)
	if err != nil {
		return nil, err
	}
	info := shellExecuteInfo{
		fMask:        seeMaskNoCloseProcess | seeMaskNoAsync | seeMaskFlagNoUI,
		lpVerb:       verb,
		lpFile:       file,
		lpParameters: args,
		nShow:        windows.SW_HIDE,
	}
	info.cbSize = uint32(unsafe.Sizeof(info))
	r, _, e := procShellExecuteExW.Call(uintptr(unsafe.Pointer(&info)))
	runtime.KeepAlive(verb)
	runtime.KeepAlive(file)
	runtime.KeepAlive(args)
	if r == 0 {
		code := uint32(0)
		if en, ok := e.(syscall.Errno); ok {
			code = uint32(en)
		}
		return nil, &uacLaunchError{Code: code, Err: e}
	}
	if info.hProcess == 0 {
		return nil, &uacLaunchError{Err: fmt.Errorf("no process handle came back")}
	}
	pid, err := windows.GetProcessId(info.hProcess)
	if err != nil {
		windows.CloseHandle(info.hProcess)
		return nil, err
	}
	p := &winProc{h: info.hProcess, pid: pid, exited: make(chan error, 1), done: make(chan struct{}), stopped: make(chan struct{})}
	go p.watch()
	return p, nil
}

type winProc struct {
	h       windows.Handle
	pid     uint32
	exited  chan error
	done    chan struct{} // closed by Close
	stopped chan struct{} // closed when watch returns
}

func (p *winProc) PID() uint32          { return p.pid }
func (p *winProc) Exited() <-chan error { return p.exited }

func (p *winProc) watch() {
	defer close(p.stopped)
	for {
		select {
		case <-p.done:
			return
		default:
		}
		r, err := windows.WaitForSingleObject(p.h, uint32(500*time.Millisecond/time.Millisecond))
		if err != nil {
			p.exited <- err
			return
		}
		if r == windows.WAIT_OBJECT_0 {
			var code uint32
			if err := windows.GetExitCodeProcess(p.h, &code); err != nil {
				p.exited <- err
			} else if code != 0 {
				p.exited <- fmt.Errorf("the helper exited with status %d", code)
			} else {
				p.exited <- nil
			}
			return
		}
	}
}

func (p *winProc) Close() {
	select {
	case <-p.done:
		return
	default:
	}
	close(p.done)
	<-p.stopped
	windows.CloseHandle(p.h)
}
