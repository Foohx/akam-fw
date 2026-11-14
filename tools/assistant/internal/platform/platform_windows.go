//go:build windows

package platform

import (
	"os"
	"syscall"

	"golang.org/x/sys/windows"
)

var (
	sigTerm os.Signal = syscall.SIGTERM
	sigHup  os.Signal // no hangup signal on Windows
)

func signalSet() []os.Signal { return []os.Signal{os.Interrupt, syscall.SIGTERM} }

// EnableVT turns on ENABLE_VIRTUAL_TERMINAL_PROCESSING on stdout and stderr. A stream that is not
// a console (redirected) is fine; a console that refuses the mode makes it return false.
func EnableVT() bool {
	ok := true
	for _, h := range []windows.Handle{windows.Handle(os.Stdout.Fd()), windows.Handle(os.Stderr.Fd())} {
		var mode uint32
		if windows.GetConsoleMode(h, &mode) != nil {
			continue // not a console
		}
		if mode&windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0 {
			continue
		}
		if windows.SetConsoleMode(h, mode|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING) != nil {
			ok = false
		}
	}
	return ok
}
