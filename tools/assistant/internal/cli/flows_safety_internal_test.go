package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The guided flows only chain the audited commands. This reads their source: no flow writes a flash,
// builds a camera command line, kills a process, reboots or touches the watchdog by itself, and the
// single audited video restart is the only kill (called from the option screens and the video
// command, never from anywhere else).
func TestFlowFilesHaveNoDirectDestructiveCalls(t *testing.T) {
	files, _ := filepath.Glob("flows_*.go")
	if len(files) < 5 {
		t.Fatalf("flow files: %v", files)
	}
	forbidden := []string{"flash.Flash(", "flash.Confirmation", "i-understand-no-uart", "kill", "poweroff", "watchdog",
		"of=/dev", "sdcard.", "a.run(", "RunStream", "exec.Command", "os.RemoveAll", "os.Remove("}
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") {
			continue
		}
		var code []string
		raw, _ := os.ReadFile(f)
		for _, l := range strings.Split(string(raw), "\n") {
			if !strings.HasPrefix(strings.TrimSpace(l), "//") {
				code = append(code, l)
			}
		}
		b := []byte(strings.Join(code, "\n"))
		for _, w := range forbidden {
			// the one read-only helper call of the option screens is allowed: remoteNtp("status")
			s := strings.ReplaceAll(string(b), `a.run(c, remoteNtp("status"), nil, "")`, "")
			if strings.Contains(s, w) {
				t.Errorf("%s contains %q: flows must go through the audited commands", f, w)
			}
		}
	}
}

func TestRestartVideoIsOnlyCalledFromAuditedPlaces(t *testing.T) {
	allowed := map[string]bool{"remote.go": true, "optcmds.go": true, "flows_options.go": true, "flows_options_form.go": true}
	all, _ := filepath.Glob("*.go")
	for _, f := range all {
		if strings.HasSuffix(f, "_test.go") || allowed[f] {
			continue
		}
		b, _ := os.ReadFile(f)
		if strings.Contains(string(b), "restartVideo") {
			t.Errorf("%s calls restartVideo", f)
		}
	}
}

// Every Danger call of the flows names a flash.Confirmation-backed path: the flows never call
// UI.Danger themselves (the red boxes live in flashConfirm and the SD commands).
func TestFlowsNeverOpenTheirOwnRedBox(t *testing.T) {
	files, _ := filepath.Glob("flows_*.go")
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") {
			continue
		}
		b, _ := os.ReadFile(f)
		if strings.Contains(string(b), ".Danger(") {
			t.Errorf("%s opens a red box: destructive steps go through flashConfirm or the SD commands", f)
		}
	}
}
