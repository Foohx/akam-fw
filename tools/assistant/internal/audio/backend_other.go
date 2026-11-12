//go:build !linux && !(windows && (amd64 || arm64))

package audio

// defaultBackend: macOS (CoreAudio needs cgo) and other systems have no built-in microphone;
// talk uses ffmpeg there.
func defaultBackend() MicBackend { return nil }
