package cli

import (
	"fmt"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The option screens (spec 11, 3.4) on top of internal/camopt: one screen per group, every value read
// in one exchange of the control port, values edited locally and validated by the same validators as
// the command line, nothing sent before "apply". Options that go through a helper script of the camera
// (video password, ONVIF, NTP, host name) call the existing commands, which keep their own yes/no.

// optSnap is what the camera said when the screen was opened.
type optSnap struct {
	vals     map[string]camopt.OptionState
	fw, host string
	onvifOn  bool
	rtsp     string // enabled | disabled | invalid | unknown
	ntp      string // on | off | unknown
	health   camHealth
}

var optGroups = []string{"LED and night", "Picture", "Video", "Audio", "Network", "Security"}

// optionGroupOptions are the options a group lets the user edit through the control port.
func optionGroupOptions(group string) []camopt.Option {
	var out []camopt.Option
	for _, o := range camopt.Catalog() {
		if o.Group == group && o.Key != "" && !o.ReadOnly {
			out = append(out, o)
		}
	}
	return out
}

// readOptions reads every value of every group: one control-port exchange (with the firmware and host
// name riding on it) and three small reads of the helper state, all over one SSH session.
func (a *App) readOptions(c *camssh.Client) optSnap {
	snap := optSnap{vals: map[string]camopt.OptionState{}}
	var raw []string
	ex := func(lines ...string) []string {
		lines = append(lines, "GET fw.version", "GET sys.hostname")
		for _, k := range healthKeys {
			lines = append(lines, "GET "+k)
		}
		raw = a.ctlExchange(c, lines...)
		return raw
	}
	for _, st := range camopt.Read(ex) {
		snap.vals[st.ID] = st
	}
	for _, l := range raw {
		for _, k := range []string{"fw.version", "sys.hostname"} {
			if v, ok := strings.CutPrefix(l, k+"="); ok {
				if i := strings.IndexByte(v, ' '); i >= 0 {
					v = v[:i]
				}
				if !camopt.SafeValue(v) {
					v = ""
				}
				if k == "fw.version" {
					snap.fw = v
				} else {
					snap.host = v
				}
			}
		}
	}
	snap.health = readCamHealth(func(k string) string { return ctl.Get(k, raw) })
	a.readCamClock(c, &snap.health)
	snap.onvifOn, _ = a.onvifFlag(c)
	snap.rtsp, _ = a.rtspStatus(c)
	snap.ntp = "unknown"
	if res, err := a.run(c, remoteNtp("status"), nil, ""); err == nil && res.Status == 0 {
		switch first := strings.TrimSpace(strings.SplitN(string(res.Stdout), "\n", 2)[0]); {
		case strings.HasPrefix(first, "time sync: on"):
			snap.ntp = "on"
		case strings.HasPrefix(first, "time sync: off"):
			snap.ntp = "off"
		}
	}
	return snap
}

// valueText is how one value reads on a screen: "-" for an option this image does not have.
func valueText(st camopt.OptionState) string {
	if st.Unavailable {
		return "n/a"
	}
	switch st.ID {
	case "audio.out_enable", "audio.talk_enable", "audio.mic_enable":
		if st.Value == "1" {
			return "on"
		}
		return "off"
	}
	return st.Value
}

func (s optSnap) v(id string) string {
	st, ok := s.vals[id]
	if !ok {
		return "n/a"
	}
	t := valueText(st)
	if st.Persisted == camopt.PersistRAM && !st.Unavailable {
		t += "*"
	}
	return t
}

func onOff(b bool) string {
	if b {
		return "on"
	}
	return "off"
}

// groupSummary is the one line of a group on the main option screen.
func groupSummary(group string, s optSnap) string {
	switch group {
	case "LED and night":
		return fmt.Sprintf("front LED: %s   IR LEDs: %s   night: %s", s.v("led.status"), s.v("night.ir_led"), s.v("night.mode"))
	case "Picture":
		return fmt.Sprintf("brightness %s  contrast %s  saturation %s  sharpness %s", s.v("isp.brightness"), s.v("isp.contrast"), s.v("isp.saturation"), s.v("isp.sharpness"))
	case "Video":
		return fmt.Sprintf("bitrate %s kbit/s   %s fps   key frame every %s s", s.v("video.bitrate_cfg"), s.v("video.fps_cfg"), s.v("video.gop_cfg"))
	case "Audio":
		return fmt.Sprintf("speaker %s (%s)   talk: %s   microphone: %s", s.v("audio.out_volume"), s.v("audio.out_enable"), s.v("audio.talk_enable"), s.v("audio.mic_enable"))
	case "Network":
		ssid := "n/a"
		if st, ok := s.vals["net.ssid"]; ok && !st.Unavailable {
			ssid = ui.Sanitize(st.Value)
		}
		name := s.host
		if name == "" {
			name = "n/a"
		}
		return fmt.Sprintf("ONVIF: %s   NTP: %s   name: %s   Wi-Fi: %s (read-only)", onOff(s.onvifOn), s.ntp, name, ssid)
	case "Security":
		t := map[string]string{"enabled": "on", "disabled": "off"}[s.rtsp]
		if t == "" {
			t = "unknown"
		}
		return "video password: " + t
	}
	return ""
}

// flowOptions is the group list; it opens one SSH session for the whole visit.
func (a *App) flowOptions() error {
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.banner("Camera options - " + a.Cams.Name)
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH on the camera: printf '<GET lines>' | nc -w 2 127.0.0.1 %d (every option in one go),", 8091))
		a.UI.Say("  show the groups, edit values locally, and only on 'apply' send the SET lines (read-only until then).")
		a.UI.Say("  WOULD ask: Restart the video service now? [y/N] (default No) after a video change.")
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	offerClock := true
	for {
		snap := a.readOptions(c)
		a.UI.Blank()
		a.UI.Bold(fmt.Sprintf("Camera options - %s (%s)%s", a.Cams.Name, ip, fwSuffix(snap.fw)))
		a.showHealth(snap.health)
		if offerClock {
			a.clockOffer(c, snap.health)
			offerClock = false
		}
		items := make([]string, 0, len(optGroups)+2)
		for _, g := range optGroups {
			items = append(items, fmt.Sprintf("%-14s %s", g, groupSummary(g, snap)))
		}
		items = append(items, "Reload", "Back")
		a.UI.Say("  * not saved: back to the default at the next reboot")
		i, err := a.UI.Pick("Which group?", items)
		if err != nil || i == len(optGroups)+1 {
			return nil
		}
		if i == len(optGroups) {
			offerClock = true
			continue
		}
		switch g := optGroups[i]; g {
		case "Network":
			a.networkScreen(c, snap)
		case "Security":
			a.securityScreen(snap)
		default:
			a.groupScreen(c, g, snap)
		}
	}
}

func fwSuffix(fw string) string {
	if fw == "" {
		return ""
	}
	return "   Akam " + ui.Sanitize(fw)
}

// optionLine is one editable option: the value, its range and when it applies.
func optionLine(o camopt.Option, st camopt.OptionState, pending string) string {
	v := valueText(st)
	if st.Unavailable {
		return fmt.Sprintf("%-26s not available on this image (update first)", o.Label)
	}
	rng := ""
	switch o.Kind {
	case camopt.Int:
		rng = fmt.Sprintf("%d to %d", o.Min, o.Max)
	case camopt.Enum:
		rng = strings.Join(enumLabels(o), ", ")
	}
	line := fmt.Sprintf("%-26s %-6s [%s]  %s", o.Label, v, rng, camopt.Describe(o))
	if pending != "" && pending != st.Value {
		line += fmt.Sprintf("   -> %s (pending)", pending)
	}
	return line
}

// enumLabels are the choices of an enum option as the user reads them (0/1 enables are off/on).
func enumLabels(o camopt.Option) []string {
	if len(o.Values) == 2 && o.Values[0] == "0" && o.Values[1] == "1" {
		return []string{"off", "on"}
	}
	return o.Values
}

func enumValue(o camopt.Option, label string) string {
	for i, l := range enumLabels(o) {
		if l == label {
			return o.Values[i]
		}
	}
	return label
}

// groupScreen edits the options of one control-port group, then applies them together.
func (a *App) groupScreen(c *camssh.Client, group string, snap optSnap) {
	opts := optionGroupOptions(group)
	pending := map[string]string{}
	for {
		a.UI.Blank()
		a.UI.Bold(fmt.Sprintf("%s - %s", group, a.Cams.Name))
		items := make([]string, 0, len(opts)+3)
		for _, o := range opts {
			items = append(items, optionLine(o, snap.vals[o.ID], pending[o.ID]))
		}
		n := len(pending)
		items = append(items, fmt.Sprintf("Apply %d pending change(s)", n), "Discard the pending changes", "Back")
		i, err := a.UI.Pick(group+": which option?", items)
		if err != nil || i == len(opts)+2 {
			if n > 0 {
				a.UI.Say("  Nothing was sent: the pending changes are discarded.")
			}
			return
		}
		switch {
		case i < len(opts):
			o := opts[i]
			if snap.vals[o.ID].Unavailable {
				a.UI.Say("  This camera's image does not have that option: update it first (Update this camera).")
				continue
			}
			if v, ok := a.askOptionValue(o, snap.vals[o.ID]); ok {
				if v == snap.vals[o.ID].Value {
					delete(pending, o.ID)
				} else {
					pending[o.ID] = v
				}
			}
		case i == len(opts):
			if n == 0 {
				a.UI.Say("  Nothing to apply.")
				continue
			}
			var changes []camopt.Change
			for _, o := range opts {
				if v, ok := pending[o.ID]; ok {
					changes = append(changes, camopt.Change{ID: o.ID, Value: v})
				}
			}
			a.applyChanges(c, changes)
			snap = a.readOptions(c)
			pending = map[string]string{}
		default:
			pending = map[string]string{}
			a.UI.Say("  Discarded.")
		}
	}
}

// askOptionValue asks the new value of one option and validates it before it is kept.
func (a *App) askOptionValue(o camopt.Option, cur camopt.OptionState) (string, bool) {
	switch o.Kind {
	case camopt.Enum:
		labels := enumLabels(o)
		items := append(append([]string(nil), labels...), "Cancel")
		i, err := a.UI.Pick(fmt.Sprintf("%s (now %s):", o.Label, valueText(cur)), items)
		if err != nil || i >= len(labels) {
			return "", false
		}
		v, err := camopt.Validate(o.ID, enumValue(o, labels[i]))
		return v, err == nil
	case camopt.Int:
		for {
			s, err := a.UI.Prompt(fmt.Sprintf("  %s (%d to %d, now %s; empty cancels):", o.Label, o.Min, o.Max, cur.Value), "")
			if err != nil || s == "" {
				return "", false
			}
			v, verr := camopt.Validate(o.ID, strings.TrimSpace(s))
			if verr != nil {
				a.UI.Say("  " + verr.Error() + ".")
				continue
			}
			return v, true
		}
	}
	return "", false
}

// applyChanges sends the pending changes in one exchange, tells what each did and when it applies,
// and offers the video restart (the single audited restartVideo, default No).
func (a *App) applyChanges(c *camssh.Client, changes []camopt.Change) {
	for _, ch := range changes {
		o, _ := camopt.Find(ch.ID)
		a.UI.Say(fmt.Sprintf("  WILL send: SET %s %s", o.Key, ch.Value))
	}
	res, err := camopt.Apply(a.exchanger(c), changes)
	if err != nil {
		a.UI.Err(err.Error())
		return
	}
	restart, power := false, false
	for _, r := range res {
		o, _ := camopt.Find(r.ID)
		switch {
		case r.OK:
			if r.NotSaved {
				a.UI.Warn(fmt.Sprintf("%s is %s now, but the camera could not save it: it will be lost at the next reboot.", o.Label, r.Value))
			} else {
				a.UI.Good(fmt.Sprintf("  %s: %s (%s).", o.Label, r.Value, camopt.Describe(o)))
			}
			if r.Restart || o.Applies == camopt.VideoRestart {
				restart = true
			}
			if o.Applies == camopt.NextPowerCycle {
				power = true
			}
		case r.Answered:
			a.UI.Err(fmt.Sprintf("the camera refused %s %s: %s (this image may not have that setting; update it)", o.Key, r.Value, ui.Sanitize(r.Err)))
		default:
			a.UI.Err(fmt.Sprintf("the camera did not confirm %s %s: its control port may be busy; try again.", o.Key, r.Value))
		}
	}
	if power {
		a.UI.Say("  A change above applies at the next power cycle: unplug the USB-C power, wait 5 seconds, plug it back (the assistant never does it for you).")
	}
	if restart {
		a.UI.Say("  Saved on the camera. It applies when the video service restarts.")
		if a.restartVideo(c) {
			a.UI.Good(fmt.Sprintf("  The video service was asked to restart; it is back in about 20 seconds (%s video show).", Prog))
		} else {
			a.UI.Say("  Not restarted: the new values apply at the next restart of the video service or the next reboot.")
		}
	}
}

// networkScreen: ONVIF, NTP, host name; Wi-Fi is shown and cannot be changed here (Q7).
func (a *App) networkScreen(c *camssh.Client, snap optSnap) {
	ssid := "n/a"
	if st, ok := snap.vals["net.ssid"]; ok && !st.Unavailable {
		ssid = ui.Sanitize(st.Value)
	}
	items := []string{
		fmt.Sprintf("ONVIF: %s  (turn it %s)", onOff(snap.onvifOn), onOff(!snap.onvifOn)),
		fmt.Sprintf("NTP: %s  (set the time servers, or turn it off)", snap.ntp),
		fmt.Sprintf("Host name: %s  (applies at the next power cycle)", orDash(snap.host)),
		fmt.Sprintf("Wi-Fi: %s  (read-only: changing it needs the flash card)", ssid),
		"Back",
	}
	i, err := a.UI.Pick("Network:", items)
	if err != nil {
		return
	}
	var e error
	switch i {
	case 0:
		if snap.onvifOn {
			e = a.cmdOnvif([]string{"disable"})
		} else {
			e = a.cmdOnvif([]string{"enable"})
		}
	case 1:
		s, perr := a.UI.Prompt("  NTP servers (1 to 3 separated by spaces, or 'off'; empty cancels):", "")
		if perr != nil || strings.TrimSpace(s) == "" {
			return
		}
		f := strings.Fields(s)
		if len(f) == 1 && f[0] == "off" {
			e = a.cmdNtp([]string{"off"})
		} else if verr := camopt.ValidNTPServers(f); verr != nil {
			a.UI.Say("  " + verr.Error() + ".")
		} else {
			e = a.cmdNtp(append([]string{"set"}, f...))
		}
	case 2:
		s, perr := a.UI.Prompt(fmt.Sprintf("  Host name [%s]:", a.Cams.Name), a.Cams.Name)
		if perr != nil {
			return
		}
		e = a.cmdHostname(s)
	case 3:
		a.UI.Say("  The Wi-Fi name and password are written to the flash card when the camera is installed; a wrong value would lock the camera out of your network, so they cannot be changed from here.")
	}
	a.reportFlowError(e)
}

func orDash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}

// securityScreen: the video stream password.
func (a *App) securityScreen(snap optSnap) {
	cur := map[string]string{"enabled": "on", "disabled": "off"}[snap.rtsp]
	if cur == "" {
		cur = "unknown"
	}
	items := []string{"Set or change the video password", "Remove the video password (asks first)", "Back"}
	i, err := a.UI.Pick("Security - video password is "+cur+":", items)
	if err != nil || i == 2 {
		return
	}
	if i == 0 {
		a.reportFlowError(a.cmdRtspAuth("set"))
		return
	}
	a.reportFlowError(a.cmdRtspAuth("disable"))
}
