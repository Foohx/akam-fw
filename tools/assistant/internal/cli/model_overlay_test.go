package cli_test

import (
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

const rotModel = "lsc-rotatable-2k-ak3918av130"

// The assistant never offers to install or update a kit on a decloud-overlay model (Phase 1 of its port keeps the
// stock app); the 1080p flows are covered unchanged by model_test.go and the rest of the suite.

func TestGetKitIsRefusedForADecloudOverlayModel(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false), relJSONFor(rotModel, "v2.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\n")
	r := e.run("", "get-kit", "--model", rotModel)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not yet supported for this model")
	if len(a.hits) != 0 {
		t.Errorf("nothing may be requested from the network: %v", a.hits)
	}
	if e.readState("camera_model") != "" || e.readState("kit_model") != "" {
		t.Error("a refused model must not be recorded")
	}
}

func TestReleasesAndRecordedOverlayModelAreRefused(t *testing.T) {
	e := newEnv(t)
	useAPI(t, e, newFwAPI(t, relList(relJSONFor(rotModel, "v2.0.0", false, false))))
	e.writeState("camera_model=" + rotModel + "\n")
	r := e.run("", "releases")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not yet supported for this model")
}

func TestAKitBuiltForAnOverlayModelIsRefused(t *testing.T) {
	k := kittest.Build(t, t.TempDir(), kittest.Spec{Model: rotModel})
	e := getKitEnv(t, nil, &k)
	r := e.run("", "get-kit", "--kit", k.Tarball)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not yet supported for this model")
	if e.readState("kit_model") != "" {
		t.Error("a refused kit must not be recorded")
	}
}
