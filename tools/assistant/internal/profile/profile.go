// Package profile is the saved configuration of a camera on this computer: a plain key=value file
// with a version line (the settings), and a second file of the same shape for the secrets (stream
// password, MQTT password, Wi-Fi). It has no UI and no network code: the cli package reads a camera,
// builds a File, and applies a File to a camera through the same paths as the option screens.
//
// Rules of the format (strict on purpose, the files are read back and sent to a camera):
//   - the first line that is not a comment or blank is akam_config_version=N (N is 1 today); a file
//     of a newer version is refused, never half read;
//   - one key=value per line, LF only, the value is everything after the first "=" (no quoting, no
//     trimming), no control character, at most 512 bytes per line and 64 KiB per file;
//   - a key appears once; a known key with a value that does not validate is an error (nothing is
//     applied from a file that does not parse);
//   - an unknown key (a newer akam wrote it) is kept as it is when the file is written again and is
//     ignored everywhere else.
package profile

import (
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
)

// Version is the format version this build reads and writes.
const Version = 1

// Kind says which of the two files a File is.
type Kind int

// The two kinds of file.
const (
	Settings Kind = iota // global.conf, camera.conf: no secret
	Secrets              // global.secrets, camera.secrets: passwords and Wi-Fi
)

func (k Kind) String() string {
	if k == Secrets {
		return "secrets"
	}
	return "settings"
}

const versionKey = "akam_config_version"

const (
	maxLine = 512
	maxFile = 64 << 10
)

// Keys of the settings file that are not control-port options (those use the option IDs of camopt:
// video.bitrate_cfg, isp.brightness, led.status, ...).
const (
	OnvifEnabled  = "onvif.enabled" // 0 | 1
	OnvifPort     = "onvif.port"    // 1..65535
	NTPEnabled    = "ntp.enabled"   // 0 | 1
	NTPServers    = "ntp.servers"   // 1 to 3 host names or IPv4 addresses, separated by commas
	MQTTEnabled   = "mqtt.enabled"
	MQTTHost      = "mqtt.host"
	MQTTPort      = "mqtt.port"
	MQTTUser      = "mqtt.user"
	MQTTBaseTopic = "mqtt.base_topic"
	MQTTClientID  = "mqtt.client_id"
	MQTTDiscovery = "mqtt.discovery" // on | off
	MQTTPrefix    = "mqtt.discovery_prefix"
	MQTTInterval  = "mqtt.interval" // seconds, 10..300
	AuthEnabled   = "auth.enabled"  // the video stream password: 0 | 1
	AuthUser      = "auth.user"
	KeyKind       = "key.kind" // shared | own | custom
	KeyPath       = "key.path" // the .pub file of a custom key
	SavedFrom     = "saved.from"
	SavedAt       = "saved.at"
)

// Keys of the secrets file.
const (
	AuthPassword = "auth.password"
	MQTTPassword = "mqtt.password"
	WifiSSID     = "wifi.ssid"
	WifiPassword = "wifi.password"
	// A saved password belongs to the setting it was typed for: the video password to its user, the MQTT
	// password to its broker and user. It is used only while the settings still say the same (see Bound*).
	AuthPasswordUser = "auth.password_user"
	MQTTPasswordHost = "mqtt.password_host"
	MQTTPasswordUser = "mqtt.password_user"
)

var (
	hostRe   = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9.-]{0,62}$`)
	userRe   = regexp.MustCompile(`^[A-Za-z0-9._@+-]{1,32}$`)
	idRe     = regexp.MustCompile(`^[A-Za-z0-9_-]{1,32}$`)
	topicRe  = regexp.MustCompile(`^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*$`)
	extraRe  = regexp.MustCompile(`^[a-z][a-z0-9_.]*$`)
	metaRe   = regexp.MustCompile(`^[A-Za-z0-9._:@+ -]{0,64}$`)
	digitsRe = regexp.MustCompile(`^[0-9]{1,5}$`)
)

// HostValid is the grammar of akam_mqtt.sh for a broker host.
func HostValid(s string) bool { return hostRe.MatchString(s) }

// UserValid is the grammar of the stream and MQTT user names.
func UserValid(s string) bool { return userRe.MatchString(s) }

// TopicValid is the grammar of an MQTT topic ("a/b", no leading, trailing or double slash).
func TopicValid(s string, max int) bool { return len(s) <= max && topicRe.MatchString(s) }

// IDValid is the grammar of an MQTT client id.
func IDValid(s string) bool { return idRe.MatchString(s) }

// canon says s is a whole number written the one way (no sign, no leading zero).
func canon(s string) (int, bool) {
	n, err := strconv.Atoi(s)
	return n, err == nil && strconv.Itoa(n) == s
}

func port(s string) bool {
	n, ok := canon(s)
	return ok && n >= 1 && n <= 65535
}

// onvifPort refuses the ports the camera uses for something else (FTP, telnet, SSH, RTSP, control port).
func onvifPort(s string) bool {
	n, ok := canon(s)
	return port(s) && ok && !(n >= 21 && n <= 24) && n != 554 && n != 8091
}

// validators of the settings keys that are not option IDs of camopt.
var settingCheck = map[string]func(string) error{
	OnvifEnabled: oneOf("0", "1"),
	OnvifPort: func(v string) error {
		return need(onvifPort(v), "a port from 1 to 65535 other than 21 to 24, 554 and 8091")
	},
	NTPEnabled:  oneOf("0", "1"),
	NTPServers:  func(v string) error { return camopt.ValidNTPServers(strings.Split(v, ",")) },
	MQTTEnabled: oneOf("0", "1"),
	MQTTHost:    func(v string) error { return need(HostValid(v), "a host name or an IPv4 address") },
	MQTTPort:    func(v string) error { return need(port(v), "a port from 1 to 65535") },
	MQTTUser: func(v string) error {
		return need(v == "" || (v != "-" && UserValid(v)), "empty or 1 to 32 of letters, digits . _ @ + -")
	},
	MQTTBaseTopic: func(v string) error { return need(TopicValid(v, 64), "a topic of letters, digits _ - and /") },
	MQTTClientID:  func(v string) error { return need(idRe.MatchString(v), "1 to 32 of letters, digits _ -") },
	MQTTDiscovery: oneOf("on", "off"),
	MQTTPrefix:    func(v string) error { return need(TopicValid(v, 32), "a topic of letters, digits _ - and /") },
	MQTTInterval: func(v string) error {
		n, ok := canon(v)
		return need(ok && n >= 10 && n <= 300, "seconds from 10 to 300")
	},
	AuthEnabled: oneOf("0", "1"),
	AuthUser:    func(v string) error { return need(UserValid(v), "1 to 32 of letters, digits . _ @ + -") },
	KeyKind:     oneOf("shared", "own", "custom"),
	KeyPath:     func(v string) error { return need(v != "", "a path") },
	SavedFrom:   func(v string) error { return need(metaRe.MatchString(v), "a short name") },
	SavedAt:     func(v string) error { return need(metaRe.MatchString(v), "a time stamp") },
}

var secretCheck = map[string]func(string) error{
	AuthPassword: func(v string) error { return need(len(v) >= 8 && len(v) <= 128, "8 to 128 printable characters") },
	MQTTPassword: func(v string) error { return need(v != "" && len(v) <= 128, "1 to 128 printable characters") },
	WifiSSID:     func(v string) error { return need(v != "" && len(v) <= 32, "1 to 32 printable characters") },
	WifiPassword: func(v string) error { return need(v != "" && len(v) <= 128, "1 to 128 printable characters") },

	AuthPasswordUser: func(v string) error { return need(UserValid(v), "a user name") },
	MQTTPasswordHost: func(v string) error { return need(HostValid(v), "a host name or an IPv4 address") },
	MQTTPasswordUser: func(v string) error { return need(UserValid(v), "a user name") },
}

// BoundAuth returns the saved video password when it was saved for this user ("" otherwise).
func BoundAuth(sec *File, user string) string {
	if sec == nil || user == "" || sec.Get(AuthPasswordUser) != user {
		return ""
	}
	return sec.Get(AuthPassword)
}

// BoundMQTT returns the saved MQTT password when it was saved for this broker and user.
func BoundMQTT(sec *File, host, user string) string {
	if sec == nil || user == "" || sec.Get(MQTTPasswordHost) != host || sec.Get(MQTTPasswordUser) != user {
		return ""
	}
	return sec.Get(MQTTPassword)
}

func need(ok bool, what string) error {
	if ok {
		return nil
	}
	return errors.New("needs " + what)
}

func oneOf(vals ...string) func(string) error {
	return func(v string) error {
		for _, w := range vals {
			if v == w {
				return nil
			}
		}
		return fmt.Errorf("needs one of %s", strings.Join(vals, ", "))
	}
}

func hasControl(s string) bool {
	for _, r := range s {
		if r < 0x20 || (r >= 0x7f && r < 0xa0) {
			return true
		}
	}
	return false
}

// optionKey says whether k is the ID of an option of the control port that a screen edits and the camera
// keeps. The options that go back to their default at a reboot (night.mode, img.wb) are not part of a
// config: a saved value would look applied and be gone at the next power cycle.
func optionKey(k string) (camopt.Option, bool) {
	o, ok := camopt.Find(k)
	return o, ok && o.Key != "" && !o.ReadOnly && o.Persisted == camopt.PersistCamera
}

// KnownSetting reports whether k is a key of the settings file this build understands.
func KnownSetting(k string) bool {
	if _, ok := settingCheck[k]; ok {
		return true
	}
	_, ok := optionKey(k)
	return ok
}

// IsSecret reports whether k is a key of the secrets file.
func IsSecret(k string) bool { _, ok := secretCheck[k]; return ok }

// OptionIDs are the control-port options a settings file may hold, in screen order.
func OptionIDs() []string {
	var ids []string
	for _, o := range camopt.Catalog() {
		if _, ok := optionKey(o.ID); ok {
			ids = append(ids, o.ID)
		}
	}
	return ids
}

// File is one parsed file: the values of the keys this build knows, and the unknown keys.
type File struct {
	Kind  Kind
	Vals  map[string]string
	Extra map[string]string // unknown keys of a newer akam: written back as they are, never used
}

// New is an empty file.
func New(k Kind) *File { return &File{Kind: k, Vals: map[string]string{}, Extra: map[string]string{}} }

// Get returns the value of key k ("" when absent).
func (f *File) Get(k string) string { return f.Vals[k] }

// Has reports whether key k is set (a value may be empty, for example mqtt.user).
func (f *File) Has(k string) bool { _, ok := f.Vals[k]; return ok }

// Set checks and stores one value; it refuses a key that does not belong to this kind of file and a
// value that does not validate.
func (f *File) Set(k, v string) error {
	if err := f.check(k, v); err != nil {
		return err
	}
	f.Vals[k] = v
	return nil
}

// Unset removes key k.
func (f *File) Unset(k string) { delete(f.Vals, k) }

// Empty reports whether the file holds no known value (unknown keys do not count).
func (f *File) Empty() bool { return len(f.Vals) == 0 }

// Clone is a deep copy.
func (f *File) Clone() *File {
	n := New(f.Kind)
	for k, v := range f.Vals {
		n.Vals[k] = v
	}
	for k, v := range f.Extra {
		n.Extra[k] = v
	}
	return n
}

func (f *File) check(k, v string) error {
	if len(k)+1+len(v) > maxLine {
		return fmt.Errorf("%s: longer than %d bytes", k, maxLine)
	}
	if strings.ContainsAny(v, "\r\n") || hasControl(v) {
		return fmt.Errorf("%s: a value is one line without control characters", k)
	}
	if f.Kind == Secrets {
		fn, ok := secretCheck[k]
		if !ok {
			return fmt.Errorf("%s is not a secret key", k)
		}
		if err := fn(v); err != nil {
			return fmt.Errorf("%s: %w", k, err)
		}
		return nil
	}
	if o, ok := optionKey(k); ok {
		n, err := camopt.Validate(o.ID, v)
		if err == nil && n != v {
			return fmt.Errorf("%s: write the number as %s", o.Label, n)
		}
		return err
	}
	fn, ok := settingCheck[k]
	if !ok {
		return fmt.Errorf("%s is not a settings key", k)
	}
	if err := fn(v); err != nil {
		return fmt.Errorf("%s: %w", k, err)
	}
	return nil
}

// ErrNewer is returned for a file written by a newer akam.
var ErrNewer = errors.New("written by a newer akam")

// Parse reads a file of kind k. Every problem is an error that names the line.
func Parse(data []byte, k Kind) (*File, error) {
	if len(data) > maxFile {
		return nil, fmt.Errorf("the file is larger than %d KiB", maxFile>>10)
	}
	f := New(k)
	seen := map[string]bool{}
	version := 0
	text := strings.TrimSuffix(string(data), "\n")
	for i, line := range strings.Split(text, "\n") {
		n := i + 1
		if strings.Contains(line, "\r") {
			return nil, fmt.Errorf("line %d: CR found (the file uses LF line ends)", n)
		}
		if len(line) > maxLine {
			return nil, fmt.Errorf("line %d: longer than %d bytes", n, maxLine)
		}
		if line == "" || line[0] == '#' {
			continue
		}
		if hasControl(line) {
			return nil, fmt.Errorf("line %d: control character", n)
		}
		key, val, ok := strings.Cut(line, "=")
		if !ok || key == "" {
			return nil, fmt.Errorf("line %d: not key=value", n)
		}
		if seen[key] {
			return nil, fmt.Errorf("line %d: %s appears twice", n, key)
		}
		seen[key] = true
		if key == versionKey {
			v, err := strconv.Atoi(val)
			if err != nil || v < 1 || !digitsRe.MatchString(val) {
				return nil, fmt.Errorf("line %d: %s needs a whole number", n, versionKey)
			}
			if v > Version {
				return nil, fmt.Errorf("version %d: %w (this build reads version %d)", v, ErrNewer, Version)
			}
			version = v
			continue
		}
		if version == 0 {
			return nil, fmt.Errorf("line %d: the first line must be %s=%d", n, versionKey, Version)
		}
		known := KnownSetting(key)
		if k == Secrets {
			known = IsSecret(key)
			if KnownSetting(key) {
				return nil, fmt.Errorf("line %d: %s is a setting: it does not belong in a secrets file", n, key)
			}
		} else if IsSecret(key) {
			return nil, fmt.Errorf("line %d: %s is a secret: it does not belong in a settings file", n, key)
		}
		if !known {
			if !extraRe.MatchString(key) {
				return nil, fmt.Errorf("line %d: bad key", n)
			}
			f.Extra[key] = val
			continue
		}
		if err := f.check(key, val); err != nil {
			return nil, fmt.Errorf("line %d: %w", n, err)
		}
		f.Vals[key] = val
	}
	if version == 0 {
		return nil, fmt.Errorf("no %s line", versionKey)
	}
	return f, nil
}

// order is the canonical order of the keys when a file is written (the option IDs first, in screen
// order, then the other settings, then the secrets); the unknown keys follow, sorted.
func order() []string {
	ks := append(SettingKeys(), SavedFrom, SavedAt)
	return append(ks, AuthPassword, AuthPasswordUser, MQTTPassword, MQTTPasswordHost, MQTTPasswordUser, WifiSSID, WifiPassword)
}

// SettingKeys are the keys of the settings a config holds, in the order a screen shows them (the option IDs
// first, then ONVIF, time sync, MQTT, the video password, the SSH key); the bookkeeping keys are not among them.
func SettingKeys() []string {
	return append(OptionIDs(), OnvifEnabled, OnvifPort, NTPEnabled, NTPServers,
		MQTTEnabled, MQTTHost, MQTTPort, MQTTUser, MQTTBaseTopic, MQTTClientID, MQTTDiscovery, MQTTPrefix, MQTTInterval,
		AuthEnabled, AuthUser, KeyKind, KeyPath)
}

// Marshal writes the file: the version line, then the known keys in a fixed order, then the unknown ones.
func (f *File) Marshal() []byte {
	var b strings.Builder
	if f.Kind == Secrets {
		b.WriteString("# Akam secrets: stream password, MQTT password, Wi-Fi. Private file: do not share it.\n")
	} else {
		b.WriteString("# Akam camera settings (no password in this file).\n")
	}
	fmt.Fprintf(&b, "%s=%d\n", versionKey, Version)
	for _, k := range order() {
		if v, ok := f.Vals[k]; ok {
			fmt.Fprintf(&b, "%s=%s\n", k, v)
		}
	}
	var ex []string
	for k := range f.Extra {
		ex = append(ex, k)
	}
	sort.Strings(ex)
	for _, k := range ex {
		fmt.Fprintf(&b, "%s=%s\n", k, f.Extra[k])
	}
	return []byte(b.String())
}

// Groups are the names `--only` and the diff use.
var Groups = []string{"led", "picture", "video", "audio", "onvif", "ntp", "mqtt", "auth", "key"}

// GroupOf is the group of a settings key ("" for the bookkeeping keys).
func GroupOf(k string) string {
	if o, ok := optionKey(k); ok {
		switch o.Group {
		case "LED and night":
			return "led"
		case "Picture":
			return "picture"
		case "Video":
			return "video"
		case "Audio":
			return "audio"
		}
		return ""
	}
	g, _, ok := strings.Cut(k, ".")
	if !ok {
		return ""
	}
	switch g {
	case "onvif", "ntp", "mqtt", "auth", "key":
		return g
	}
	return ""
}

// ParseOnly reads the value of --only: a comma list of group names (nil: every group).
func ParseOnly(s string) (map[string]bool, error) {
	if s == "" {
		return nil, nil
	}
	out := map[string]bool{}
	for _, g := range strings.Split(s, ",") {
		g = strings.TrimSpace(g)
		ok := false
		for _, w := range Groups {
			if w == g {
				ok = true
			}
		}
		if !ok {
			return nil, fmt.Errorf("--only: '%s' is not a group (%s)", g, strings.Join(Groups, ", "))
		}
		out[g] = true
	}
	return out, nil
}
