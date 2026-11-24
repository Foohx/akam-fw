package cli_test

import (
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// A write the camera applied but could not save ("saved":false in the PATCH reply) is said, as the old text port said "not saved".
func TestAnAPIWriteThatCouldNotBeSavedIsReported(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.FlashSaveFails = true })
	r := e.run("", "led", "status", "off")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "could not save it: it will be lost at the next reboot")
	if e.cam.CtlValue("led.status") != "off" {
		t.Error("the value was not applied")
	}
	e2 := newEnv(t)
	r = e2.run("", "led", "status", "off")
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, "lost at the next reboot")
}
