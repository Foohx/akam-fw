package cli

import (
	"regexp"
	"strings"
	"testing"
)

// restartVideo is the one audited exception of the "no kill" rule (specs/installer.md): this exact command,
// nothing else, and it never names the watchdog.
func TestRestartVideoCommandIsExactAndTheOnlyKill(t *testing.T) {
	const want = `for p in $(pidof ak_rtsp); do grep -qa akam_klog /proc/$p/cmdline || kill $p; done`
	if restartVideoCmd != want {
		t.Fatalf("restartVideoCmd = %q", restartVideoCmd)
	}
	if forbiddenRemote(restartVideoCmd) != "" {
		t.Error("the audited command is refused by the guard")
	}
	if strings.Contains(strings.ToLower(restartVideoCmd), "watchdog") || strings.Contains(restartVideoCmd, "killall") {
		t.Error("the audited command names the watchdog or killall")
	}
	// the exception is by exact string: any variation is refused
	for _, v := range []string{
		want + "; reboot", want + " 1", "x; " + want, want + "; kill 1", strings.Replace(want, "ak_rtsp", "watchdog", 1),
		strings.Replace(want, "ak_rtsp", "ak_rtsp watchdog", 1), "kill $(pidof ak_rtsp)", "killall ak_rtsp", " " + want,
	} {
		if forbiddenRemote(v) == "" {
			t.Errorf("the guard lets %q through", v)
		}
	}
	// no other constructor builds a command with kill
	kill := regexp.MustCompile(`\bkill`)
	for _, c := range allRemoteCommands() {
		if kill.MatchString(c) {
			t.Errorf("%q contains kill", c)
		}
	}
}
