package cli_test

import (
	"io"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// orderWriter records how many camera control exchanges had happened when each "SET ..." line was shown.
type orderWriter struct {
	w    io.Writer
	sets func() int
	seen *[]int
}

func (o orderWriter) Write(b []byte) (int, error) {
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "SET ") {
			*o.seen = append(*o.seen, o.sets())
		}
	}
	return o.w.Write(b)
}

// TestCommandShownBeforeRun (spec 6.4): the SET line of a change is in the transcript before the camera
// receives it, asserted by counting the exchanges the fake camera has seen when the line is shown.
func TestCommandShownBeforeRun(t *testing.T) {
	e := formEnv(t)
	var seen []int
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		for _, c := range [][2]string{{"led.status", "off"}, {"isp.brightness", "10"}} {
			if _, err := spec.Apply(c[0], c[1]); err != nil {
				t.Errorf("apply %v: %v", c, err)
			}
		}
		return ui.FormResult{}
	}}
	t.Setenv("AKAM_FORCE_TTY", "1")
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		p := cli.NewPlainUI(o, st, log, secrets, dry)
		p.Out = orderWriter{p.Out, func() int { return len(e.cam.CtlSets()) }, &seen}
		fs.boardSession = &boardSession{Plain: p}
		return formOuter{p, fs}
	}
	expectRC(t, runWith(e, h, "", append(e.base(), "--camera", "salon")...), 0)
	if len(seen) != 2 || seen[0] != 0 || seen[1] != 1 {
		t.Errorf("exchanges already sent when each SET line was shown: %v (want [0 1])", seen)
	}
}

// TestNoNetworkBeforeOnvifYes (spec 6.4, with TestNoNetworkBeforeScanYes): the ONVIF search of the
// chooser sends no packet at all (no WS-Discovery probe, no TCP dial) before an explicit yes, from the
// board key and from the numbered menu; a yes sends exactly the probe.
func TestNoNetworkBeforeOnvifYes(t *testing.T) {
	for _, tc := range []struct {
		name, stdin string
		packets     bool
	}{{"default no", "\n", false}, {"explicit no", "n\n", false}, {"q", "q\n", false}, {"end of input", "", false}, {"yes", "y\n", true}} {
		t.Run("board/"+tc.name, func(t *testing.T) {
			e := newEnv(t)
			e.fakeLAN("192.0.2.99")
			pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Skip("no loopback UDP: " + err.Error())
			}
			defer pc.Close()
			var pk, dials atomic.Int32
			go func() {
				buf := make([]byte, 4096)
				for {
					if _, _, err := pc.ReadFrom(buf); err != nil {
						return
					}
					pk.Add(1)
				}
			}()
			e.disc.WSDTarget = pc.LocalAddr().String()
			e.countDial(&dials, false)
			bs := &boardSession{keys: []string{"c", "o", "esc", "esc"}}
			expectRC(t, runBoard(t, e, bs, tc.stdin), 0)
			time.Sleep(100 * time.Millisecond) // the datagram is already on loopback; this only lets the reader run
			if got := pk.Load() > 0; got != tc.packets {
				t.Fatalf("packets=%d, want packets=%v", pk.Load(), tc.packets)
			}
			if !tc.packets && dials.Load() != 0 {
				t.Fatalf("%d dials before a yes", dials.Load())
			}
		})
	}
	t.Run("menu/default no", func(t *testing.T) {
		e := newEnv(t)
		e.rememberCamera("salon", "192.0.2.21")
		e.fakeLAN("192.0.2.21")
		pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
		if err != nil {
			t.Skip("no loopback UDP: " + err.Error())
		}
		defer pc.Close()
		var pk atomic.Int32
		go func() {
			buf := make([]byte, 4096)
			for {
				if _, _, err := pc.ReadFrom(buf); err != nil {
					return
				}
				pk.Add(1)
			}
		}()
		e.disc.WSDTarget = pc.LocalAddr().String()
		r := e.noIP("5\n4\n\n7\n9\n", "--camera", "salon") // find > ONVIF search > Enter (No) > back
		expectRC(t, r, 0)
		time.Sleep(100 * time.Millisecond)
		if pk.Load() != 0 {
			t.Errorf("%d packets before a yes", pk.Load())
		}
	})
}
