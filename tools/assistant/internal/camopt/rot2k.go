package camopt

// The option catalogue of the LSC Rotatable 2K (firmware/lsc-rotatable-2k-ak3918av130/settings.c and netreg.c,
// docs/porting/lsc-rotatable-2k/control-api.md section 4). Its keys are the registry keys of the signed API
// (PATCH /akam/v1/settings), not control-port keys: it has its own list so that the two catalogues never mix
// (net.hostname exists in both and means different things). Every validator here mirrors a check of the
// firmware, so a value that cannot work is refused before anything is sent; the camera still has the last word
// and its own refusal text is shown as it is.
//
// The user-file rows (firmware cfgreg.c: audio.mic_enable, mqtt.*, onvif.*, rtsp.user / rtsp.password, ntp.servers) need an ADMIN key. The two
// passwords are write-only rows (Kind Secret): the camera never returns them, they are never listed, echoed or logged, and the assistant
// asks for them without echo and shows them as "***". All of these apply at the next restart of the app (the supervisor writes the files).

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// Pattern is the kind of an LED rule pattern: "on", "off" or "N:on_ms:off_ms:pause_ms".
const Pattern Kind = Action + 1

// Queued is an Applies value of the 2K: the camera takes the request within seconds and applies it through its
// network script (a Wi-Fi change is confirmed, or reverted, within a minute).
const Queued Applies = NextPowerCycle + 1

// Groups2K are the groups of the 2K catalogue, in screen order.
var Groups2K = []string{"LED and night", "Picture", "Video", "Tracking", "Patrol", "Network", "Audio", "Security"}

var ledRules = []string{"boot", "nowifi", "person", "stream", "night", "idle"}

// crow is one static row of the 2K catalogue (smaller than an Option: no ID, Helper, Needs or ReadOnly).
type crow struct {
	k, g, l, h string
	v          []string
	lo, hi     int32
	kind       Kind
	p          Persist
	ap         Applies
	d          Danger
}

var cat2K0 = []crow{
	{k: "led.mode", g: "LED and night", l: "Front LED", kind: Enum, h: "auto follows the rules below, on and off force it", v: []string{"auto", "on", "off"}},
}

var cat2K1 = []crow{
	{k: "night.ir_led", g: "LED and night", l: "IR night LEDs", kind: Enum, v: []string{"auto", "off"}},
	{k: "image.brightness", g: "Picture", l: "Brightness", kind: Int, hi: 100, h: "0..100, 50 = the camera's own; applied live and saved on the camera (written only when you change it), put back after a day/night switch"},
	{k: "image.contrast", g: "Picture", l: "Contrast", kind: Int, hi: 100, h: "0..100, 50 = the camera's own; applied live and saved on the camera (written only when you change it), put back after a day/night switch"},
	{k: "image.sharpness", g: "Picture", l: "Sharpness", kind: Int, hi: 100, h: "0..100, 50 = the camera's own; applied live and saved on the camera (written only when you change it), put back after a day/night switch"},
	{k: "image.mains_hz", g: "Picture", l: "Mains frequency (Hz)", kind: Enum, h: "anti-flicker; applied live and saved on the camera (written only when you change it)", v: []string{"50", "60"}},
	{k: "image.flip", g: "Picture", l: "Flip (vertical)", kind: Bool, h: "host-tested only, not verified on a camera yet"},
	{k: "image.mirror", g: "Picture", l: "Mirror (horizontal)", kind: Bool, h: "host-tested only, not verified on a camera yet"},
	{k: "image.ceiling", g: "Picture", l: "Ceiling mount (flip + mirror)", kind: Bool, h: "1 turns flip and mirror on, 0 off; saved through the two rows above"},
}

var cat2K2 = []crow{
	{k: "isp.saturation", g: "Picture", l: "Saturation", kind: Int, lo: -50, hi: 50, h: "0 = the camera's own; applied live, put back after a day/night switch; not verified on a camera yet"},
	{k: "isp.denoise", g: "Picture", l: "Denoise (0 = camera's own)", kind: Int, hi: 4, h: "temporal noise reduction taken N light levels higher, like the 1080p (its default there is 2, here 0 until judged); not verified on a camera yet"},
	{k: "isp.nr2", g: "Picture", l: "Noise reduction, spatial (0 = camera's own)", kind: Int, hi: 4, h: "stronger grain removal in the shadows (a backlit scene), more at high gain; 0 sends nothing; applied live; may soften detail; not verified on a camera yet"},
	{k: "isp.wdr", g: "Picture", l: "Backlight strength (WDR)", kind: Int, hi: 100, h: "0 = off, 100 = the full curves of the ISP file; 90 is the stock (nothing changes by default); applied live; not verified on a camera yet"},
	{k: "img.wb", g: "Picture", l: "White balance", kind: Enum, p: PersistRAM, h: "always conf = the camera's automatic white balance. A fixed (manual) white balance is refused on the 2K: the camera gives no way to keep the current gains, and the vendor manual gains give a blue picture", v: []string{"conf"}},
}

var cat2K3 = []crow{
	{k: "detect.publish_hz", g: "Tracking", l: "Person boxes published per second", kind: Int, hi: 10, h: "0 = off; never more than 5 a second"},
	{k: "track.enabled", g: "Tracking", l: "Person tracking", kind: Bool},
	{k: "track.tilt", g: "Tracking", l: "Tilt while tracking", kind: Bool, h: "0 = pan only"},
	{k: "track.return", g: "Tracking", l: "When the person is gone", kind: Enum, h: "stay where it was, or go home", v: []string{"stay", "home"}},
	{k: "track.return_delay_s", g: "Tracking", l: "Seconds before going home", kind: Int, hi: 86400},
	{k: "track.tilt_return_s", g: "Tracking", l: "Seconds before the tilt returns (stay mode)", kind: Int, hi: 3600, h: "0 = never"},
	{k: "track.tilt_back_s", g: "Tracking", l: "Tilt back after (seconds)", kind: Int, hi: 600, h: "0 = off"},
	{k: "track.motion", g: "Tracking", l: "Motion style", kind: Enum, v: []string{"continuous", "steps"}},
	{k: "track.band_pct", g: "Tracking", l: "Pan dead band (% of the width)", kind: Int, lo: 2, hi: 40},
	{k: "track.head_pct", g: "Tracking", l: "Head row (% of the height from the top)", kind: Int, hi: 50},
	{k: "track.tilt_band_pct", g: "Tracking", l: "Tilt dead band (% of the height)", kind: Int, lo: 2, hi: 40},
	{k: "track.gain_pan", g: "Tracking", l: "Pan gain (steps per frame width)", kind: Int, lo: 200, hi: 4000},
	{k: "track.gain_tilt", g: "Tracking", l: "Tilt gain (steps per frame height)", kind: Int, lo: 200, hi: 4000},
	{k: "track.zone_pct", g: "Tracking", l: "Comfort zone width (%)", kind: Int, hi: 90, h: "0 = the old targeting"},
	{k: "track.zone_y_pct", g: "Tracking", l: "Comfort zone height, body (%)", kind: Int, hi: 90},
	{k: "track.zone_head_pct", g: "Tracking", l: "Comfort zone height, head (%)", kind: Int, hi: 90},
	{k: "track.lost_policy", g: "Tracking", l: "Lost-person policy", kind: String, lo: 1, hi: 255, h: "none, or up to 8 steps joined by commas: hold:N, backsweep, exit, local:W:D:N, route:NAME[:N], home, stay (needs track.motion continuous)"},
	{k: "track.close_scan", g: "Tracking", l: "Close-person scan", kind: Bool},
	{k: "track.close_scan_up", g: "Tracking", l: "Close scan, steps up", kind: Int, hi: 600},
	{k: "track.close_scan_down", g: "Tracking", l: "Close scan, steps down", kind: Int, hi: 600},
	{k: "track.lag_ms", g: "Tracking", l: "Video lag (ms)", kind: Int, lo: -150, hi: 300, h: "calibrate with tools/track_lagfit"},
	{k: "track.search", g: "Tracking", l: "Search when the person is lost", kind: Enum, v: []string{"legacy", "motion"}, h: "motion (default) = at most two short looks toward where the person went or where something moves, no sweeps, no stops, fewer motor moves; someone who stays just out of view is no longer searched for. legacy = the scans and sweeps. Needs track.motion continuous"},
	{k: "motion.enabled", g: "Tracking", l: "Motion detection", kind: Bool, h: "on by default; a coarse grid on the picture, gives the direction of a moving person (motion.dir) to the search"},
	{k: "motion.sensitivity", g: "Tracking", l: "Motion sensitivity", kind: Int, lo: 1, hi: 10, h: "5 by default; higher reacts to smaller changes"},
	{k: "ptz.scene_min", g: "Patrol", l: "Boot scene check threshold", kind: String, lo: 4, hi: 5, h: "0.80 to 0.99, default 0.85: how alike the view must be to the saved home view for the motor to skip its boot calibration; a shifted head is refused whatever the score; applied at the next boot check, overrides ptz_scene_min of akam.conf"},
	{k: "ptz.patrol.idle_start_s", g: "Patrol", l: "Start the patrol after idle (seconds)", kind: Int, hi: 86400, h: "0 = patrol off"},
	{k: "ptz.patrol.travel_budget", g: "Patrol", l: "Patrol travel budget (steps)", kind: Int, lo: 1000, hi: 2000000000},
	{k: "ptz.patrol.route_default", g: "Patrol", l: "Default patrol route", kind: String, hi: 16, h: "a route name (a-z first, then a-z 0-9 _, at most 16), or empty"},
	{k: "net.hostname", g: "Network", l: "Host name", kind: String, lo: 1, hi: 32, ap: Queued, d: DangerAsks, h: "1 to 32 letters, digits and '-', not starting or ending with '-', not only digits (RFC 1123 label)"},
	{k: "net.wifi_country", g: "Network", l: "Wi-Fi country", kind: String, lo: 2, hi: 4, ap: Queued, d: DangerAsks, h: "two capital letters (FR, DE, US...) or none; 5 GHz needs a country"},
	{k: "net.wifi_band", g: "Network", l: "Wi-Fi band (GHz)", kind: Enum, v: []string{"2.4", "5"}, ap: Queued, d: DangerAsks, h: "5 needs a country; a change that loses the network is reverted by the camera after about a minute"},
	{k: "audio.mic_enable", g: "Audio", l: "Microphone", kind: Bool, ap: NextPowerCycle, h: "off by default (opt-in): on adds an L16 audio track to both streams, read when the app starts; needs an admin key"},
}

var cat2K4 = []crow{
	{k: "audio.out_enable", g: "Audio", l: "Speaker", kind: Bool, h: "off by default on the 2K; starts at once, no restart; not verified on a camera yet"},
	{k: "audio.out_volume", g: "Audio", l: "Speaker volume", kind: Int, hi: 100, h: "0 mutes"},
	{k: "audio.out_gain", g: "Audio", l: "Speaker hardware gain", kind: Int, hi: 5, h: "the amplifier gain step, 2 is the stock one"},
	{k: "audio.out_amp", g: "Audio", l: "Amplifier", kind: Enum, p: PersistRAM, h: "auto switches it on while there is sound; not saved", v: []string{"auto", "on", "off"}},
	{k: "audio.talk_enable", g: "Audio", l: "Talk (two-way audio)", kind: Bool, h: "the ONVIF backchannel of the stream; needs the stream password and the speaker on"},
	{k: "audio.talk_duplex", g: "Audio", l: "Talk duplex", kind: Enum, h: "half silences the microphone while the speaker plays speech, full never does", v: []string{"half", "full"}},
	{k: "mqtt.host", g: "Network", l: "MQTT broker host", kind: String, hi: 63, ap: NextPowerCycle, h: "a host name or IPv4 address (letters, digits, '.' and '-', at most 63); empty forgets the broker; read when the app starts"},
	{k: "mqtt.port", g: "Network", l: "MQTT broker port", kind: Int, lo: 1, hi: 65535, ap: NextPowerCycle, h: "plain TCP, usually 1883"},
	{k: "mqtt.user", g: "Network", l: "MQTT user", kind: String, hi: 32, ap: NextPowerCycle, h: "1 to 32 of A-Z a-z 0-9 . _ @ + -, or empty for none"},
	{k: "mqtt.password", g: "Network", l: "MQTT password", kind: Secret, hi: 128, ap: NextPowerCycle, h: "write-only, never shown; needs an MQTT user; empty removes it. It travels in clear on your network (no TLS)"},
	{k: "mqtt.enabled", g: "Network", l: "MQTT (Home Assistant)", kind: Bool, ap: NextPowerCycle, h: "off by default; needs a broker host"},
	{k: "onvif.enabled", g: "Network", l: "ONVIF", kind: Bool, ap: NextPowerCycle, h: "off by default; needs a stream password (ONVIF logs in with it)"},
	{k: "onvif.port", g: "Network", l: "ONVIF port", kind: Int, lo: 1024, hi: 65535, ap: NextPowerCycle, h: "default 8000"},
	{k: "ntp.servers", g: "Network", l: "NTP servers", kind: String, hi: 200, ap: NextPowerCycle, h: "1 to 3 host names or IPv4 addresses separated by spaces, or empty for no time sync; the camera has no clock of its own"},
	{k: "rtsp.user", g: "Security", l: "Stream user", kind: String, hi: 32, ap: Now, d: DangerAsks, h: "1 to 32 of A-Z a-z 0-9 . _ @ + - ; empty removes the stream password (no login); a new user needs its password in the same change"},
	{k: "rtsp.password", g: "Security", l: "Stream password", kind: Secret, lo: 8, hi: 128, ap: Now, d: DangerAsks, h: "write-only, never shown; 8 to 128 characters; only its Digest hash is stored on the camera"},
}

// Catalog2K returns every settable option of the 2K, in screen order. ID and Key are both the registry key.
func Catalog2K() []Option {
	var l []Option
	add := func(o Option) {
		o.ID = o.Key
		if o.Kind == Bool {
			o.Values = []string{"0", "1"}
		}
		l = append(l, o)
	}
	// emit adds the static rows (the tables above, kept as data: a table is smaller than one call per option).
	emit := func(rows []crow) {
		for _, r := range rows {
			add(Option{Key: r.k, Group: r.g, Label: r.l, Help: r.h, Kind: r.kind, Values: r.v, Min: int(r.lo), Max: int(r.hi),
				Persisted: r.p, Applies: r.ap, Danger: r.d})
		}
	}
	num := func(key, group, label string, lo, hi int, p Persist, help string) {
		add(Option{Key: key, Group: group, Label: label, Kind: Int, Min: lo, Max: hi, Persisted: p, Help: help})
	}
	enum := func(key, group, label string, p Persist, help string, vals ...string) {
		add(Option{Key: key, Group: group, Label: label, Kind: Enum, Values: vals, Persisted: p, Help: help})
	}
	emit(cat2K0)
	for _, r := range ledRules {
		add(Option{Key: "led." + r, Group: "LED and night", Label: "LED pattern: " + r, Kind: Pattern, Persisted: PersistCamera,
			Help: "on, off or N:on_ms:off_ms:pause_ms (N 1..10, times 50..60000 ms, pause 0 or 50..60000, whole cycle at most 120000 ms)"})
	}
	emit(cat2K1)
	// The 1080p's own names, ranges and meaning (control port isp.saturation, isp.denoise, img.wb), through the 2K ISP. Host-tested only on the 2K.
	emit(cat2K2)
	for s, r := range [2]struct{ kLo, kHi, mLo, mHi int }{{64, 8000, 64, 12000}, {32, 1000, 32, 1500}} {
		g := "Video"
		p := fmt.Sprintf("video%d.", s)
		name := [2]string{"main stream", "second stream"}[s]
		num(p+"kbps", g, "Bitrate, "+name+" (kbit/s)", r.kLo, r.kHi, PersistCamera, "target bitrate; the stream re-opens by itself")
		add(Option{Key: p + "max_kbps", Group: g, Label: "Peak bitrate, " + name + " (kbit/s)", Kind: Int, Min: 0, Max: r.mHi, Persisted: PersistCamera,
			Help: fmt.Sprintf("0 = automatic (1.25 x the target), else %d to %d and not below the target", r.mLo, r.mHi)})
		num(p+"fps", g, "Frame rate, "+name, 1, 15, PersistCamera, "")
		num(p+"min_qp", g, "Lowest quantiser, "+name, 10, 51, PersistCamera, "not above the highest quantiser")
		num(p+"max_qp", g, "Highest quantiser, "+name, 10, 51, PersistCamera, "not below the lowest quantiser")
		num(p+"gop", g, "Key frame interval, "+name+" (frames)", 1, 300, PersistCamera, "")
		enum(p+"rc", g, "Rate control, "+name, PersistCamera, "", "vbr", "cbr", "avbr")
	}
	emit(cat2K3)
	// Speaker and two-way audio: the 1080p's keys (control port audio.*), the same engine. The speaker is opt-in on the 2K.
	emit(cat2K4)
	return l
}

// Find2K returns the option with this registry key.
func Find2K(key string) (Option, bool) {
	for _, o := range Catalog2K() {
		if o.Key == key {
			return o, true
		}
	}
	return Option{}, false
}

var (
	patternRe  = regexp.MustCompile(`^([0-9]{1,2}):([0-9]{1,5}):([0-9]{1,5}):([0-9]{1,5})$`)
	sceneMinRe = regexp.MustCompile(`^0\.(8[0-9]|9[0-9])$`)
	lostPolRe  = regexp.MustCompile(`^[A-Za-z0-9:,._-]{1,255}$`)
	routeRe    = regexp.MustCompile(`^[a-z][a-z0-9_]{0,15}$`)
	countryRe  = regexp.MustCompile(`^[A-Z]{2}$`)
	whole2KRe  = regexp.MustCompile(`^-?[0-9]{1,10}$`)
	hostLabel  = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?$`)
	onlyDigits = regexp.MustCompile(`^[0-9]+$`)
)

// ValidHostname2K mirrors a2k_netreg_hostname_ok: 1 to 32 characters, letters, digits and '-', no '-' at either end,
// and not only digits.
func ValidHostname2K(s string) bool {
	return len(s) >= 1 && len(s) <= 32 && hostLabel.MatchString(s) && !onlyDigits.MatchString(s)
}

// validPattern mirrors the LED pattern grammar of leds.h.
func validPattern(s string) bool {
	if s == "on" || s == "off" {
		return true
	}
	m := patternRe.FindStringSubmatch(s)
	if m == nil {
		return false
	}
	n, _ := strconv.Atoi(m[1])
	on, _ := strconv.Atoi(m[2])
	off, _ := strconv.Atoi(m[3])
	pause, _ := strconv.Atoi(m[4])
	return n >= 1 && n <= 10 && on >= 50 && on <= 60000 && off >= 50 && off <= 60000 &&
		(pause == 0 || pause >= 50 && pause <= 60000) && n*(on+off)+pause <= 120000
}

// Validate2K checks a user-typed value for the 2K option with this key and returns the canonical text the API
// takes (no leading zeros, 0/1 for booleans, capital country). Nothing is sent when it fails.
func Validate2K(key, value string) (string, error) {
	o, ok := Find2K(key)
	if !ok || o.ReadOnly {
		return "", fmt.Errorf("%s is not a setting of the 2K that can be changed here (settings list shows them)", key)
	}
	bad := func(f string, a ...any) (string, error) {
		return "", fmt.Errorf("%s: "+f, append([]any{o.Label}, a...)...)
	}
	switch key {
	case "net.hostname":
		if !ValidHostname2K(value) {
			return bad("1 to 32 letters, digits and '-', not starting or ending with '-', not only digits")
		}
		return value, nil
	case "net.wifi_country":
		v := strings.ToUpper(value)
		if v == "NONE" {
			return "none", nil
		}
		if !countryRe.MatchString(v) {
			return bad("two capital letters (for example FR) or none")
		}
		return v, nil
	}
	switch key {
	case "mqtt.host":
		if value != "" && !ValidServiceHost(value) {
			return bad("a host name or IPv4 address (letters, digits, '.' and '-', at most 63), or empty")
		}
		return value, nil
	case "mqtt.user", "rtsp.user":
		if value != "" && !ValidServiceUser(value) {
			return bad("1 to 32 of A-Z a-z 0-9 . _ @ + - (not a lone -), or empty")
		}
		return value, nil
	case "mqtt.password":
		if len(value) > 128 || hasControl(value) {
			return bad("at most 128 printable characters")
		}
		return value, nil
	case "rtsp.password":
		if len(value) < 8 || len(value) > 128 || hasControl(value) {
			return bad("8 to 128 printable characters")
		}
		return value, nil
	case "ntp.servers":
		v := strings.Join(strings.Fields(value), " ")
		if v == "" {
			return "", nil
		}
		if err := ValidNTPServers(strings.Fields(v)); err != nil {
			return bad("%s", strings.TrimPrefix(err.Error(), "ntp set needs "))
		}
		return v, nil
	}
	switch o.Kind {
	case Int:
		if !whole2KRe.MatchString(value) {
			return bad("a whole number from %d to %d", o.Min, o.Max)
		}
		n, _ := strconv.Atoi(value)
		if n < o.Min || n > o.Max {
			return bad("a whole number from %d to %d", o.Min, o.Max)
		}
		if strings.HasSuffix(key, ".max_kbps") && n != 0 {
			lo := 64
			if strings.HasPrefix(key, "video1.") {
				lo = 32
			}
			if n < lo {
				return bad("0 (automatic) or %d to %d", lo, o.Max)
			}
		}
		return strconv.Itoa(n), nil
	case Bool:
		switch strings.ToLower(value) {
		case "1", "on", "true", "yes":
			return "1", nil
		case "0", "off", "false", "no":
			return "0", nil
		}
		return bad("on or off (1 or 0)")
	case Enum:
		for _, v := range o.Values {
			if v == value {
				return v, nil
			}
		}
		if o.Key == "img.wb" && value == "manual" {
			return bad("a fixed (manual) white balance is refused on the 2K: the camera gives no way to keep the current gains, and the vendor manual gains give a blue picture; conf is the camera's automatic white balance")
		}
		return bad("one of %s", strings.Join(o.Values, ", "))
	case Pattern:
		if !validPattern(value) {
			return bad("on, off or N:on_ms:off_ms:pause_ms (N 1..10, times 50..60000 ms, whole cycle at most 120000 ms)")
		}
		return value, nil
	case String:
		if key == "ptz.patrol.route_default" {
			if value != "" && !routeRe.MatchString(value) {
				return bad("a route name: a-z first, then a-z 0-9 _, at most 16 characters (or empty)")
			}
			return value, nil
		}
		if key == "ptz.scene_min" {
			if !sceneMinRe.MatchString(value) {
				return bad("0.80 to 0.99 (two decimals, for example 0.85)")
			}
			return value, nil
		}
		if key == "track.lost_policy" {
			if !lostPolRe.MatchString(value) || len(strings.Split(value, ",")) > 8 {
				return bad("none, or up to 8 steps joined by commas (letters, digits and : . _ - only)")
			}
			return value, nil
		}
	}
	return "", fmt.Errorf("%s cannot be set here", key)
}

// Check2K mirrors the cross-field checks of the firmware for a whole change set: pending holds the validated new
// values, current what the camera reported (may be empty or partial: a rule that needs an unknown value is left
// to the camera). It returns one error per broken rule.
func Check2K(pending, current map[string]string) []error {
	val := func(k string) (string, bool) {
		if v, ok := pending[k]; ok {
			return v, true
		}
		v, ok := current[k]
		return v, ok
	}
	var errs []error
	_, bandSet := pending["net.wifi_band"]
	_, countrySet := pending["net.wifi_country"]
	if band, ok1 := val("net.wifi_band"); ok1 && band == "5" {
		if c, ok2 := val("net.wifi_country"); ok2 && c == "none" && (bandSet || countrySet) {
			if countrySet {
				errs = append(errs, fmt.Errorf("the Wi-Fi country cannot be none while the band is 5 GHz: set the band to 2.4 in the same change, or choose a country"))
			} else {
				errs = append(errs, fmt.Errorf("the 5 GHz band needs a Wi-Fi country: set net.wifi_country first (two capitals, for example FR)"))
			}
		}
	}
	atoi := func(k string) (int, bool) {
		v, ok := val(k)
		if !ok {
			return 0, false
		}
		n, err := strconv.Atoi(v)
		return n, err == nil
	}
	for s := 0; s < 2; s++ {
		p := fmt.Sprintf("video%d.", s)
		touched := func(ks ...string) bool {
			for _, k := range ks {
				if _, ok := pending[p+k]; ok {
					return true
				}
			}
			return false
		}
		if touched("min_qp", "max_qp") {
			lo, ok1 := atoi(p + "min_qp")
			hi, ok2 := atoi(p + "max_qp")
			if ok1 && ok2 && lo > hi {
				errs = append(errs, fmt.Errorf("%smin_qp (%d) must not be above %smax_qp (%d)", p, lo, p, hi))
			}
		}
		if touched("kbps", "max_kbps") {
			kb, ok1 := atoi(p + "kbps")
			mx, ok2 := atoi(p + "max_kbps")
			if ok1 && ok2 && mx != 0 && mx < kb {
				errs = append(errs, fmt.Errorf("%smax_kbps (%d) must be 0 (automatic) or not below %skbps (%d)", p, mx, p, kb))
			}
		}
	}
	return append(errs, checkServices2K(pending, current)...)
}

// Display2K is how a value is shown: secrets (none exist in the 2K registry today) are never echoed, and a value
// of a key whose name says it is a credential is masked even when the camera reports it.
func Display2K(o Option, v string) string {
	if o.Kind == Secret || looksSecret(o.Key) {
		if v == "" {
			return "(not set)"
		}
		return "(set, hidden)"
	}
	if o.Kind == Bool {
		if v == "1" {
			return "on"
		}
		if v == "0" {
			return "off"
		}
	}
	return v
}

func looksSecret(key string) bool {
	k := strings.ToLower(key)
	for _, w := range []string{"pass", "secret", "token", "psk", "key"} {
		if strings.Contains(k, w) {
			return true
		}
	}
	return false
}

// Describe2K is the short "applies when / saved where" note of a 2K option.
func Describe2K(o Option) string {
	when := "applies now"
	if o.Applies == Queued {
		when = "queued: the camera applies it within seconds"
	}
	if o.Persisted == PersistRAM {
		return when + "; not saved (back to the default at a restart)"
	}
	return when + "; saved on the camera"
}

// ValidServiceHost mirrors a2k_cfgreg_host_ok (the grammar of the strict mqtt file of the app): 1 to 63 of A-Z a-z 0-9 . -.
func ValidServiceHost(s string) bool { return hostRe.MatchString(s) }

// ValidServiceUser mirrors a2k_cfgreg_user_ok: 1 to 32 of A-Z a-z 0-9 . _ @ + -, not a lone '-'.
func ValidServiceUser(s string) bool { return s != "-" && userRe.MatchString(s) }

var (
	hostRe = regexp.MustCompile(`^[A-Za-z0-9.-]{1,63}$`)
	userRe = regexp.MustCompile(`^[A-Za-z0-9._@+-]{1,32}$`)
)

func hasControl(s string) bool {
	for _, r := range s {
		if r < 0x20 || r == 0x7f {
			return true
		}
	}
	return false
}

// checkServices2K mirrors the cross-field rules of cfgreg.c for a change set; what the camera alone knows (the stored hash, its auth state) is left
// to the camera, whose refusal text is shown as it is.
func checkServices2K(pending, current map[string]string) []error {
	var errs []error
	val := func(k string) (string, bool) {
		if v, ok := pending[k]; ok {
			return v, true
		}
		v, ok := current[k]
		return v, ok
	}
	if v, ok := pending["mqtt.enabled"]; ok && v == "1" {
		if h, known := val("mqtt.host"); known && h == "" {
			errs = append(errs, fmt.Errorf("MQTT needs a broker host: set mqtt.host in the same change"))
		}
	}
	if pw, ok := pending["mqtt.password"]; ok && pw != "" {
		if u, known := val("mqtt.user"); known && u == "" {
			errs = append(errs, fmt.Errorf("an MQTT password needs an MQTT user: set mqtt.user in the same change"))
		}
	}
	user, userSet := pending["rtsp.user"]
	_, pwSet := pending["rtsp.password"]
	if userSet && user == "" && pwSet {
		errs = append(errs, fmt.Errorf("an empty stream user removes the stream password: it cannot come with a new password"))
	}
	if userSet && user != "" && !pwSet {
		if cur, known := current["rtsp.user"]; !known || cur != user {
			errs = append(errs, fmt.Errorf("a new stream user needs its password in the same change (only the password makes the hash)"))
		}
	}
	if v, ok := pending["onvif.enabled"]; ok && v == "1" {
		if u, known := val("rtsp.user"); known && u == "" {
			errs = append(errs, fmt.Errorf("ONVIF logs in with the stream credentials: set a stream user and password first (in the same change is fine)"))
		}
	}
	return errs
}
