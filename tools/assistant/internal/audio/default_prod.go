//go:build !akamtest

package audio

// DefaultBackend is the built-in microphone of this OS, nil when there is none.
func DefaultBackend() MicBackend { return defaultBackend() }
