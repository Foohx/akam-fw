//go:build unix

package sdcard

import (
	"io"
	"strings"
	"testing"
)

// Elsewhere than macOS, osascript elevation and the socket helper refuse.
func TestOsascriptRefusedOffMac(t *testing.T) {
	if _, err := (unixTransport{}).RunElevated("", "osascript", Request{Op: OpList}, io.Discard); err == nil {
		t.Skip("running on macOS")
	}
	if code, handled := HelperEntry([]string{"sd-helper", "--socket", "/x/s.sock", "--uid", "1", "--nonce", strings.Repeat("a", 32)}, strings.NewReader(""), io.Discard, io.Discard); !handled || code == 0 {
		t.Errorf("code %d handled %v", code, handled)
	}
}
