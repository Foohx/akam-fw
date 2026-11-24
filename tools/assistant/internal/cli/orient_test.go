package cli_test

import (
	"strings"
	"testing"
)

// The 1080p orientation rows (image.flip, image.mirror, image.ceiling) in the three places an option can be set: the picture
// command, the options screens and the saved config applied to a camera (what is written at install time). The fake camera answers
// like the firmware (bool 0|1, ceiling = both at once). Not yet verified on a real 1080p camera.

func TestPictureFlipMirrorCeiling(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "picture")
	expectRC(t, r, 0)
	contains(t, r.out, "Flip (vertical) 0, mirror (horizontal) 0")
	r = e.run("", "picture", "flip", "on", "mirror", "1")
	expectRC(t, r, 0)
	contains(t, r.out, "  Done.", "Flip (vertical) 1, mirror (horizontal) 1")
	if got := strings.Join(e.cam.CtlSets(), "|"); got != "SET image.flip 1|SET image.mirror 1" {
		t.Errorf("control lines: %s", got)
	}
	r = e.run("", "picture", "ceiling", "off")
	expectRC(t, r, 0)
	contains(t, r.out, "Flip (vertical) 0, mirror (horizontal) 0")
	if e.cam.CtlValue("image.flip") != "0" || e.cam.CtlValue("image.mirror") != "0" {
		t.Error("ceiling off did not turn both off")
	}
	r = e.run("", "picture", "ceiling", "on")
	expectRC(t, r, 0)
	if e.cam.CtlValue("image.flip") != "1" || e.cam.CtlValue("image.mirror") != "1" || e.cam.CtlValue("image.ceiling") != "1" {
		t.Error("ceiling on did not turn both on")
	}
	n := len(e.cam.CtlSets())
	for _, bad := range [][]string{{"picture", "flip"}, {"picture", "flip", "maybe"}, {"picture", "flip", "2"}, {"picture", "flip", "on", "flip", "off"},
		{"picture", "ceiling", "on", "flip", "on"}, {"picture", "mirror", "on", "ceiling", "on"}, {"picture", "reset", "flip", "on"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	if len(e.cam.CtlSets()) != n {
		t.Error("a bad word reached the camera")
	}
	// an image from before the rows: the picture screen says nothing about them
	e = newEnv(t)
	e.cam.SetCtl("image.flip", "-")
	r = e.run("", "picture")
	expectRC(t, r, 0)
	notContains(t, r.out, "Flip (vertical)")
	// dry run: what would be sent, nothing is
	e = newEnv(t)
	r = e.run("", "--dry-run", "picture", "mirror", "on")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD send: SET image.mirror 1")
	if len(e.cam.CtlSets()) != 0 {
		t.Error("dry run sent a SET")
	}
}

func TestVideoPointsFlipAndMirrorToThePictureCommand(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "video", "flip", "on")
	expectRC(t, r, 2)
	contains(t, r.err, "picture flip on|off")
}

func TestOptionsScreenListsTheOrientationRows(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("3\n2\n6\n8\n10\n", "--camera", "salon") // Camera options > Picture
	expectRC(t, r, 0)
	contains(t, r.out, "Flip (vertical)", "Mirror (horizontal)", "Ceiling mount (flip + mirror)")
}

func TestConfigAppliesTheOrientationOfATemplate(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "image.flip", "1", "image.mirror", "1")
	r := e.cr("y\n\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Flip (vertical)", "off  ->  on", "Mirror (horizontal)", "WILL send: SET image.flip 1", "applied")
	if got := sets(e); !strings.Contains(got, "SET image.flip 1") || !strings.Contains(got, "SET image.mirror 1") {
		t.Errorf("sets: %s", got)
	}
	if e.cam.CtlValue("image.flip") != "1" || e.cam.CtlValue("image.mirror") != "1" {
		t.Error("the camera does not have the orientation of the config")
	}
	// the camera has it now: a second apply has nothing to do
	r = e.cr("", "config", "apply", "--global")
	contains(t, r.out, "Nothing to change")
}

func TestConfigSaveRecordsTheOrientationOfTheCamera(t *testing.T) {
	e := cfgEnv(t)
	e.cam.SetCtl("image.flip", "1")
	r := e.cr("\n", "config", "save-global")
	expectRC(t, r, 0)
	g := e.loadCfg(e.globalPlace())
	if g.Get("image.flip") != "1" || g.Get("image.mirror") != "0" {
		t.Errorf("image.flip = %q, image.mirror = %q", g.Get("image.flip"), g.Get("image.mirror"))
	}
}
