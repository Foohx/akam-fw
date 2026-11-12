//go:build !akamtest

package audio

import "flag"

// DefaultBackend is the built-in microphone of this OS, nil when there is none. It is also nil
// inside a `go test` binary (the test flags are registered), even without the akamtest tag, so a
// plain `go test` can never connect to the desktop sound server.
func DefaultBackend() MicBackend {
	if flag.Lookup("test.v") != nil {
		return nil
	}
	return defaultBackend()
}
