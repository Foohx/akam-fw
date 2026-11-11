//go:build !unix && !windows

package sdcard

import "io"

// An OS with no elevation transport (neither Unix nor Windows) says so.
type unsupportedTransport struct{}

func newTransport() Transport { return unsupportedTransport{} }

func (unsupportedTransport) NeedsElevation(*Candidate) bool { return true }

func (unsupportedTransport) RunElevated(string, string, Request, io.Writer) (Response, error) {
	return Response{}, ErrUnsupported
}
