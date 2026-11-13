package cli

import (
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
)

// The guard is an allow-list: harmless arguments that merely contain a scary word are fine, and
// every bypass of the old deny-list (quoting, escapes, other tools, eval) is refused.
func TestGuardIsAnAllowListNotADenyList(t *testing.T) {
	for _, ok := range []string{
		remoteHostnameSet("watchdog"), remoteHostnameSet("date"), remoteHostnameSet("halt"), remoteHostnameSet("kill-1"),
		remoteNtp("set", "halt.lan"), remoteNtp("set", "pool.ntp.org", "192.0.2.1", "2001:db8::1"),
		remoteMqtt("set", "kill.lan", "1883", "reboot"), remoteMqtt("set", "broker.lan", "1883", "-"),
		remoteRtspAuth("set-creds"), remoteDebug("on", "60"), remoteOnvif("enable", "80"),
		remoteDateRead(), remoteDateSet(time.Unix(1893456000, 0)), restartVideoCmd,
		remoteMtdList, remoteKernelInfo, remoteHwcheckPut, remoteHwcheckRun, remotePreflightPut, remotePreflightRun, remoteOnvifFlag,
		remoteAuthkeysPut, remoteAuthkeysAdd, remoteAuthkeysRemove, remoteAuthkeysCard,
		remoteTestX(hostnameCmd), remoteTrue(), remoteRegister(), remoteReadMtd(3),
	} {
		if why := forbiddenRemote(ok); why != "" {
			t.Errorf("%q refused: %s", ok, why)
		}
	}
	for _, bad := range []string{
		"re''boot", `r\eboot`, `"reboot"`, "rm -rf /", "rm -rf /etc/config", "mtd erase /dev/mtd3", "flash_erase /dev/mtd3 0 0",
		"eval reboot", "eval $(echo cmVib290 | base64 -d)", "sh -c reboot", "busybox reboot", "/sbin/reboot", "$(reboot)", "`reboot`",
		"echo b > /proc/sysrq-trigger", "cat /dev/zero > /dev/mtd7", "ls", "cat /etc/passwd", "cat /proc/mtd; reboot",
		"uname -r; uname -v; reboot", "true && reboot", "true\nreboot",
		hostnameCmd + " set salon; reboot", hostnameCmd + " set $(reboot)", hostnameCmd + " set `x`", hostnameCmd + " set a'b",
		hostnameCmd + " set a\\b", hostnameCmd + " set a|b", hostnameCmd + " set a>b", hostnameCmd + " set a&b", hostnameCmd + " set a\nb",
		hostnameCmd + "  set salon", hostnameCmd + " set salon ", hostnameCmd + " set a\tb", hostnameCmd + " set é",
		ntpCmd + " set pool.ntp.org;reboot", mqttCmd + " set a b\x00", "/usr/sbin/akam_other.sh status", "/usr/sbin/akam_ntp.sh2 status",
		"/tmp/usr/sbin/akam_ntp.sh status", "x/usr/sbin/akam_ntp.sh status", "date -s x", "date", " date +%s",
		restartVideoCmd + "; reboot", remoteHwcheckRun + " x", "sh /tmp/akam_hwcheck.sh; reboot",
		"cat > /tmp/x", "dd if=/dev/mtdblock3 bs=64k of=/tmp/x",
		ctl.NcCommand + "; reboot", ctl.NcCommand + " ", "nc -w 2 127.0.0.1 22", "nc -w 2 192.0.2.1 8091", "nc -w 9 127.0.0.1 8091", "nc 127.0.0.1 8091",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
}

// Every command the program builds, and every fixed string of its call sites, passes the guard.
func TestEveryBuiltCommandPassesTheAllowList(t *testing.T) {
	for _, c := range allRemoteCommands() {
		if forbiddenRemote(c) != "" || strings.ContainsAny(c, "\r") {
			t.Errorf("%q refused", c)
		}
	}
}
