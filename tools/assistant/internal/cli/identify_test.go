package cli_test

import "testing"

// find-camera on an Akam camera reads fw.version and sys.hostname (read-only), records them, and
// `cameras` shows them; an older image is "Akam, version unknown"; the camera only ever receives
// GET and QUIT on its control port.
func TestFindCameraIdentifiesAkamAndCamerasShowsIt(t *testing.T) {
	e := newEnv(t)
	e.cam.SetCtl("sys.hostname", "akam\x1b[2J-01")
	r := e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "Firmware b35-2af81c5, host name akam-01.")
	notContains(t, r.out, "\x1b")
	if e.readState("camera_fw") != "b35-2af81c5" || e.readState("camera_host") != "akam-01" || e.readState("camera_seen") == "" {
		t.Errorf("state: fw=%q host=%q", e.readState("camera_fw"), e.readState("camera_host"))
	}
	for _, w := range e.cam.CtlWords() {
		if w != "GET" && w != "QUIT" {
			t.Errorf("control port received %q", w)
		}
	}
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "cameras")
	contains(t, r.out, "host akam-01", "fw b35-2af81c5", "seen just now")

	// an older image: ERR for both keys
	e = newEnv(t)
	e.cam.SetCtl("fw.version", "-")
	e.cam.SetCtl("sys.hostname", "-")
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "Akam, firmware version unknown")
}
