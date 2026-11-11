//go:build !unix

package sdcard

import "io"

// The Windows transport (UAC, named pipe) arrives with T11.9; until then this build says so.
type unsupportedTransport struct{}

func newTransport() Transport { return unsupportedTransport{} }

func (unsupportedTransport) NeedsElevation(*Candidate) bool { return true }

func (unsupportedTransport) RunElevated(string, string, Request, io.Writer) (Response, error) {
	return Response{}, ErrUnsupported
}
