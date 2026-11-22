package camopt

// The option catalogue of the LSC Rotatable 2K (firmware/lsc-rotatable-2k-ak3918av130/settings.c and netreg.c,
// docs/porting/lsc-rotatable-2k/control-api.md section 4). Its keys are the registry keys of the signed API
// (PATCH /akam/v1/settings), not control-port keys: it has its own list so that the two catalogues never mix
// (net.hostname exists in both and means different things). Every validator here mirrors a check of the
// firmware, so a value that cannot work is refused before anything is sent; the camera still has the last word
// and its own refusal text is shown as it is.
//
// Not in the registry of the camera (so not here): the MQTT keys (akam2k.conf), the stream password and
// anything secret ("No secret setting exists; a secret would be refused over HTTP").

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
var Groups2K = []string{"LED and night", "Picture", "Video", "Tracking", "Patrol", "Network"}

var ledRules = []string{"boot", "nowifi", "person", "stream", "night", "idle"}

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
	num := func(key, group, label string, lo, hi int, p Persist, help string) {
		add(Option{Key: key, Group: group, Label: label, Kind: Int, Min: lo, Max: hi, Persisted: p, Help: help})
	}
	enum := func(key, group, label string, p Persist, help string, vals ...string) {
		add(Option{Key: key, Group: group, Label: label, Kind: Enum, Values: vals, Persisted: p, Help: help})
	}
	boolean := func(key, group, label string, p Persist, help string) {
		add(Option{Key: key, Group: group, Label: label, Kind: Bool, Persisted: p, Help: help})
	}
	enum("led.mode", "LED and night", "Front LED", PersistCamera, "auto follows the rules below, on and off force it", "auto", "on", "off")
	for _, r := range ledRules {
		add(Option{Key: "led." + r, Group: "LED and night", Label: "LED pattern: " + r, Kind: Pattern, Persisted: PersistCamera,
			Help: "on, off or N:on_ms:off_ms:pause_ms (N 1..10, times 50..60000 ms, pause 0 or 50..60000, whole cycle at most 120000 ms)"})
	}
	enum("night.ir_led", "LED and night", "IR night LEDs", PersistCamera, "", "auto", "off")
	num("image.brightness", "Picture", "Brightness", 0, 100, PersistRAM, "not saved: back to 50 at the next camera restart")
	num("image.contrast", "Picture", "Contrast", 0, 100, PersistRAM, "not saved: back to 50 at the next camera restart")
	num("image.sharpness", "Picture", "Sharpness", 0, 100, PersistRAM, "not saved: back to 50 at the next camera restart")
	enum("image.mains_hz", "Picture", "Mains frequency (Hz)", PersistRAM, "anti-flicker; not saved", "50", "60")
	boolean("image.flip", "Picture", "Flip (vertical)", PersistCamera, "host-tested only, not verified on a camera yet")
	boolean("image.mirror", "Picture", "Mirror (horizontal)", PersistCamera, "host-tested only, not verified on a camera yet")
	boolean("image.ceiling", "Picture", "Ceiling mount (flip + mirror)", PersistCamera, "1 turns flip and mirror on, 0 off; saved through the two rows above")
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
	num("detect.publish_hz", "Tracking", "Person boxes published per second", 0, 10, PersistCamera, "0 = off; never more than 5 a second")
	boolean("track.enabled", "Tracking", "Person tracking", PersistCamera, "")
	boolean("track.tilt", "Tracking", "Tilt while tracking", PersistCamera, "0 = pan only")
	enum("track.return", "Tracking", "When the person is gone", PersistCamera, "stay where it was, or go home", "stay", "home")
	num("track.return_delay_s", "Tracking", "Seconds before going home", 0, 86400, PersistCamera, "")
	num("track.tilt_return_s", "Tracking", "Seconds before the tilt returns (stay mode)", 0, 3600, PersistCamera, "0 = never")
	num("track.tilt_back_s", "Tracking", "Tilt back after (seconds)", 0, 600, PersistCamera, "0 = off")
	enum("track.motion", "Tracking", "Motion style", PersistCamera, "", "continuous", "steps")
	num("track.band_pct", "Tracking", "Pan dead band (% of the width)", 2, 40, PersistCamera, "")
	num("track.head_pct", "Tracking", "Head row (% of the height from the top)", 0, 50, PersistCamera, "")
	num("track.tilt_band_pct", "Tracking", "Tilt dead band (% of the height)", 2, 40, PersistCamera, "")
	num("track.gain_pan", "Tracking", "Pan gain (steps per frame width)", 200, 4000, PersistCamera, "")
	num("track.gain_tilt", "Tracking", "Tilt gain (steps per frame height)", 200, 4000, PersistCamera, "")
	num("track.zone_pct", "Tracking", "Comfort zone width (%)", 0, 90, PersistCamera, "0 = the old targeting")
	num("track.zone_y_pct", "Tracking", "Comfort zone height, body (%)", 0, 90, PersistCamera, "")
	num("track.zone_head_pct", "Tracking", "Comfort zone height, head (%)", 0, 90, PersistCamera, "")
	add(Option{Key: "track.lost_policy", Group: "Tracking", Label: "Lost-person policy", Kind: String, Min: 1, Max: 255, Persisted: PersistCamera,
		Help: "none, or up to 8 steps joined by commas: hold:N, backsweep, exit, local:W:D:N, route:NAME[:N], home, stay (needs track.motion continuous)"})
	boolean("track.close_scan", "Tracking", "Close-person scan", PersistCamera, "")
	num("track.close_scan_up", "Tracking", "Close scan, steps up", 0, 600, PersistCamera, "")
	num("track.close_scan_down", "Tracking", "Close scan, steps down", 0, 600, PersistCamera, "")
	num("track.lag_ms", "Tracking", "Video lag (ms)", -150, 300, PersistCamera, "calibrate with tools/track_lagfit")
	num("ptz.patrol.idle_start_s", "Patrol", "Start the patrol after idle (seconds)", 0, 86400, PersistCamera, "0 = patrol off")
	num("ptz.patrol.travel_budget", "Patrol", "Patrol travel budget (steps)", 1000, 2000000000, PersistCamera, "")
	add(Option{Key: "ptz.patrol.route_default", Group: "Patrol", Label: "Default patrol route", Kind: String, Min: 0, Max: 16, Persisted: PersistCamera,
		Help: "a route name (a-z first, then a-z 0-9 _, at most 16), or empty"})
	add(Option{Key: "net.hostname", Group: "Network", Label: "Host name", Kind: String, Min: 1, Max: 32, Persisted: PersistCamera, Applies: Queued, Danger: DangerAsks,
		Help: "1 to 32 letters, digits and '-', not starting or ending with '-', not only digits (RFC 1123 label)"})
	add(Option{Key: "net.wifi_country", Group: "Network", Label: "Wi-Fi country", Kind: String, Min: 2, Max: 4, Persisted: PersistCamera, Applies: Queued, Danger: DangerAsks,
		Help: "two capital letters (FR, DE, US...) or none; 5 GHz needs a country"})
	add(Option{Key: "net.wifi_band", Group: "Network", Label: "Wi-Fi band (GHz)", Kind: Enum, Values: []string{"2.4", "5"}, Persisted: PersistCamera, Applies: Queued, Danger: DangerAsks,
		Help: "5 needs a country; a change that loses the network is reverted by the camera after about a minute"})
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
	return errs
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
