//go:build unix

package platform

import (
	"os"
	"syscall"
)

var (
	sigTerm os.Signal = syscall.SIGTERM
	sigHup  os.Signal = syscall.SIGHUP
)

func signalSet() []os.Signal { return []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP} }

// EnableVT: terminals understand ANSI sequences already.
func EnableVT() bool { return true }
