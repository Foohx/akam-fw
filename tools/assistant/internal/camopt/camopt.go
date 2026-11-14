// Package camopt is the catalog of the camera options and the small engine that reads and writes them
// (specs/installer.md). It has no UI code: it returns structured states and applies a list of changes through
// one control-port exchange, so that the command line and the option screens share the same validators.
//
// Two rules hold for every value:
//   - a value typed by the user is validated (range, list, host name grammar) before anything is sent;
//   - a value supplied by the camera is never trusted: it is only accepted when it has a safe shape
//     (see SafeValue) and is never concatenated into a shell command.
package camopt

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// Kind is the type of an option.
type Kind int

// The kinds of options.
const (
	Enum Kind = iota
	Int
	Bool
	String
	Secret
	Action
)

// Persist tells where a setting lives.
type Persist int

// Where a setting is kept.
const (
	PersistCamera Persist = iota // saved on the camera, kept after a reboot
	PersistRAM                   // back to the default at the next reboot
	PersistNone                  // read-only
)

// Applies tells when a change takes effect.
type Applies int

// When a change takes effect.
const (
	Now Applies = iota
	VideoRestart
	NextPowerCycle
)

func (a Applies) String() string {
	switch a {
	case VideoRestart:
		return "video-restart"
	case NextPowerCycle:
		return "next-power-cycle"
	}
	return "now"
}

// Danger is how much confirmation a change needs.
type Danger int

// Confirmation levels.
const (
	DangerNone Danger = iota
	DangerAsks        // yes/no, default No
	DangerRedBox
)

// Option is one entry of the catalog. Key is the control-port key (empty for the options that go
// through a helper script of the camera).
type Option struct {
	ID, Group, Label, Help string
	Kind                   Kind
	Values                 []string // Enum
	Min, Max               int      // Int
	Key                    string   // ctl GET/SET key
	Helper                 string   // helper script (fixed words, validated argument)
	Persisted              Persist
	Applies                Applies
	Danger                 Danger
	Needs                  string
	ReadOnly               bool
}

// Video limits (src/ak_rtsp/video_cfg).
const (
	VideoKbpsMin = 500
	VideoKbpsMax = 3000
	VideoGopMin  = 1
	VideoGopMax  = 4
)

// Catalog returns every option, in screen order.
func Catalog() []Option {
	num := func(id, group, label string, lo, hi int, p Persist, ap Applies) Option {
		return Option{ID: id, Group: group, Label: label, Kind: Int, Min: lo, Max: hi, Key: id, Persisted: p, Applies: ap}
	}
	enum := func(id, group, label string, p Persist, ap Applies, vals ...string) Option {
		return Option{ID: id, Group: group, Label: label, Kind: Enum, Values: vals, Key: id, Persisted: p, Applies: ap}
	}
	return []Option{
		enum("led.status", "LED and night", "Front LED", PersistCamera, Now, "on", "off", "auto"),
		enum("night.ir_led", "LED and night", "IR night LEDs", PersistCamera, Now, "auto", "off"),
		enum("night.mode", "LED and night", "Day/night mode", PersistRAM, Now, "auto", "day", "night"),
		num("isp.brightness", "Picture", "Brightness", -50, 50, PersistCamera, Now),
		num("isp.contrast", "Picture", "Contrast", -50, 50, PersistCamera, Now),
		num("isp.saturation", "Picture", "Saturation", -50, 50, PersistCamera, Now),
		num("isp.sharpness", "Picture", "Sharpness", -50, 50, PersistCamera, Now),
		enum("img.wb", "Picture", "White balance", PersistRAM, Now, "conf", "manual"),
		num("audio.out_volume", "Audio", "Speaker volume", 0, 100, PersistCamera, Now),
		enum("audio.out_enable", "Audio", "Speaker", PersistCamera, Now, "0", "1"),
		enum("audio.talk_enable", "Audio", "Talk (two-way)", PersistCamera, Now, "0", "1"),
		enum("audio.mic_enable", "Audio", "Microphone", PersistCamera, NextPowerCycle, "0", "1"),
		num("video.bitrate_cfg", "Video", "Bitrate (kbit/s)", VideoKbpsMin, VideoKbpsMax, PersistCamera, VideoRestart),
		enum("video.fps_cfg", "Video", "Frame rate", PersistCamera, VideoRestart, "10", "15", "20", "25"),
		num("video.gop_cfg", "Video", "Key frame interval (s)", VideoGopMin, VideoGopMax, PersistCamera, VideoRestart),
		{ID: "video.cfg_pending", Group: "Video", Label: "Settings not running yet", Kind: Int, Key: "video.cfg_pending", Persisted: PersistNone, ReadOnly: true},
		{ID: "video.bitrate", Group: "Video", Label: "Measured bitrate (kbit/s)", Kind: Int, Key: "video.bitrate", Persisted: PersistNone, ReadOnly: true},
		{ID: "video.fps_actual", Group: "Video", Label: "Measured frame rate (tenths)", Kind: Int, Key: "video.fps_actual", Persisted: PersistNone, ReadOnly: true},
		{ID: "security.rtsp_auth", Group: "Security", Label: "Stream password", Kind: Secret, Helper: "akam_rtsp_auth.sh", Persisted: PersistCamera, Applies: Now, Danger: DangerAsks},
		{ID: "net.onvif", Group: "Network", Label: "ONVIF", Kind: Action, Helper: "akam_onvif.sh", Persisted: PersistCamera, Applies: Now, Danger: DangerAsks, Needs: "security.rtsp_auth"},
		{ID: "net.mqtt", Group: "Network", Label: "MQTT (Home Assistant)", Kind: Action, Helper: "akam_mqtt.sh", Persisted: PersistCamera, Applies: Now, Danger: DangerAsks},
		{ID: "net.ntp", Group: "Network", Label: "NTP", Kind: String, Helper: "akam_ntp.sh", Persisted: PersistCamera, Applies: Now},
		{ID: "net.hostname", Group: "Network", Label: "Host name", Kind: String, Helper: "akam_hostname.sh", Persisted: PersistCamera, Applies: NextPowerCycle},
		{ID: "net.ssid", Group: "Network", Label: "Wi-Fi network", Kind: String, Key: "net.ssid", Persisted: PersistNone, ReadOnly: true},
		{ID: "net.wifi_rssi", Group: "Network", Label: "Wi-Fi signal", Kind: Int, Key: "net.wifi_rssi", Persisted: PersistNone, ReadOnly: true},
		{ID: "debug", Group: "Advanced", Label: "Debug mode", Kind: Action, Helper: "akam_debug.sh", Persisted: PersistRAM, Applies: Now, Danger: DangerAsks},
	}
}

// Find returns the option with this ID.
func Find(id string) (Option, bool) {
	for _, o := range Catalog() {
		if o.ID == id {
			return o, true
		}
	}
	return Option{}, false
}

// ---------------------------------------------------------------- validators

var (
	wholeRe = regexp.MustCompile(`^-?[0-9]{1,6}$`)
	ntpRe   = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$`)
)

// ValidNTPServer is the grammar of akam_ntp.sh: a host name or an IPv4 address, letters, digits,
// inner hyphens and dots, at most 253 characters.
func ValidNTPServer(s string) bool {
	return s != "" && len(s) <= 253 && ntpRe.MatchString(s)
}

// ValidNTPServers checks a "set" list: 1 to 3 valid servers.
func ValidNTPServers(list []string) error {
	if len(list) < 1 || len(list) > 3 {
		return fmt.Errorf("ntp set needs 1 to 3 servers")
	}
	for _, s := range list {
		if !ValidNTPServer(s) {
			return fmt.Errorf("'%s' is not a host name or an IPv4 address", s)
		}
	}
	return nil
}

// Validate checks a user-typed value for an option with a control key and returns it in its
// normalised form (no leading zeros). Nothing is sent when it fails.
func Validate(id, value string) (string, error) {
	o, ok := Find(id)
	if !ok || o.Key == "" || o.ReadOnly {
		return "", fmt.Errorf("%s cannot be set", id)
	}
	switch o.Kind {
	case Int:
		if !wholeRe.MatchString(value) {
			return "", fmt.Errorf("%s needs a whole number from %d to %d", o.Label, o.Min, o.Max)
		}
		n, _ := strconv.Atoi(value)
		if n < o.Min || n > o.Max {
			return "", fmt.Errorf("%s needs a whole number from %d to %d", o.Label, o.Min, o.Max)
		}
		return strconv.Itoa(n), nil
	case Enum, Bool:
		for _, v := range o.Values {
			if v == value {
				return value, nil
			}
		}
		return "", fmt.Errorf("%s needs one of %s", o.Label, strings.Join(o.Values, ", "))
	}
	return "", fmt.Errorf("%s cannot be set here", id)
}

// SafeValue reports whether a camera-supplied value may be kept: 1 to 64 characters among letters,
// digits and . _ : / + - (no space, quote, backslash, dollar, control character or non-ASCII byte).
func SafeValue(s string) bool {
	if s == "" || len(s) > 64 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		case c == '.', c == '_', c == ':', c == '/', c == '+', c == '-':
		default:
			return false
		}
	}
	return true
}

// SafeNumber reports whether a camera-supplied value is a plain whole number.
func SafeNumber(s string) bool { return wholeRe.MatchString(s) }
