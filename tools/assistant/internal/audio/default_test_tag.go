//go:build akamtest

package audio

// DefaultBackend is always nil in test builds (-tags akamtest): no test may ever connect to the
// sound server or the microphone of the machine it runs on.
func DefaultBackend() MicBackend { return nil }
