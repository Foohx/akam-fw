package cli

import (
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// Every command line sent to the camera is built here and nowhere else, so that a test can check
// the whole set against the safety rule: no reboot, poweroff, halt, watchdog or flash write. The only
// code that may write the flash is internal/flash (reached through flashSSH in flashcmds.go): it does not
// use this guard, which therefore refuses the reflash commands on every other path.

const (
	hostnameCmd = "/usr/sbin/akam_hostname.sh"
	rtspAuthCmd = "/usr/sbin/akam_rtsp_auth.sh"
	debugCmd    = "/usr/sbin/akam_debug.sh"
	onvifCmd    = "/usr/sbin/akam_onvif.sh"
	ntpCmd      = "/usr/sbin/akam_ntp.sh"
	mqttCmd     = "/usr/sbin/akam_mqtt.sh"
)

// restartVideoCmd is the ONE command that kills a process: it stops ak_rtsp (the video service), which its
// wrapper starts again by itself. pidof also lists the kernel log filter (same binary, renamed akam_klog in
// its cmdline), so the loop skips it by cmdline; nothing else, and never the watchdog, is touched. It is one
// of the exact strings the guard (forbiddenRemote) allows, and it is only sent by
// restartVideo below, after a yes/no that defaults to No (spec 11, Q8).
const restartVideoCmd = `for p in $(pidof ak_rtsp); do grep -qa akam_klog /proc/$p/cmdline || kill $p; done`

// restartVideo asks (default No) and, on yes, restarts the video service. It reports whether it did.
func (a *App) restartVideo(c *camssh.Client) bool {
	ok, _ := a.UI.AskYN("Restart the video service now? The stream stops for a few seconds and starts again by itself.")
	if !ok {
		return false
	}
	// the exit status is not an error: nothing to kill means the service was not running
	_, _ = a.run(c, restartVideoCmd, nil, `kill $(pidof ak_rtsp)`)
	return true
}

// remoteNtp is the time-sync helper: action is status, off or set (then the validated servers).
func remoteNtp(action string, servers ...string) string {
	return strings.TrimSpace(ntpCmd + " " + action + " " + strings.Join(servers, " "))
}

// remoteDateRead reads the camera clock (seconds since 1970).
func remoteDateRead() string { return "date +%s" }

// remoteDateSet sets the camera clock to t (UTC). It uses the "YYYY-MM-DD hh:mm:ss" form every busybox
// `date -s` accepts (the "@epoch" form is only in newer busybox). It is the only form of `date` that may
// set the clock: forbiddenRemote refuses any other `date` that is not the read above.
func remoteDateSet(t time.Time) string {
	return "date -u -s '" + t.UTC().Format("2006-01-02 15:04:05") + "'"
}

// remoteTrue is cam_ssh_ok.
func remoteTrue() string { return "true" }

// remoteTestX is the capability probe of the hostname helper.
func remoteTestX(path string) string { return "test -x " + path }

func remoteHostnameSet(name string) string { return hostnameCmd + " set " + name }

func remoteRtspAuth(args ...string) string { return rtspAuthCmd + " " + strings.Join(args, " ") }

func remoteDebug(action string, minutes string) string {
	s := debugCmd + " " + action
	if minutes != "" {
		s += " " + minutes
	}
	return s
}

func remoteOnvif(args ...string) string { return onvifCmd + " " + strings.Join(args, " ") }

// remoteMqtt is the MQTT helper. The broker password is never an argument: it goes on stdin (see mqttSet).
func remoteMqtt(args ...string) string { return mqttCmd + " " + strings.Join(args, " ") }

// remoteRegister reads the camera's name and the md5 of its APP partition (read-only).
func remoteRegister() string {
	return `echo "REG_HOSTNAME=$(hostname)"; echo "REG_MTD7_MD5=$(dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum | cut -d" " -f1)"`
}

// allRemoteCommands lists one instance of every constructor (used by the safety test).
func allRemoteCommands() []string {
	return []string{
		remoteTrue(), remoteTestX(hostnameCmd), remoteHostnameSet("salon"),
		remoteRtspAuth("status"), remoteRtspAuth("show"), remoteRtspAuth("set-creds"), remoteRtspAuth("disable"),
		remoteDebug("on", "60"), remoteDebug("off", ""), remoteDebug("status", ""),
		remoteOnvif("enable", "80"), remoteOnvif("disable"), remoteOnvif("status"),
		remoteMqtt("status"), remoteMqtt("set", "192.0.2.10", "1883", "bob"), remoteMqtt("set", "broker.lan", "1883", "-"),
		remoteMqtt("disable"), remoteMqtt("forget"), remoteMqtt("test"),
		remoteRegister(), remoteManifest, remoteReadMtd(0), remoteReadMtd(7), remoteNtp("status"), remoteNtp("off"), remoteNtp("set", "pool.ntp.org", "192.0.2.1"),
		remoteDateRead(), remoteDateSet(time.Unix(1893456000, 0)), ctl.NcCommand,
	}
}

// The fixed command lines that are not built from a helper (every one of them is sent by exactly one
// function; the guard below allows exactly these strings).
const (
	remoteMtdList      = "cat /proc/mtd"
	remoteKernelInfo   = "uname -r; uname -v"
	remoteHwcheckPut   = "cat > /tmp/akam_hwcheck.sh"
	remoteHwcheckRun   = "sh /tmp/akam_hwcheck.sh"
	remotePreflightPut = "cat > /tmp/akam_preflight.sh"
	remotePreflightRun = "sh /tmp/akam_preflight.sh --ensure-wifi"
	remoteOnvifFlag    = "cat " + onvifFlagFile + " 2>/dev/null"
	// remoteManifest reads the manifest of the kit the image was built from (its akam_version)
	remoteManifest = "cat /usr/local/akam/MANIFEST.txt 2>/dev/null"
)

// remoteDateSetRe is the one form of `date` that sets the clock (see remoteDateSet); nothing can be chained.
var remoteDateSetRe = regexp.MustCompile(`^date -u -s '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}'$`)

// remoteReadRe is the ONE read-only form of a flash read that is allowed on its own: dd with an
// input and a block size, never an of=. The partition number is a single digit 0..7.
var remoteReadRe = regexp.MustCompile(`^dd if=/dev/mtdblock[0-7] bs=64k$`)

// remoteReadMtd is the command that streams partition n (0..7) to standard output (read-only). It
// panics on any other n: the number is never built from camera text.
func remoteReadMtd(n int) string {
	if n < 0 || n > 7 {
		panic("remoteReadMtd: partition out of range")
	}
	return fmt.Sprintf("dd if=/dev/mtdblock%d bs=64k", n)
}

// helperRe is the other allowed shape: one of the helper scripts of the image, then arguments made of
// [A-Za-z0-9._@:+-] only, separated by single spaces. No quote, backslash, $, ;, &, |, <, >, newline or
// space inside an argument, so nothing can be chained, substituted or redirected.
var helperRe = func() *regexp.Regexp {
	var alt []string
	for _, h := range []string{hostnameCmd, rtspAuthCmd, debugCmd, onvifCmd, ntpCmd, mqttCmd} {
		alt = append(alt, regexp.QuoteMeta(h))
	}
	return regexp.MustCompile(`\A(?:` + strings.Join(alt, "|") + `)(?: [A-Za-z0-9._@:+-]+)*\z`)
}()

// fixedRemote is the set of exact command lines allowed besides the helpers.
var fixedRemote = map[string]bool{
	restartVideoCmd: true, "true": true, remoteTestX(hostnameCmd): true,
	remoteDateRead(): true, remoteRegister(): true,
	remoteMtdList: true, remoteKernelInfo: true,
	remoteHwcheckPut: true, remoteHwcheckRun: true, remotePreflightPut: true, remotePreflightRun: true, remoteOnvifFlag: true,
	remoteManifest: true,
	remoteAuthkeysPut: true, remoteAuthkeysAdd: true, remoteAuthkeysRemove: true, remoteAuthkeysCard: true,
	ctl.NcCommand: true,
}

// forbiddenRemote is an ALLOW-list: it returns "" only for a command line the assistant itself
// builds (an exact string of fixedRemote, the anchored partition read, the date set form, or a
// helper script with plain arguments) and a reason for everything else. A deny-list cannot work
// here: quoting (re”boot), escapes, eval, other tools (rm, mtd) and any new spelling get past it,
// while harmless words (a host named "watchdog") are refused by it.
func forbiddenRemote(cmd string) string {
	if fixedRemote[cmd] || remoteDateSetRe.MatchString(cmd) || remoteReadRe.MatchString(cmd) || helperRe.MatchString(cmd) {
		return ""
	}
	return fmt.Sprintf("forbidden camera command: %s", cmd)
}
