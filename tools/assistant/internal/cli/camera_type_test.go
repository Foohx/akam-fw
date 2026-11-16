package cli_test

import (
	"strings"
	"testing"
)

const lsc1080 = "lsc-indoor-1080p-ak3918"

// A flow written for the 1080p must never run on a 2K: every one of them stops with the same refusal before it
// touches a camera, a card or a kit.
func TestFlowsAreRefusedForTheRotatable2K(t *testing.T) {
	for _, args := range [][]string{
		{"backup"}, {"restore"}, {"update"}, {"verify"}, {"image"}, {"flash"}, {"get-kit"},
		{"prepare-backup-sd"}, {"prepare-flash-sd"}, {"config", "show"}, {"led", "status"}, {"onvif", "status"},
		{"ntp", "status"}, {"mqtt", "status"}, {"video", "show"}, {"audio", "show"}, {"key", "show"}, {"hostname"},
		{"rtsp-auth", "status"}, {"step", "backup"}, {"step", "flash"}, {"step", "get-kit"},
	} {
		t.Run(strings.Join(args, "-"), func(t *testing.T) {
			e := newEnv(t)
			e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\nstep_doctor_done=2026-11-11T00:00:00Z\n")
			r := e.run("", args...)
			expectRC(t, r, 1)
			contains(t, r.out+r.err, "not yet supported for this model", "LSC Smart Rotatable Camera 2K", "nothing was changed on the camera")
			if e.readState("camera_model") != rotModel {
				t.Error("the type must stay")
			}
		})
	}
}

// --model alone (no recorded type) is refused too, so that a flow cannot be pointed at a 2K by mistake.
func TestModelFlagOfA2KRefusesTheFlows(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "backup", "--model", rotModel)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not yet supported for this model")
}

// The menu entries that run those flows refuse as well.
func TestMenuActionsAreRefusedForTheRotatable2K(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	// 3 = options, 4 = backup / restore, 2 = update, 9 = configs, 10 = quit
	r := e.run("3\n4\n2\n9\n10\n")
	expectRC(t, r, 0)
	if n := strings.Count(r.out+r.err, "not yet supported for this model"); n < 4 {
		t.Errorf("%d refusals, want 4:\n%s", n, r.out+r.err)
	}
}

// Existing profiles carry no type: they are 1080p, nothing is written for them, their flows are not refused.
func TestAProfileWithoutATypeIsA1080p(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_ip=192.0.2.10\nstep_doctor_done=2026-11-11T00:00:00Z\n")
	r := e.run("", "status")
	expectRC(t, r, 0)
	notContains(t, r.out, "Camera model:")
	r = e.run("", "ntp", "status")
	notContains(t, r.out+r.err, "not yet supported for this model")
	if e.readState("camera_model") != "" {
		t.Errorf("a migrated profile must not be rewritten: %q", e.readState("camera_model"))
	}
	e.writeState("camera_model=" + lsc1080 + "\ncamera_ip=192.0.2.10\n")
	r = e.run("", "ntp", "status")
	notContains(t, r.out+r.err, "not yet supported for this model")
}

// --camera NAME --model ID types a camera that is created by this run; an existing camera is never retyped.
func TestModelFlagTypesANewCameraOnly(t *testing.T) {
	e := newEnv(t)
	r := e.noIP("", "--camera", "garage", "--model", rotModel, "status")
	expectRC(t, r, 0)
	if got := e.readStateOf("garage", "camera_model"); got != rotModel {
		t.Errorf("garage type %q", got)
	}
	r = e.noIP("", "--camera", "garage", "--model", lsc1080, "status")
	expectRC(t, r, 0)
	if got := e.readStateOf("garage", "camera_model"); got != rotModel {
		t.Errorf("an existing camera was retyped: %q", got)
	}
	r = e.noIP("", "--camera", "porch", "status") // no --model: no type written, default 1080p
	expectRC(t, r, 0)
	if got := e.readStateOf("porch", "camera_model"); got != "" {
		t.Errorf("porch type %q", got)
	}
}

// The chooser asks the type of a new camera; the second item is the 2K, Cancel creates nothing.
func TestChooserAsksTheTypeOfANewCamera(t *testing.T) {
	e := newEnv(t)
	e.fakeLAN("192.0.2.58")
	r := e.noIP("5\n4\n192.0.2.58\n1\nkitchen\n2\n10\n", "--camera", "first")
	expectRC(t, r, 0)
	contains(t, r.out, "What type of camera is it?", "LSC Smart Indoor 1080p", "LSC Smart Rotatable Camera 2K")
	if got := e.readStateOf("kitchen", "camera_model"); got != rotModel {
		t.Errorf("kitchen type %q", got)
	}
	e2 := newEnv(t)
	e2.fakeLAN("192.0.2.59")
	r = e2.noIP("5\n4\n192.0.2.59\n1\nden\n3\n10\n", "--camera", "first") // Cancel
	expectRC(t, r, 0)
	if e2.readStateOf("den", "camera_ip") != "" || e2.readStateOf("den", "camera_model") != "" {
		t.Error("a cancelled type must create nothing")
	}
}
