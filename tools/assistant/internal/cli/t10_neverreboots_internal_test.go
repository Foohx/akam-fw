package cli

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// rebootWords are the commands that restart, power off or halt a machine, or reach the hardware
// watchdog or the magic SysRq key.
var rebootWords = regexp.MustCompile(`(?i)(^|[\s;&|(` + "`" + `'"$/])(reboot|poweroff|halt|shutdown|init\s+[06]|telinit|watchdog|sysrq)([\s;&|)` + "`" + `'"-]|$)`)

// TestNeverReboots (specs/installer.md, section 12): the assistant never reboots or power-cycles a camera. (1) Every remote
// command it can build or send, and every fixed command string, passes the allow-list of remote.go and
// holds no reboot word; the one process it kills is the video service, by its exact command. (2) No
// string literal of the code that talks to the camera starts a reboot, a halt, a watchdog or SysRq
// command, so a new command string cannot slip in unnoticed (messages that tell the user to power-cycle
// by hand are prose, not commands).
func TestNeverReboots(t *testing.T) {
	cmds := append(allRemoteCommands(), restartVideoCmd, remoteMtdList, remoteKernelInfo, remoteHwcheckPut, remoteHwcheckRun,
		remotePreflightPut, remotePreflightRun, remoteOnvifFlag, remoteAuthkeysPut, remoteAuthkeysAdd, remoteAuthkeysRemove, remoteAuthkeysCard)
	for _, c := range cmds {
		if why := forbiddenRemote(c); why != "" {
			t.Errorf("%q is refused by the allow-list: %s", c, why)
		}
		if rebootWords.MatchString(c) {
			t.Errorf("%q holds a reboot word", c)
		}
	}
	for _, c := range []string{"reboot", "/sbin/reboot", "poweroff", "halt", "shutdown -h now", "init 6", "busybox reboot", "echo b > /proc/sysrq-trigger",
		"cat /dev/watchdog", restartVideoCmd + "; reboot"} {
		if forbiddenRemote(c) == "" {
			t.Errorf("the allow-list lets %q through", c)
		}
	}
	// (2) the source of everything that sends a command to the camera
	var files []string
	for _, pat := range []string{"*.go", "../camera/ctl/*.go", "../camera/ssh/*.go", "../camera/telnet/*.go"} {
		m, _ := filepath.Glob(pat)
		files = append(files, m...)
	}
	if len(files) < 20 {
		t.Fatalf("source files found: %d", len(files))
	}
	lit := regexp.MustCompile("\"((?:[^\"\\\\\\n]|\\\\.)*)\"|`([^`]*)`")
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") {
			continue
		}
		b, _ := os.ReadFile(f)
		for n, line := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(strings.TrimSpace(line), "//") {
				continue
			}
			for _, m := range lit.FindAllStringSubmatch(line, -1) {
				s := m[1] + m[2]
				// a command starts with the word, or chains it after ; && | $( or a backquote
				if regexp.MustCompile("^\\s*(reboot|poweroff|halt|shutdown|init\\s+[06]|telinit)(\\s|$)|(;|&&|\\|\\||\\||\\$\\(|`)\\s*(reboot|poweroff|halt|shutdown|telinit)(\\s|;|$)|sysrq|/dev/watchdog").MatchString(s) {
					t.Errorf("%s:%d: string literal %q looks like a reboot or halt command", f, n+1, s)
				}
			}
		}
	}
}
