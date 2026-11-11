//go:build !unix && !windows

package platform

import (
	"os"
	"syscall"
)

var (
	sigTerm os.Signal = syscall.SIGTERM
	sigHup  os.Signal
)

func signalSet() []os.Signal { return []os.Signal{os.Interrupt, syscall.SIGTERM} }

// EnableVT: nothing to do.
func EnableVT() bool { return true }
