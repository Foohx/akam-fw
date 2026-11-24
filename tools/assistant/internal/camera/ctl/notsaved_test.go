package ctl

import (
	"context"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
)

func TestNotSavedBecomesTheOldNote(t *testing.T) {
	f := &fakeClient{patchRes: akamapi.PatchResult{Status: 200, Results: map[string]akamapi.KeyResult{"led.mode": {OK: true, Apply: "applied", NotSaved: true}}}}
	got, _ := Exchange(context.Background(), f, []string{"SET led.status off"})
	if len(got) != 1 || got[0] != "OK led.status=off (not saved: the camera could not write it to flash)" {
		t.Fatalf("%q", got)
	}
	if r := ParseLedSet("led.status", "off", got); !r.OK || !r.NotSaved {
		t.Errorf("%+v", r)
	}
	if r := ParseSet(got); !r.OK || !r.NotSaved {
		t.Errorf("%+v", r)
	}
}
