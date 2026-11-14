package cli_test

import (
	"strings"
	"testing"
)

// The home screen on the plain UI: the header names the camera (or none), the eight actions are
// listed, and quitting is the last number.
func TestHomeScreenPlain(t *testing.T) {
	e := newEnv(t)
	r := e.run("10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Akam test  (", "Camera: salon  127.0.0.1  address known, not checked yet  (item 8 chooses another camera)",
		"Set up a new camera  - First install from the stock firmware.",
		"Update this camera  - Akam to Akam, over SSH, no card.",
		"Camera options", "Back up / restore", "Find cameras on my network", "Settings", "Check this computer", "Change camera")
	contains(t, r.out, "What do you want to do?")
}

func TestHomeScreenNoCamera(t *testing.T) {
	e := newEnv(t)
	r := e.runRaw("10\n", "--no-color", "--state-dir", e.state, "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Camera: none (item 8 chooses one)")
	if strings.Contains(r.out, "127.0.0.1") {
		t.Error("an address without a chosen camera")
	}
}
