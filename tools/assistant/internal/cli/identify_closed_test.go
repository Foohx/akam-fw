package cli_test

import "testing"

// The text port is closed to the network on a current image: SSH and video answering (with the Dropbear banner) is a camera running
// Akam; its firmware and name are not read from the port.
func TestFindCameraWithTheTextPortClosed(t *testing.T) {
	e := newEnv(t)
	r := e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "are open and telnet is closed: this is a camera running Akam.", "is closed to the network, as it should be")
	if e.readState("camera_kind") != "akam" {
		t.Errorf("kind %q", e.readState("camera_kind"))
	}
	for _, w := range e.cam.CtlWords() {
		t.Errorf("the closed text port received %q", w)
	}
}
