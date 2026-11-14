package cli

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
)

func TestWSDItemStripsTerminalControls(t *testing.T) {
	got := wsdItem(discover.Found{IP: "192.0.2.5", Name: "cam\x1b[2J\x1b]0;x\x07\rname", XAddrs: []string{"http://h/\x1b[31m"}})
	if strings.ContainsAny(got, "\x1b\x07\r") || got != "192.0.2.5  camname  http://h/" {
		t.Errorf("%q", got)
	}
}
