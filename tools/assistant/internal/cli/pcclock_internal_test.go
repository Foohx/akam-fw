package cli

import (
	"strings"
	"testing"
	"time"
)

// A computer whose own clock is not plausible (dead battery: 1970 or 2000) is not a reference: the
// clock of the camera is neither offered nor set from it, and the report says why.
func TestClockNotOfferedFromAnImplausiblePC(t *testing.T) {
	good := int64(1893456000) // 2030
	for name, h := range map[string]camHealth{
		"camera unset, PC at 1970":  {timeSet: "0", camEpoch: 946684800, pcEpoch: 0},
		"camera unset, PC at 2000":  {timeSet: "0", camEpoch: 946684800, pcEpoch: 946684900},
		"camera set but off by 1 y": {timeSet: "1", camEpoch: good, pcEpoch: 946684900},
	} {
		old := pcNow
		pcNow = func() time.Time { return time.Unix(0, 0) }
		lines, offer := clockReport(h)
		pcNow = old
		if offer {
			t.Errorf("%s: offered", name)
		}
		if got := strings.Join(lines, "\n"); !strings.Contains(got, "clock of this computer") {
			t.Errorf("%s: no explanation in %q", name, got)
		}
	}
	// a sane PC keeps the offer
	old := pcNow
	pcNow = func() time.Time { return time.Unix(good, 0) }
	defer func() { pcNow = old }()
	if lines, offer := clockReport(camHealth{timeSet: "0", camEpoch: 946684800}); !offer || len(lines) != 1 {
		t.Errorf("sane PC: offer=%v %v", offer, lines)
	}
	if _, offer := clockReport(camHealth{timeSet: "0", camEpoch: 946684800, pcEpoch: good}); !offer {
		t.Error("sane PC with a measured epoch: not offered")
	}
}
