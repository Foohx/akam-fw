package cli

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
)

// Camera health shown with the status of a camera: the Wi-Fi signal (with the advice a weak one calls
// for) and the clock. Everything here is a read; the only thing that ever changes the camera is the
// clock action, which asks first (default No).

// healthKeys are the read-only metrics of the control port that ride on the options exchange.
var healthKeys = []string{
	"net.wifi_rssi", "net.wifi_rate", "video.bitrate", "video.fps_actual", "video.fps_cfg",
	"video.bitrate_cfg", "video.gop_cfg", "night.state", "sys.time_set",
}

// Target values of the advice for a weak link (the settings that carried a camera at -78 dBm).
const (
	lowBitrateKbps = 1024
	lowGopSeconds  = 1
)

// clockToleranceSec is how far the camera clock may be from this PC before it is called off.
const clockToleranceSec = 120

// minPlausibleEpoch is 2024-01-01 UTC: a camera clock before it was never set (no RTC: the camera
// boots in 2000), the same rule as sys.time_set on the camera.
const minPlausibleEpoch = 1704067200

// pcNow is the time of this computer (a variable for tests).
var pcNow = time.Now

// pcClockNote is the line said instead of the clock offer when the clock of this computer is not
// plausible (a dead battery gives 1970 or 2000): setting the camera from it would make it worse.
func pcClockNote(pc int64) string {
	return "The clock of this computer reads " + time.Unix(pc, 0).UTC().Format("2006-01-02") + " UTC, which cannot be right, so the camera clock is not offered to be set from it: fix the clock of this computer first (or turn time sync on)."
}

// camHealth is what the camera said; -1 or "" is "not answered" (an older image).
type camHealth struct {
	rssi, rate           int    // dBm (negative; 0 unknown), Mb/s
	kbps, fps10          int    // measured: kbit/s, tenths of a frame per second
	fpsCfg, kbpsCfg, gop int    // configured: frames per second, kbit/s, seconds
	night                string // night.state: night | day | ""
	timeSet              string // sys.time_set: 1 | 0 | ""
	camEpoch, pcEpoch    int64  // seconds since 1970 on the camera / on this PC (0: unknown)
	ntp                  string // time sync: on | off | "" (unknown)
	ntpSync              string // last helper log of time sync: ok | failed | "" (none, or unknown)
}

// readCamHealth parses the answers of the control port (healthKeys) into a camHealth.
func readCamHealth(get func(string) string) camHealth {
	n := func(k string) int {
		v, err := strconv.Atoi(get(k))
		if err != nil {
			return -1
		}
		return v
	}
	h := camHealth{rssi: n("net.wifi_rssi"), rate: n("net.wifi_rate"), kbps: n("video.bitrate"), fps10: n("video.fps_actual"),
		fpsCfg: n("video.fps_cfg"), kbpsCfg: n("video.bitrate_cfg"), gop: n("video.gop_cfg"),
		night: get("night.state"), timeSet: get("sys.time_set")}
	if v, err := strconv.Atoi(get("net.wifi_rssi")); err != nil || v >= 0 {
		h.rssi = 0 // an RSSI of 0 dBm or more is never real: it means "unknown"
	} else {
		h.rssi = v
	}
	return h
}

// wifiLevel classifies a signal in dBm. Thresholds: from -60 up the link keeps its top rates; between
// -60 and -70 it still works but has little margin; the 65 Mb/s PHY rate seen at -78 dBm could not carry
// a 1.5 Mb/s 1080p stream over TCP once retries and rate drops ate the margin, so -70 to -75 is "weak"
// (trouble under load) and below -75 is "poor" (trouble expected).
func wifiLevel(rssi int) string {
	switch {
	case rssi == 0:
		return ""
	case rssi >= -60:
		return "good"
	case rssi >= -70:
		return "fair"
	case rssi >= -75:
		return "weak"
	}
	return "poor"
}

// wifiReport is the "Wi-Fi signal" line and, for a weak or poor one, the short advice. It never changes
// anything: the commands it names are for the user to run.
func wifiReport(h camHealth, prog string) []string {
	lvl := wifiLevel(h.rssi)
	if lvl == "" {
		return nil
	}
	line := fmt.Sprintf("Wi-Fi signal: %d dBm (%s)", h.rssi, lvl)
	if h.rate > 0 {
		line += fmt.Sprintf(", link %d Mb/s", h.rate)
	}
	out := []string{line + "."}
	if lvl != "weak" && lvl != "poor" {
		return out
	}
	out = append(out, "A weak signal can make the video stutter: RTSP over UDP loses key frames (the picture breaks up for a moment),")
	out = append(out, "RTSP over TCP lags and the camera drops frames.")
	out = append(out, "Best fix: move the camera or the access point closer, or add a Wi-Fi repeater.")
	var set []string
	if h.kbpsCfg > lowBitrateKbps {
		set = append(set, fmt.Sprintf("bitrate %d", lowBitrateKbps))
	}
	if h.gop > lowGopSeconds {
		set = append(set, fmt.Sprintf("gop %d", lowGopSeconds))
	}
	now := fmt.Sprintf("now: bitrate %s kbit/s, key frame every %s s", numOrQ(h.kbpsCfg), numOrQ(h.gop))
	if len(set) > 0 {
		out = append(out, fmt.Sprintf("Or lower the load (%s): %s video %s   (nothing is changed automatically)", now, prog, strings.Join(set, " ")))
	} else if h.kbpsCfg > 0 || h.gop > 0 {
		out = append(out, fmt.Sprintf("The video settings are already low (%s).", now))
	}
	if h.fps10 > 0 {
		switch {
		case h.night == "night":
			out = append(out, fmt.Sprintf("Measured %.1f fps: at night this is normal (long exposure), it is not the Wi-Fi.", float64(h.fps10)/10))
		case h.fpsCfg > 0 && h.fps10 < h.fpsCfg*10-10:
			out = append(out, fmt.Sprintf("Measured %.1f fps for %d configured: frames are being dropped, which fits a weak link.", float64(h.fps10)/10, h.fpsCfg))
		}
	}
	return out
}

func numOrQ(v int) string {
	if v < 0 {
		return "?"
	}
	return strconv.Itoa(v)
}

// clockReport is the clock line, and whether it is worth offering to set it from this PC.
func clockReport(h camHealth) (lines []string, offer bool) {
	lines, offer = clockReportRaw(h)
	if !offer {
		return lines, false
	}
	pc := h.pcEpoch
	if pc == 0 {
		pc = pcNow().Unix()
	}
	if pc < minPlausibleEpoch { // this computer cannot be the reference
		return append(lines, pcClockNote(pc)), false
	}
	// Time sync is on and has worked, and the camera clock is plausibly set: the camera is the better reference. A
	// difference then most likely means this computer is wrong, so never offer to overwrite the camera.
	if h.ntp == "on" && !clockUnset(h) && h.ntpSync != "ok" {
		// Time sync is on but nothing says it has worked: the camera clock is not known to be right, so
		// the offer stays (default No).
		return append(lines, "Time sync is on but has not succeeded yet (see "+ntpStatusHint+"), so the camera clock is not known to be right."), true
	}
	if h.ntp == "on" && !clockUnset(h) {
		return append(lines,
			"The camera keeps its time with NTP (time sync is on), so its clock is probably right and the clock or time zone of this computer may be wrong.",
			fmt.Sprintf("Camera %s UTC, this computer %s UTC (difference %s). The camera clock is not offered to be set from this computer: check the clock and time zone of this computer.",
				time.Unix(h.camEpoch, 0).UTC().Format("2006-01-02 15:04:05"), time.Unix(pc, 0).UTC().Format("2006-01-02 15:04:05"), humanSeconds(absInt64(h.camEpoch-pc)))), false
	}
	return lines, true
}

// ntpStatusHint is where the reason is shown.
const ntpStatusHint = "akam ntp status"

// parseNtpStatus reads the output of `akam_ntp.sh status`: "on", "off" or "" for the first line, and
// for the last helper log line "ok" (sync ok), "failed" (sync failed) or "" (none, or anything else).
func parseNtpStatus(out string) (ntp, sync string) {
	lines := strings.Split(strings.TrimSpace(out), "\n")
	switch first := strings.TrimSpace(lines[0]); {
	case strings.HasPrefix(first, "time sync: on"):
		ntp = "on"
	case strings.HasPrefix(first, "time sync: off"):
		ntp = "off"
	}
	for _, l := range lines {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "last log:"); ok {
			switch {
			case strings.Contains(v, "sync ok"):
				sync = "ok"
			case strings.Contains(v, "sync failed"):
				sync = "failed"
			}
		}
	}
	return ntp, sync
}

func absInt64(v int64) int64 {
	if v < 0 {
		return -v
	}
	return v
}

// clockUnset is true when the camera says nothing set its clock, or it reads before minPlausibleEpoch.
func clockUnset(h camHealth) bool {
	return h.timeSet == "0" || (h.camEpoch > 0 && h.camEpoch < minPlausibleEpoch)
}

func clockReportRaw(h camHealth) (lines []string, offer bool) {
	if clockUnset(h) {
		l := "Clock: not set (the camera has no battery clock and starts in 2000 until something sets it)"
		if h.camEpoch > 0 {
			l += "; it reads " + time.Unix(h.camEpoch, 0).UTC().Format("2006-01-02 15:04:05") + " UTC"
		}
		return []string{l + "."}, true
	}
	if h.camEpoch > 0 && h.pcEpoch > 0 {
		off := h.camEpoch - h.pcEpoch
		abs := off
		if abs < 0 {
			abs = -abs
		}
		if abs > clockToleranceSec {
			return []string{fmt.Sprintf("Clock: off by %s compared with this computer (camera %s UTC).", humanSeconds(abs), time.Unix(h.camEpoch, 0).UTC().Format("2006-01-02 15:04:05"))}, true
		}
		return []string{fmt.Sprintf("Clock: set, %+d s compared with this computer.", off)}, false
	}
	if h.timeSet == "1" {
		return []string{"Clock: set."}, false
	}
	return nil, false
}

func humanSeconds(s int64) string {
	switch {
	case s >= 86400:
		return fmt.Sprintf("%d day(s)", s/86400)
	case s >= 3600:
		return fmt.Sprintf("%d hour(s)", s/3600)
	case s >= 120:
		return fmt.Sprintf("%d minute(s)", s/60)
	}
	return fmt.Sprintf("%d s", s)
}

// readCamClock reads the camera clock over SSH (date +%s) into h; the PC time is taken right after, so
// the difference includes only the round trip.
func (a *App) readCamClock(c *camssh.Client, h *camHealth) {
	res, err := a.run(c, remoteDateRead(), nil, "")
	if err != nil || res.Status != 0 {
		return
	}
	v, err := strconv.ParseInt(strings.TrimSpace(string(res.Stdout)), 10, 64)
	if err != nil || v <= 0 {
		return
	}
	h.camEpoch, h.pcEpoch = v, time.Now().Unix()
}

// camCmd is the program with the chosen camera, so a command in a hint is safe to paste with several cameras.
func (a *App) camCmd() string { return Prog + " --camera " + a.Cams.Name }

// clockLostNote is the end of the clock offer: a time set by hand is lost at reboot unless time sync is on.
func clockLostNote(h camHealth) string {
	if h.ntp == "on" && h.ntpSync == "ok" {
		return "Time sync is on and corrects the clock shortly, also after a reboot."
	}
	if h.ntp == "on" {
		return "Time sync is on but has not succeeded yet, so it may not correct the clock."
	}
	return "It is lost at the next reboot unless time sync (NTP) is on."
}

// showHealth prints the Wi-Fi and clock lines of a snapshot.
func (a *App) showHealth(h camHealth) {
	a.noteHealth(h)
	for _, l := range wifiReport(h, a.camCmd()) {
		a.UI.Say("  " + l)
	}
	cl, _ := clockReport(h)
	for _, l := range cl {
		a.UI.Say("  " + l)
	}
}

// clockOffer offers to set the camera clock from this PC when it is unset or off (default No).
func (a *App) clockOffer(c *camssh.Client, h camHealth) {
	_, offer := clockReport(h)
	if !offer || !a.UI.Interactive() {
		return
	}
	a.UI.Say("  The clock is used by ONVIF clients (their login checks the time) and by video time stamps.")
	a.clockAsk(c, h)
}

// clockAsk is the question "Set the camera clock from this computer now?" (default No) and what follows
// its answer. The caller decides whether it may be asked: clockOffer on the plain path, the t key of
// the options form (specs/installer.md), never unprompted there.
func (a *App) clockAsk(c *camssh.Client, h camHealth) {
	if ok, _ := a.UI.AskYN("  Set the camera clock from this computer now? " + clockLostNote(h)); !ok {
		if h.ntp != "on" {
			a.UI.Say(fmt.Sprintf("  Clock left as it is. Time sync keeps it right across reboots: %s ntp set SERVER", a.camCmd()))
		} else {
			a.UI.Say("  Clock left as it is; time sync should correct it shortly.")
		}
		return
	}
	a.setCamClock(c, h)
}

// setCamClock sets the camera clock to the time of this PC (UTC, so the time zone does not matter).
func (a *App) setCamClock(c *camssh.Client, h camHealth) {
	now := pcNow().UTC()
	if now.Unix() < minPlausibleEpoch {
		a.UI.Warn(pcClockNote(now.Unix()))
		return
	}
	res, err := a.run(c, remoteDateSet(now), nil, "")
	if err != nil || res.Status != 0 {
		a.UI.Warn("the camera did not accept the new time (the clock is unchanged).")
		return
	}
	a.UI.Good("  Camera clock set to " + now.Format("2006-01-02 15:04:05") + " UTC. " + clockLostNote(h))
}

// ntpOffer, at the end of an install or an update, offers time sync when it is off (default No): the
// camera has no clock of its own and starts in 2000.
func (a *App) ntpOffer(ip string) {
	a.UI.Blank()
	a.UI.Say("  Optional: time sync. The camera has no battery clock and starts in 2000; ONVIF apps need the right time.")
	a.UI.Say("  Off by default. Turning it on makes the camera ask time servers (a server you choose: Akam sets none for you; for example your router or another server on your network) over UDP 123, outgoing only.")
	a.UI.Say(fmt.Sprintf("  You can change your mind at any time: %s ntp set SERVER | off | status", a.camCmd()))
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask: Turn on time sync? [y/N] (default: No)")
		a.UI.Say("  WOULD then ask for a time server (no default) and run over SSH on the camera: " + remoteNtp("set", "SERVER"))
		return
	}
	if !a.UI.Interactive() {
		a.UI.Say(fmt.Sprintf("  No terminal: time sync stays off. Run %s ntp set SERVER later.", a.camCmd()))
		return
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return
	}
	defer c.Close()
	if res, err := a.run(c, remoteNtp("status"), nil, ""); err == nil && res.Status == 0 &&
		strings.HasPrefix(strings.TrimSpace(string(res.Stdout)), "time sync: on") {
		a.UI.Say(fmt.Sprintf("  Already on. Turn it off with %s ntp off.", a.camCmd()))
		return
	}
	if ok, _ := a.UI.AskYN("  Turn on time sync?"); !ok {
		a.UI.Say(fmt.Sprintf("  Time sync left off. Run %s ntp set SERVER whenever you want it.", a.camCmd()))
		return
	}
	a.UI.Say("  Akam does not pick a public server for you (the NTP Pool asks products not to hard-code its default zone): type the one you want.")
	srv, err := a.UI.Prompt("  Time server (host name or IPv4 address; Enter to leave time sync off):", "")
	srv = strings.TrimSpace(srv)
	if err != nil || srv == "" {
		a.UI.Say(fmt.Sprintf("  No server given: time sync stays off. Run %s ntp set SERVER later.", a.camCmd()))
		return
	}
	if !camopt.ValidNTPServer(srv) {
		a.UI.Say(fmt.Sprintf("  '%s' is not a host name or an IPv4 address: time sync stays off. Run %s ntp set SERVER later.", srv, a.camCmd()))
		return
	}
	a.reportFlowError(a.cmdNtp([]string{"set", srv}))
}

// readHealthOf reads the metrics of the control port (one exchange) and, when withClock, the clock.
func (a *App) readHealthOf(c *camssh.Client, withClock bool) camHealth {
	lines := make([]string, 0, len(healthKeys))
	for _, k := range healthKeys {
		lines = append(lines, "GET "+k)
	}
	raw := a.ctlExchange(c, lines...)
	h := readCamHealth(func(k string) string { return ctl.Get(k, raw) })
	if withClock {
		a.readCamClock(c, &h)
	}
	return h
}

// readCamNtp reads the time sync state (on/off and whether the last sync worked) into h, so that the
// clock report is the same wherever it is shown.
func (a *App) readCamNtp(c *camssh.Client, h *camHealth) {
	if res, err := a.run(c, remoteNtp("status"), nil, ""); err == nil && res.Status == 0 {
		h.ntp, h.ntpSync = parseNtpStatus(string(res.Stdout))
	}
}
