//go:build akamtest

package sdcard

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func TestHelperMainRefusesUnknownFieldsAndSecondDocuments(t *testing.T) {
	const size = 192 << 20
	_, p := sparse(t, size)
	good, _ := json.Marshal(imageReq(OpWipe, p, size))
	for name, body := range map[string]string{
		"unknown field": strings.Replace(string(good), `"op"`, `"path_to_read":"/etc/shadow","op"`, 1),
		"two documents": string(good) + string(good),
	} {
		var out bytes.Buffer
		if rc := HelperMain(strings.NewReader(body), &out, Helper{}); rc != 2 {
			t.Errorf("%s: rc %d: %s", name, rc, out.String())
		}
	}
}

func TestWipeReportsProgress(t *testing.T) {
	const size = 192 << 20
	_, p := sparse(t, size)
	var prog bytes.Buffer
	if r := (Helper{Progress: &prog}).Handle(imageReq(OpWipe, p, size)); !r.OK {
		t.Fatal(r.Error)
	}
	if !strings.Contains(prog.String(), "wipe: 0%") || !strings.Contains(prog.String(), "wipe: 9") {
		t.Errorf("progress: %q", prog.String())
	}
}
