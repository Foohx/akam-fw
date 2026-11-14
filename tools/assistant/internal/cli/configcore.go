package cli

import (
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Configs (the "config" command and the Configs screen): the settings of a camera saved on this computer,
// as a global template (profiles/global.conf) or as the local config of one camera (camera.conf in its
// state directory), with the secrets and the Wi-Fi in a separate private file that exists only when the
// user asked for it. The file format and the protection are in internal/profile; this file reads a camera
// into a profile, compares a profile with a camera and applies the differences through the same camera
// commands as the option screens (the control port SETs and the helper scripts, all behind forbiddenRemote).
//
// Nothing here synchronises anything: a global config is only ever applied when the user asks, and a
// camera keeps what it has.

// State keys of a camera (this PC only, never a secret):
//
//	config_source   where the settings of this camera came from: global | camera:NAME | local
//	config_pending  1 while a config chosen at the first install is still to be applied
const (
	keyConfigSource  = "config_source"
	keyConfigPending = "config_pending"
)

// groupTitle is how a group reads in a list.
var groupTitle = map[string]string{
	"led": "LED and night", "picture": "Picture", "video": "Video", "audio": "Audio",
	"onvif": "ONVIF", "ntp": "Time sync (NTP)", "mqtt": "MQTT / Home Assistant", "auth": "Video password", "key": "SSH key",
}

// keyLabel is the label of one settings key.
func keyLabel(k string) string {
	if o, ok := camopt.Find(k); ok {
		return o.Label
	}
	return map[string]string{
		profile.OnvifEnabled: "ONVIF", profile.OnvifPort: "ONVIF port", profile.NTPEnabled: "Time sync", profile.NTPServers: "Time servers",
		profile.MQTTEnabled: "MQTT", profile.MQTTHost: "Broker", profile.MQTTPort: "Broker port", profile.MQTTUser: "Broker user",
		profile.MQTTBaseTopic: "Base topic", profile.MQTTClientID: "Client id", profile.MQTTDiscovery: "Discovery",
		profile.MQTTPrefix: "Discovery prefix", profile.MQTTInterval: "Publish interval (s)",
		profile.AuthEnabled: "Video password", profile.AuthUser: "Video user",
		profile.KeyKind: "SSH key", profile.KeyPath: "SSH key file",
		profile.SavedFrom: "Saved from", profile.SavedAt: "Saved at",
	}[k]
}

// valueShown is how a value reads on screen: 0/1 enables are off/on, the key kind is spelled out.
func valueShown(k, v string) string {
	if v == "" {
		return "-"
	}
	if o, ok := camopt.Find(k); ok {
		if o.Kind == camopt.Enum {
			for i, x := range o.Values {
				if x == v {
					return enumLabels(o)[i]
				}
			}
		}
		return v
	}
	switch k {
	case profile.OnvifEnabled, profile.NTPEnabled, profile.MQTTEnabled, profile.AuthEnabled:
		return map[string]string{"0": "off", "1": "on"}[v]
	case profile.NTPServers:
		return strings.ReplaceAll(v, ",", " ")
	}
	return ui.Sanitize(v)
}

// cfgKeys are the keys of a settings file in the order a screen shows them, without the bookkeeping ones.
func cfgKeys() []string { return profile.SettingKeys() }

// ---------------------------------------------------------------- places and sources

func (a *App) globalPlace() profile.Place { return profile.Global(a.Cams.Base) }

func (a *App) cameraPlace(name string) profile.Place {
	return profile.Camera(name, a.Cams.DirOf(name))
}

// cfgSrc is a config to use: where it came from and what it holds.
type cfgSrc struct {
	label string // "global" | "camera:NAME" | "local": the value of config_source
	desc  string // for the user: "the global config", "camera kitchen (read now over SSH)", ...
	set   *profile.File
	sec   *profile.File // never nil
	// ssid is the Wi-Fi name the camera reports (the default of the Wi-Fi question); missing is what a live
	// read could not give (the settings are then incomplete, and say so)
	ssid    string
	missing []string
}

// loadPlace reads both files of a place; the secrets file is optional.
func loadPlace(p profile.Place) (set, sec *profile.File, err error) {
	if set, err = profile.Load(p, profile.Settings); err != nil {
		return nil, nil, err
	}
	if sec, err = profile.LoadOrNew(p, profile.Secrets); err != nil {
		return nil, nil, err
	}
	return set, sec, nil
}

// globalSource is the global template.
func (a *App) globalSource() (*cfgSrc, error) {
	set, sec, err := loadPlace(a.globalPlace())
	if err != nil {
		return nil, err
	}
	return &cfgSrc{label: "global", desc: "the global config", set: set, sec: sec}, nil
}

// savedStamp says when a file was saved ("saved 2026-11-13 from camera kitchen").
func savedStamp(set *profile.File) string {
	s := ""
	if v := set.Get(profile.SavedAt); v != "" {
		s = "saved " + v
	}
	if v := set.Get(profile.SavedFrom); v != "" {
		s = strings.TrimSpace(s + " from " + v)
	}
	return s
}

// cameraSource reads the settings of camera name: live when it answers (read-only, over SSH with that
// camera's own key and its pinned host key, never a first contact), else what was last saved for it on this
// computer. It says which one it used and what could not be read. A camera whose host key changed, whose
// control port said nothing, or that calls itself by another name than the one this computer knows is not used live.
func (a *App) cameraSource(name string) (*cfgSrc, error) {
	p := a.cameraPlace(name)
	sec, err := profile.LoadOrNew(p, profile.Secrets) // the secrets of that camera (opt-in), whatever the settings say
	if err != nil {
		return nil, a.UI.Fail(err.Error(), "fix or delete that file ("+Prog+" config forget-secrets --camera "+name+").")
	}
	var live *cfgRead
	var why string
	read := func(c *camssh.Client, other bool) {
		defer c.Close()
		r := a.cfgReadLive(c, name)
		switch {
		case !r.ctlOK:
			why = "its control port did not answer"
		case other && a.hostMismatch(c, name):
			why = "it calls itself by another name than the one this computer knows (is the address still the same camera?)"
		default:
			live = &r
		}
	}
	if name == a.Cams.Name {
		ip, err := a.camNeedIP()
		if err != nil {
			return nil, err
		}
		if c, err := a.connectTools(ip); err == nil {
			read(c, false)
		} else {
			why = "it did not answer"
		}
	} else if ip := a.Cams.ReadKey(name, "camera_ip"); ip == "" {
		why = "no address is known for it"
	} else {
		pass := ""
		keyPriv := camssh.KeyPathFor("", "", a.Cams.ReadKey(name, "ssh_pubkey"))
		c, err := a.dialKey(ip, keyPriv, &pass)
		var hk *camssh.HostKeyChangedError
		switch {
		case err == nil:
			read(c, true)
		case errors.As(err, &hk):
			a.UI.Err(fmt.Sprintf("REMOTE HOST IDENTIFICATION HAS CHANGED for camera %s (%s): it now shows %s. It is not read.", name, ip, hk.Fingerprint))
			why = "its SSH host key changed"
		case errors.Is(err, camssh.ErrHostKeyRefused):
			why = "its SSH host key is not remembered yet (connect to it once: " + Prog + " --camera " + name + " config show, or find-camera)"
		default:
			why = "it did not answer"
		}
	}
	if live != nil {
		a.UI.Say(fmt.Sprintf("  Using the settings camera %s has right now (read over SSH).", name))
		a.reportMissing(*live)
		return &cfgSrc{label: "camera:" + name, desc: "camera " + name + " (read now over SSH)", set: live.f, sec: sec, ssid: live.ssid, missing: live.missing}, nil
	}
	set, err := profile.Load(p, profile.Settings)
	if err != nil {
		if err == profile.ErrNone {
			return nil, a.UI.Fail(fmt.Sprintf("camera %s cannot be read (%s) and nothing was saved for it on this computer.", name, why),
				fmt.Sprintf("switch it on and on your network, or run '%s --camera %s config save-local' once it answers.", Prog, name))
		}
		return nil, a.UI.Fail(err.Error(), "fix or delete that file.")
	}
	a.UI.Warn(fmt.Sprintf("camera %s is not read live (%s): using the settings last saved for it on this computer (%s).", name, why, orUnknown(savedStamp(set))))
	return &cfgSrc{label: "camera:" + name, desc: "camera " + name + " (as last saved on this computer)", set: set, sec: sec}, nil
}

// hostMismatch is true when the camera behind c reports a host name and this computer knows another one for
// camera name (both non-empty).
func (a *App) hostMismatch(c *camssh.Client, name string) bool {
	want := a.Cams.ReadKey(name, "camera_host")
	if want == "" {
		return false
	}
	got := ctl.Get("sys.hostname", a.ctlExchange(c, "GET sys.hostname"))
	return got != "" && camopt.SafeValue(got) && got != want
}

func orUnknown(s string) string {
	if s == "" {
		return "date unknown"
	}
	return s
}

// ---------------------------------------------------------------- reading a camera

// cfgRead is what one read of a camera gave.
type cfgRead struct {
	f       *profile.File
	ssid    string   // the Wi-Fi name the camera reports (a default for the Wi-Fi question)
	missing []string // what could not be read (an older image, a helper that did not answer)
	ctlOK   bool     // the control port answered at least one option: false means ak_rtsp said nothing
	miss    []string // the settings keys, or the helper groups (onvif, ntp, mqtt, auth), that could not be read
}

// unread says whether the value of key k could not be read (its key, or the helper group it belongs to).
func (r cfgRead) unread(k string) bool {
	g := profile.GroupOf(k)
	for _, m := range r.miss {
		if m == k || (g != "" && m == g) {
			return true
		}
	}
	return false
}

// cfgHelper runs one helper command of the camera through the guard and returns its cleaned output and
// whether it exited 0. stdin may be nil.
func (a *App) cfgHelper(c *camssh.Client, cmd string, stdin []byte) (string, bool) {
	var res camssh.Result
	var err error
	if stdin == nil {
		res, err = a.run(c, cmd, nil, "")
	} else {
		res, err = a.run(c, cmd, strings.NewReader(string(stdin)), "")
		for i := range stdin {
			stdin[i] = 0
		}
	}
	if err != nil {
		return textsafe.Clean(err.Error()), false
	}
	return strings.TrimRight(textsafe.Clean(string(res.Stdout)+string(res.Stderr)), "\n"), res.Status == 0
}

// firstLine is the first line of a helper answer, short.
func firstLine(s string) string {
	l, _, _ := strings.Cut(strings.TrimSpace(s), "\n")
	if len(l) > 160 {
		l = l[:160]
	}
	return l
}

// cfgReadLive reads every setting a config holds from the camera behind c: the control-port options in one
// exchange, then the helper states (ONVIF, time sync, MQTT, video password) and the key of camera cam (read
// on this PC). Everything is read-only. A value the camera does not give (an older image) is left out.
func (a *App) cfgReadLive(c *camssh.Client, cam string) cfgRead {
	r := cfgRead{f: profile.New(profile.Settings)}
	set := func(k, v string) {
		if err := r.f.Set(k, v); err != nil {
			r.missing = append(r.missing, keyLabel(k))
			r.miss = append(r.miss, k)
		}
	}
	for _, st := range camopt.Read(a.exchanger(c)) {
		if !st.Unavailable {
			r.ctlOK = true
		}
		if st.ID == "net.ssid" && !st.Unavailable {
			r.ssid = st.Value
		}
		if !profile.KnownSetting(st.ID) {
			continue
		}
		if st.Unavailable {
			r.missing = append(r.missing, st.Label)
			r.miss = append(r.miss, st.ID)
			continue
		}
		set(st.ID, st.Value)
	}
	if res, err := a.run(c, remoteOnvifFlag, nil, ""); err != nil {
		r.missing = append(r.missing, "ONVIF") // a read that failed is not "off"
		r.miss = append(r.miss, "onvif")
	} else if on, port := parseOnvifFlag(string(res.Stdout)); on {
		set(profile.OnvifEnabled, "1")
		set(profile.OnvifPort, fmt.Sprint(port))
	} else {
		set(profile.OnvifEnabled, "0")
	}
	if out, ok := a.cfgHelper(c, remoteNtp("status"), nil); ok {
		switch n, _ := parseNtpStatus(out); n {
		case "on":
			set(profile.NTPEnabled, "1")
			for _, l := range strings.Split(out, "\n") {
				if _, v, ok := strings.Cut(l, "servers:"); ok && strings.HasPrefix(strings.TrimSpace(l), "time sync: on") {
					set(profile.NTPServers, strings.Join(strings.Fields(v), ","))
				}
			}
		case "off":
			set(profile.NTPEnabled, "0")
		}
	} else {
		r.missing = append(r.missing, "Time sync")
		r.miss = append(r.miss, "ntp")
	}
	if out, ok := a.cfgHelper(c, remoteMqtt("status"), nil); ok {
		parseMqttStatus(out, set)
	} else {
		r.missing = append(r.missing, "MQTT")
		r.miss = append(r.miss, "mqtt")
	}
	switch st, text := a.rtspStatus(c); st {
	case "enabled":
		set(profile.AuthEnabled, "1")
		if u, ok := strings.CutPrefix(text, "enabled (user "); ok {
			u, _, _ = strings.Cut(u, ",")
			set(profile.AuthUser, strings.TrimSuffix(u, ")"))
		}
	case "disabled":
		set(profile.AuthEnabled, "0")
	default:
		r.missing = append(r.missing, "Video password")
		r.miss = append(r.miss, "auth")
	}
	kind, path := a.keyOf(cam)
	set(profile.KeyKind, kind)
	if kind == camssh.KindCustom {
		set(profile.KeyPath, path)
	}
	return r
}

// keyOf is the key kind of camera cam and, for a custom key, its .pub file (this PC only).
func (a *App) keyOf(cam string) (kind, path string) {
	pub := a.Cams.ReadKey(cam, "ssh_pubkey")
	if cam == a.Cams.Name {
		pub = a.keyPub()
	}
	if pub == "" {
		return camssh.KindShared, ""
	}
	kind = camssh.KindOf(pub, cam)
	if kind == camssh.KindCustom {
		return kind, pub
	}
	return kind, ""
}

// parseMqttStatus reads `akam_mqtt.sh status`: "MQTT: enabled|disabled", "broker: HOST:PORT, user NAME|none"
// and "options: discovery on, prefix P, base topic T, client id I, interval N s". A default that the camera
// prints as <hostname> is not a setting and is left out.
func parseMqttStatus(out string, set func(k, v string)) {
	for _, l := range strings.Split(out, "\n") {
		l = strings.TrimSpace(l)
		switch {
		case strings.HasPrefix(l, "MQTT: enabled"):
			set(profile.MQTTEnabled, "1")
		case strings.HasPrefix(l, "MQTT: disabled"):
			set(profile.MQTTEnabled, "0")
		case strings.HasPrefix(l, "broker: "):
			hp, user, _ := strings.Cut(strings.TrimPrefix(l, "broker: "), ", user ")
			if h, p, ok := strings.Cut(hp, ":"); ok {
				set(profile.MQTTHost, h)
				set(profile.MQTTPort, p)
			}
			if user = strings.TrimSpace(user); user == "none" {
				user = ""
			}
			set(profile.MQTTUser, user)
		case strings.HasPrefix(l, "options: "):
			for _, part := range strings.Split(strings.TrimPrefix(l, "options: "), ", ") {
				var k, v string
				switch {
				case strings.HasPrefix(part, "discovery "):
					k, v = profile.MQTTDiscovery, strings.TrimPrefix(part, "discovery ")
				case strings.HasPrefix(part, "prefix "):
					k, v = profile.MQTTPrefix, strings.TrimPrefix(part, "prefix ")
				case strings.HasPrefix(part, "base topic "):
					k, v = profile.MQTTBaseTopic, strings.TrimPrefix(part, "base topic ")
				case strings.HasPrefix(part, "client id "):
					k, v = profile.MQTTClientID, strings.TrimPrefix(part, "client id ")
				case strings.HasPrefix(part, "interval "):
					k, v = profile.MQTTInterval, strings.TrimSuffix(strings.TrimPrefix(part, "interval "), " s")
				}
				if k != "" && !strings.Contains(v, "<") {
					set(k, v)
				}
			}
		}
	}
}

// ---------------------------------------------------------------- the difference

// The operations of a change.
const (
	opCtl     = "ctl"      // one SET of the control port
	opAuthSet = "auth-set" // video password on (set-creds)
	opAuthOff = "auth-off" // video password off
	opOnvif   = "onvif"    // enable [PORT] | disable
	opNTP     = "ntp"      // set SERVERS | off
	opMqttSet = "mqtt-set" // broker, user, password (turns MQTT on)
	opMqttOpt = "mqtt-opt" // one optional setting
	opMqttOff = "mqtt-off" // disable
	opKey     = "key"      // the SSH key of this camera
)

// cfgChange is one difference between a camera and a config.
type cfgChange struct {
	Op, Group, Label string
	From, To         string // shown to the user: never a secret
	Key, Value       string // opCtl: the option ID and value; opMqttOpt: the helper option and value
	Want             *profile.File
	User             string // opMqttSet: the broker user that will be sent (the config's, else the one the camera has)
	Secret           bool   // a password goes with it (from the secrets file or asked)
	Own              string // when set, this change needs its own yes/no (default No) with this warning
}

// cfgDiff compares what the camera has (live) with what the config wants and lists what would change.
// only limits the groups (nil: all). withSecrets re-sends a stored password even when the rest is the same
// (a password cannot be read back from a camera, so it is otherwise sent only with the setting that needs it).
// skipped says what the config holds that cannot be applied here, and why.
func cfgDiff(live, want, sec *profile.File, only map[string]bool, withSecrets bool) (ch []cfgChange, skipped []string) {
	use := func(g string) bool { return only == nil || only[g] }
	lv := func(k string) string { return live.Get(k) }
	// a group the camera could not be read for is left out (never compared with an empty value)
	readable := func(g, k, label string) bool {
		if live.Has(k) {
			return true
		}
		if use(g) {
			skipped = append(skipped, label+": could not be read from the camera, so it is left out")
		}
		return false
	}
	// 1. the options of the control port
	for _, id := range profile.OptionIDs() {
		g := profile.GroupOf(id)
		if !want.Has(id) || !use(g) {
			continue
		}
		o, _ := camopt.Find(id)
		if !live.Has(id) {
			skipped = append(skipped, o.Label+": this camera's image does not have it (update it first)")
			continue
		}
		if lv(id) != want.Get(id) {
			ch = append(ch, cfgChange{Op: opCtl, Group: g, Label: o.Label, From: valueShown(id, lv(id)), To: valueShown(id, want.Get(id)), Key: id, Value: want.Get(id)})
		}
	}
	// 2. the video password (before ONVIF, which needs it)
	if use("auth") && want.Has(profile.AuthEnabled) && readable("auth", profile.AuthEnabled, "Video password") {
		switch {
		case want.Get(profile.AuthEnabled) == "0":
			if lv(profile.AuthEnabled) == "1" {
				ch = append(ch, cfgChange{Op: opAuthOff, Group: "auth", Label: "Video password", From: "on, user " + lv(profile.AuthUser), To: "off",
					Own: "This REMOVES the video password: anyone on your network could watch the camera."})
			}
		case !want.Has(profile.AuthUser):
			skipped = append(skipped, "Video password: the config has no user name for it")
		default:
			user := want.Get(profile.AuthUser)
			differs := lv(profile.AuthEnabled) != "1" || lv(profile.AuthUser) != user
			stored := profile.BoundAuth(sec, user) != ""
			if differs || (withSecrets && stored) {
				from := "off"
				if lv(profile.AuthEnabled) == "1" {
					from = "on, user " + lv(profile.AuthUser)
				}
				pw := "password: asked"
				if stored {
					pw = "password: set (from the config)"
				}
				if !differs {
					from += ", password: not comparable"
				}
				ch = append(ch, cfgChange{Op: opAuthSet, Group: "auth", Label: "Video password", From: from, To: fmt.Sprintf("on, user %s, %s", user, pw), Secret: true, Want: want})
			}
		}
	}
	// 3. ONVIF
	if use("onvif") && want.Has(profile.OnvifEnabled) && readable("onvif", profile.OnvifEnabled, "ONVIF") {
		port := want.Get(profile.OnvifPort)
		if port == "" {
			port = "80"
		}
		switch {
		case want.Get(profile.OnvifEnabled) == "0":
			if lv(profile.OnvifEnabled) == "1" {
				ch = append(ch, cfgChange{Op: opOnvif, Group: "onvif", Label: "ONVIF", From: "on, port " + lv(profile.OnvifPort), To: "off", Value: "disable"})
			}
		case lv(profile.OnvifEnabled) != "1" || (want.Has(profile.OnvifPort) && lv(profile.OnvifPort) != port):
			from := "off"
			if lv(profile.OnvifEnabled) == "1" {
				from = "on, port " + lv(profile.OnvifPort)
			}
			ch = append(ch, cfgChange{Op: opOnvif, Group: "onvif", Label: "ONVIF", From: from, To: "on, port " + port, Value: "enable " + port})
		}
	}
	// 4. time sync
	if use("ntp") && want.Has(profile.NTPEnabled) && readable("ntp", profile.NTPEnabled, "Time sync") {
		switch {
		case want.Get(profile.NTPEnabled) == "0":
			if lv(profile.NTPEnabled) == "1" {
				ch = append(ch, cfgChange{Op: opNTP, Group: "ntp", Label: "Time sync", From: "on, " + valueShown(profile.NTPServers, lv(profile.NTPServers)), To: "off", Value: "off"})
			}
		case !want.Has(profile.NTPServers):
			skipped = append(skipped, "Time sync: the config has no servers")
		case lv(profile.NTPEnabled) != "1" || lv(profile.NTPServers) != want.Get(profile.NTPServers):
			from := "off"
			if lv(profile.NTPEnabled) == "1" {
				from = "on, " + valueShown(profile.NTPServers, lv(profile.NTPServers))
			}
			ch = append(ch, cfgChange{Op: opNTP, Group: "ntp", Label: "Time sync", From: from, To: "on, " + valueShown(profile.NTPServers, want.Get(profile.NTPServers)), Value: "set " + strings.ReplaceAll(want.Get(profile.NTPServers), ",", " ")})
		}
	}
	// 5. MQTT
	brokerQueued := false
	if use("mqtt") && want.Has(profile.MQTTEnabled) && readable("mqtt", profile.MQTTEnabled, "MQTT") {
		if want.Get(profile.MQTTEnabled) == "0" {
			if lv(profile.MQTTEnabled) == "1" {
				ch = append(ch, cfgChange{Op: opMqttOff, Group: "mqtt", Label: "MQTT", From: "on, " + lv(profile.MQTTHost) + ":" + lv(profile.MQTTPort), To: "off"})
			}
		} else if !want.Has(profile.MQTTHost) {
			skipped = append(skipped, "MQTT: the config has no broker address")
		} else {
			// a key the config does not have is not part of it: the camera keeps what it has (never "no user")
			port := want.Get(profile.MQTTPort)
			if port == "" {
				if port = lv(profile.MQTTPort); port == "" {
					port = "1883"
				}
			}
			user := want.Get(profile.MQTTUser)
			if !want.Has(profile.MQTTUser) {
				user = lv(profile.MQTTUser)
			}
			differs := lv(profile.MQTTEnabled) != "1" || lv(profile.MQTTHost) != want.Get(profile.MQTTHost) || lv(profile.MQTTPort) != port || lv(profile.MQTTUser) != user
			stored := user != "" && profile.BoundMQTT(sec, want.Get(profile.MQTTHost), user) != ""
			if differs || (withSecrets && stored) {
				from := "off"
				if lv(profile.MQTTEnabled) == "1" {
					from = fmt.Sprintf("on, %s:%s, user %s", lv(profile.MQTTHost), lv(profile.MQTTPort), orDash(lv(profile.MQTTUser)))
				}
				to := fmt.Sprintf("on, %s:%s, user %s", want.Get(profile.MQTTHost), port, orDash(user))
				switch {
				case user == "":
				case stored:
					to += ", password: set (from the config)"
				default:
					to += ", password: asked"
				}
				ch = append(ch, cfgChange{Op: opMqttSet, Group: "mqtt", Label: "MQTT broker", From: from, To: to, Secret: user != "", Want: want, Value: port, User: user})
				brokerQueued = true
			}
		}
		// the options need a broker saved on the camera, or one this apply saves
		brokerThere := live.Has(profile.MQTTHost) || brokerQueued
		hasOpt := false
		for _, k := range []string{profile.MQTTBaseTopic, profile.MQTTClientID, profile.MQTTDiscovery, profile.MQTTPrefix, profile.MQTTInterval} {
			hasOpt = hasOpt || (want.Has(k) && lv(k) != want.Get(k))
		}
		if hasOpt && !brokerThere {
			skipped = append(skipped, "MQTT options: no broker is saved on the camera and the config has none to save")
		}
		if brokerThere {
			for _, k := range []string{profile.MQTTBaseTopic, profile.MQTTClientID, profile.MQTTDiscovery, profile.MQTTPrefix, profile.MQTTInterval} {
				if want.Has(k) && lv(k) != want.Get(k) {
					from := lv(k)
					if from == "" {
						from = "default"
					}
					ch = append(ch, cfgChange{Op: opMqttOpt, Group: "mqtt", Label: keyLabel(k), From: from, To: want.Get(k), Key: strings.TrimPrefix(k, "mqtt."), Value: want.Get(k)})
				}
			}
		}
	}
	// 6. the SSH key of this camera
	if use("key") && want.Has(profile.KeyKind) {
		wk, wp := want.Get(profile.KeyKind), want.Get(profile.KeyPath)
		if n, err := normPub(wp); err == nil && wp != "" { // ~ and relative paths mean the file the camera is recorded with
			wp = n
		}
		if wk != lv(profile.KeyKind) || (wk == "custom" && wp != lv(profile.KeyPath)) {
			to := wk
			if wk == "custom" {
				to = "custom (" + tilde(wp) + ")"
			}
			ch = append(ch, cfgChange{Op: opKey, Group: "key", Label: "SSH key", From: lv(profile.KeyKind), To: to, Key: wk, Value: wp,
				Own: "This changes the SSH key that opens this camera (the new key is added, proven with a second login, only then remembered)."})
		}
	}
	return ch, skipped
}

// ---------------------------------------------------------------- applying

// cfgOutcome is what one change did.
type cfgOutcome struct {
	ch      cfgChange
	state   string // applied | failed | skipped
	note    string
	restart bool // the video service must restart to run the new value
	power   bool // applies at the next power cycle
}

// cfgDiffLines are the lines of the difference shown before the question (secrets are masked).
func cfgDiffLines(ch []cfgChange) []string {
	var out []string
	last := ""
	for _, c := range ch {
		if c.Group != last {
			out = append(out, "  "+groupTitle[c.Group])
			last = c.Group
		}
		out = append(out, fmt.Sprintf("    %-26s %s  ->  %s", c.Label, c.From, c.To))
	}
	return out
}

// cfgPasswords resolves, before anything is sent and before the operation panel opens, the passwords the
// changes need: the stored one, else asked (twice, not shown). A change whose password is missing is
// returned as failed and left out of the rest.
func (a *App) cfgPasswords(ch []cfgChange, sec *profile.File) (todo []cfgChange, pws map[string]string, failed []cfgOutcome) {
	pws = map[string]string{}
	for _, x := range ch {
		var pw string
		var ok bool
		switch x.Op {
		case opAuthSet:
			pw, ok = a.cfgPassword("video password", profile.BoundAuth(sec, x.Want.Get(profile.AuthUser)), rtspMinPass)
			pws["auth"] = pw
		case opMqttSet:
			ok = true
			if x.User != "" {
				pw, ok = a.cfgPassword("MQTT password", profile.BoundMQTT(sec, x.Want.Get(profile.MQTTHost), x.User), 0)
				pws["mqtt"] = pw
			}
		default:
			ok = true
		}
		if !ok {
			failed = append(failed, cfgOutcome{ch: x, state: "failed", note: "no password to send (it is not saved in the config and could not be asked)"})
			continue
		}
		todo = append(todo, x)
	}
	return todo, pws, failed
}

// cfgApply applies the changes to the camera behind c, one group of work after the other, and tells what each
// did. It never asks a yes/no except the ones the changes carry (Own) and the video restart.
func (a *App) cfgApply(c *camssh.Client, ip string, ch []cfgChange, pws map[string]string) []cfgOutcome {
	var out []cfgOutcome
	done := func(o cfgOutcome) { out = append(out, o) }
	op := a.op()
	// The panel lists what this function sends. The key change has its own panel and its own questions, so it
	// runs after this one is closed (two panels are never nested).
	var panel []int // indexes in ch of the items on the panel
	pos := map[int]int{}
	var items []string
	for i, x := range ch {
		if x.Op != opKey {
			pos[i] = len(panel)
			panel = append(panel, i)
			items = append(items, x.Label)
		}
	}
	op.plan(items...)
	// every control-port change goes in one exchange (camopt.Apply), the others one by one
	var ctlIdx []int
	var changes []camopt.Change
	for i, x := range ch {
		if x.Op == opCtl {
			ctlIdx = append(ctlIdx, i)
			changes = append(changes, camopt.Change{ID: x.Key, Value: x.Value})
		}
	}
	if len(changes) > 0 {
		op.start(pos[ctlIdx[0]])
		for _, x := range changes {
			o, _ := camopt.Find(x.ID)
			a.UI.Say(fmt.Sprintf("  WILL send: SET %s %s", o.Key, x.Value))
		}
		op.writing(true)
		res, err := camopt.Apply(a.exchanger(c), changes)
		op.writing(false)
		for j, i := range ctlIdx {
			o := cfgOutcome{ch: ch[i]}
			switch {
			case err != nil:
				o.state, o.note = "failed", err.Error()
			case !res[j].Answered:
				o.state, o.note = "failed", "the camera did not confirm (its control port may be busy)"
			case !res[j].OK:
				o.state, o.note = "failed", "the camera refused it: "+ui.Sanitize(res[j].Err)
			default:
				o.state = "applied"
				opt, _ := camopt.Find(res[j].ID)
				o.note = camopt.Describe(opt)
				switch {
				case res[j].NotSaved:
					o.note = "applied now, but the camera could not save it: lost at the next reboot"
				case res[j].Restart || opt.Applies == camopt.VideoRestart:
					o.restart = true
					o.note = "saved; applies when the video service restarts"
				case opt.Applies == camopt.NextPowerCycle:
					o.power = true
					o.note = "saved; applies at the next power cycle"
				}
			}
			done(o)
			op.done(pos[i], o.state)
		}
	}
	for i, x := range ch {
		if x.Op == opCtl || x.Op == opKey {
			continue
		}
		if a.stopRequested() {
			done(cfgOutcome{ch: x, state: "skipped", note: "stopped before it was sent"})
			continue
		}
		op.start(pos[i])
		o := a.cfgRunOne(c, ip, x, pws, op)
		done(o)
		op.done(pos[i], o.state)
	}
	op.end()
	for _, x := range ch {
		if x.Op != opKey {
			continue
		}
		if a.stopRequested() {
			done(cfgOutcome{ch: x, state: "skipped", note: "stopped before it was sent"})
			continue
		}
		done(a.cfgRunOne(c, ip, x, pws, op))
	}
	return out
}

// pwCheck validates a password typed or stored for the stream (8 to 128) or the broker (0 to 128).
func pwProblem(pw string, min int) string {
	switch {
	case len(pw) < min:
		return fmt.Sprintf("too short (at least %d)", min)
	case len(pw) > 128:
		return "too long (at most 128)"
	case hasControl(pw):
		return "it contains a control character"
	}
	return ""
}

// cfgPassword is the password of a change: the stored one, else asked (twice, not shown). "" with ok false
// means there is none and none can be asked here.
func (a *App) cfgPassword(what string, stored string, min int) (string, bool) {
	if stored != "" {
		a.Secrets.Add(stored)
		return stored, true
	}
	if a.UI.Dry() || !a.UI.Interactive() {
		return "", false
	}
	a.UI.Say("  The " + what + " is not saved in this config: type it now.")
	pw, err := a.UI.Secret(fmt.Sprintf("  %s (not shown):", what))
	if err != nil {
		return "", false
	}
	if pw == "" && min == 0 {
		return "", true
	}
	if p := pwProblem(pw, min); p != "" {
		a.UI.Err(what + ": " + p + ".")
		return "", false
	}
	pw2, err := a.UI.Secret("  Type it again:")
	if err != nil || pw != pw2 {
		a.UI.Err("the two entries differ.")
		return "", false
	}
	a.Secrets.Add(pw)
	return pw, true
}

func (a *App) cfgRunOne(c *camssh.Client, ip string, x cfgChange, pws map[string]string, op opr) cfgOutcome {
	o := cfgOutcome{ch: x}
	fail := func(msg string) cfgOutcome { o.state, o.note = "failed", msg; return o }
	okRes := func(out string, ok bool) cfgOutcome {
		if !ok {
			return fail(firstLine(out))
		}
		o.state, o.note = "applied", firstLine(out)
		return o
	}
	switch x.Op {
	case opAuthSet:
		pw := pws["auth"]
		op.writing(true)
		defer op.writing(false)
		payload := []byte(x.Want.Get(profile.AuthUser) + "\n" + pw + "\n")
		out, good := a.cfgHelper(c, remoteRtspAuth("set-creds"), payload)
		if good {
			o.state, o.note = "applied", "on; players that are watching log in again"
			a.secretsAfterSet(profile.AuthPassword, "", x.Want.Get(profile.AuthUser), pw, false)
			return o
		}
		return fail(firstLine(out))
	case opAuthOff:
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteRtspAuth("disable"), nil)
		return okRes(out, good)
	case opOnvif:
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteOnvif(strings.Fields(x.Value)...), nil)
		return okRes(out, good)
	case opNTP:
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteNtp(strings.Fields(x.Value)[0], strings.Fields(x.Value)[1:]...), nil)
		return okRes(out, good)
	case opMqttSet:
		user := x.User
		pw := ""
		if user != "" {
			pw = pws["mqtt"]
		}
		userArg := user
		if userArg == "" {
			userArg = "-"
		}
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteMqtt("set", x.Want.Get(profile.MQTTHost), x.Value, userArg), []byte(pw+"\n"))
		if good {
			o.state, o.note = "applied", "on"
			if user != "" {
				a.secretsAfterSet(profile.MQTTPassword, x.Want.Get(profile.MQTTHost), user, pw, false)
			}
			return o
		}
		return fail(firstLine(out))
	case opMqttOpt:
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteMqttOption(x.Key, x.Value), nil)
		return okRes(out, good)
	case opMqttOff:
		op.writing(true)
		defer op.writing(false)
		out, good := a.cfgHelper(c, remoteMqtt("disable"), nil)
		return okRes(out, good)
	case opKey:
		ch, kerr := a.keyChoiceFor(x.Key, x.Value)
		if kerr != nil {
			return fail(kerr.Error())
		}
		before := a.St.Get("ssh_pubkey")
		if err := a.keyChange(&ch, ip, c); err != nil {
			return fail("the key was not changed (see above)")
		}
		// what counts is what the camera now uses: the key flow may have settled on another file name
		after := a.St.Get("ssh_pubkey")
		if after == before {
			o.state, o.note = "skipped", "not changed (not confirmed)"
			return o
		}
		o.state, o.note = "applied", keyKindText(camssh.KindOf(after, a.Cams.Name), after)
		return o
	}
	return fail("unknown change")
}

// keyChoiceFor is the key a config names: the shared key, the key of this camera only, or a key file (a custom
// path may be written with ~ or relative; it must be a .pub file that exists on this computer).
func (a *App) keyChoiceFor(kind, path string) (keyChoice, error) {
	switch kind {
	case camssh.KindShared:
		return keyChoice{kind: kind, pub: camssh.SharedKeyPath() + ".pub"}, nil
	case camssh.KindOwn:
		return keyChoice{kind: kind, pub: camssh.OwnKeyPath(a.Cams.Name) + ".pub"}, nil
	}
	p, err := normPub(path)
	if err != nil || path == "" || !strings.HasSuffix(p, ".pub") || !exists(p) {
		return keyChoice{}, fmt.Errorf("the key file %s is not on this computer", tilde(p))
	}
	return keyChoice{kind: camssh.KindCustom, pub: p}, nil
}

// cfgReport prints what each change did and returns how many were applied and how many failed.
func (a *App) cfgReport(out []cfgOutcome) (applied, failed int, restart, power bool) {
	for _, o := range out {
		line := fmt.Sprintf("  %-26s %s", o.ch.Label, o.note)
		switch o.state {
		case "applied":
			applied++
			a.UI.Good(fmt.Sprintf("  %-26s applied: %s", o.ch.Label, o.note))
		case "failed":
			failed++
			a.UI.Err(fmt.Sprintf("%s: FAILED: %s", o.ch.Label, o.note))
		default:
			a.UI.Say(line + " (skipped)")
		}
		restart = restart || (o.restart && o.state == "applied")
		power = power || (o.power && o.state == "applied")
	}
	return
}

// cfgNow is the stamp written in a file ("2026-11-13 10:02").
func cfgNow() string { return time.Now().UTC().Format("2006-01-02 15:04") + " UTC" }

// settingsSuffix is the end of the title of the option screen: where the settings of this camera came from
// ("   settings: local (from global)"); nothing until a config was chosen or applied.
func (a *App) settingsSuffix() string {
	if t := sourceText(a.St.Get(keyConfigSource)); t != "" {
		return "   settings: " + t
	}
	return ""
}
