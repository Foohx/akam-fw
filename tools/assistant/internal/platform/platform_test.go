package platform

import (
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
)

func TestStateDirFor(t *testing.T) {
	env := func(m map[string]string) func(string) string { return func(k string) string { return m[k] } }
	home := filepath.Join("home", "u")
	cases := []struct {
		goos string
		env  map[string]string
		want string
	}{
		{"linux", nil, filepath.Join(home, ".local", "state", "akam")},
		{"linux", map[string]string{"XDG_STATE_HOME": "xdg"}, filepath.Join("xdg", "akam")},
		{"darwin", map[string]string{"XDG_STATE_HOME": "xdg"}, filepath.Join(home, "Library", "Application Support", "Akam")},
		{"windows", map[string]string{"LOCALAPPDATA": "lad", "APPDATA": "roam"}, filepath.Join("lad", "Akam")},
		{"windows", nil, filepath.Join(home, "AppData", "Local", "Akam")},
	}
	for _, c := range cases {
		if got := StateDirFor(c.goos, env(c.env), home); got != c.want {
			t.Errorf("%s %v: got %q want %q", c.goos, c.env, got, c.want)
		}
	}
}

func TestSignalsAndExitCodes(t *testing.T) {
	got := SignalsToIgnore()
	has := func(s os.Signal) bool {
		for _, x := range got {
			if x == s {
				return true
			}
		}
		return false
	}
	if !has(os.Interrupt) || !has(syscall.SIGTERM) {
		t.Fatalf("missing INT or TERM: %v", got)
	}
	if runtime.GOOS != "windows" && !has(syscall.SIGHUP) {
		t.Fatalf("hangup missing on %s", runtime.GOOS)
	}
	if ExitCodeFor(os.Interrupt) != 130 || ExitCodeFor(syscall.SIGTERM) != 143 {
		t.Fatal("exit codes")
	}
	if runtime.GOOS != "windows" && ExitCodeFor(syscall.SIGHUP) != 129 {
		t.Fatal("hangup exit code")
	}
}
