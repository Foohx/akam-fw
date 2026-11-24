package appfs

import (
	"crypto/md5" //nolint:gosec // the Digest HA1 of the stream password is MD5("user:akam:password") by RFC 2617, not a security choice of ours
	"encoding/hex"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
)

// Setup2K are the optional services chosen at install time, before the first boot. The zero value changes nothing: every service stays
// at its default (off). They are written as the same files the camera reads later (and the signed API edits afterwards): akam.conf
// (microphone, ONVIF), mqtt + mqtt_pass, rtsp.conf (the Digest HA1 only: the password itself is never stored) and ntp.
type Setup2K struct {
	Mic       bool // audio=1: an L16 microphone track on both streams
	ONVIF     bool // onvif=1 (needs the stream password)
	ONVIFPort int  // 0 = the default 8000
	MQTTHost  string
	MQTTPort  int // 0 = 1883
	MQTTUser  string
	MQTTPass  string // needs a user
	AuthUser  string // the stream login: user and password together
	AuthPass  string
	NTP       []string
	// Settings are registry settings kept by the camera in /config/akam/settings (the speaker, two-way audio and picture ones asked at install
	// time): key to typed value, validated by camopt.SettingsText2K (only options the camera keeps across a restart). Written as the file "settings".
	Settings map[string]string
}

// SetupFileNames are the optional files of the card (and of /config/akam) that Setup2K may produce.
var SetupFileNames = []string{"akam.conf", "mqtt", "mqtt_pass", "rtsp.conf", "ntp", "settings"}

func ctl(s string) bool {
	for _, r := range s {
		if r < 0x20 || r == 0x7f {
			return true
		}
	}
	return false
}

// Files validates the choices with the same grammars as the camera (camopt, mirrored from the firmware) and returns the files, relative to
// the akam directory of the card. Nothing is returned for an unset service.
func (s *Setup2K) Files() (map[string][]byte, error) {
	out := map[string][]byte{}
	if s == nil {
		return out, nil
	}
	var conf []string
	if s.Mic {
		conf = append(conf, "audio=1")
	}
	if s.ONVIF {
		if s.AuthUser == "" {
			return nil, errors.New("ONVIF logs in with the stream credentials: set a stream user and password too")
		}
		conf = append(conf, "onvif=1")
		if s.ONVIFPort != 0 {
			if s.ONVIFPort < 1024 || s.ONVIFPort > 65535 {
				return nil, errors.New("the ONVIF port must be 1024 to 65535")
			}
			conf = append(conf, fmt.Sprintf("onvif_port=%d", s.ONVIFPort))
		}
	} else if s.ONVIFPort != 0 {
		return nil, errors.New("an ONVIF port without ONVIF")
	}
	if len(conf) > 0 {
		out["akam.conf"] = []byte("# written by the assistant at install time; edit it over SSH or with the akam program\n" + strings.Join(conf, "\n") + "\n")
	}
	if s.MQTTHost != "" {
		if !camopt.ValidServiceHost(s.MQTTHost) {
			return nil, errors.New("the MQTT broker must be a host name or an IPv4 address (letters, digits, '.' and '-', at most 63)")
		}
		port := s.MQTTPort
		if port == 0 {
			port = 1883
		}
		if port < 1 || port > 65535 {
			return nil, errors.New("the MQTT port must be 1 to 65535")
		}
		txt := fmt.Sprintf("enabled=1\nhost=%s\nport=%d\n", s.MQTTHost, port)
		if s.MQTTUser != "" {
			if !camopt.ValidServiceUser(s.MQTTUser) {
				return nil, errors.New("the MQTT user must be 1 to 32 of A-Z a-z 0-9 . _ @ + -")
			}
			txt += "user=" + s.MQTTUser + "\n"
		}
		out["mqtt"] = []byte(txt)
		if s.MQTTPass != "" {
			if s.MQTTUser == "" {
				return nil, errors.New("an MQTT password needs an MQTT user")
			}
			if len(s.MQTTPass) > 128 || ctl(s.MQTTPass) {
				return nil, errors.New("the MQTT password must be at most 128 printable characters")
			}
			out["mqtt_pass"] = []byte(s.MQTTPass + "\n")
		}
	} else if s.MQTTUser != "" || s.MQTTPass != "" || s.MQTTPort != 0 {
		return nil, errors.New("MQTT settings without a broker host")
	}
	if s.AuthUser != "" || s.AuthPass != "" {
		if !camopt.ValidServiceUser(s.AuthUser) {
			return nil, errors.New("the stream user must be 1 to 32 of A-Z a-z 0-9 . _ @ + -")
		}
		if len(s.AuthPass) < 8 || len(s.AuthPass) > 128 || ctl(s.AuthPass) {
			return nil, errors.New("the stream password must be 8 to 128 printable characters")
		}
		h := md5.Sum([]byte(s.AuthUser + ":akam:" + s.AuthPass)) //nolint:gosec
		out["rtsp.conf"] = []byte(s.AuthUser + ":akam:" + hex.EncodeToString(h[:]) + "\n")
	}
	if len(s.NTP) > 0 {
		if err := camopt.ValidNTPServers(s.NTP); err != nil {
			return nil, err
		}
		var b strings.Builder
		for _, n := range s.NTP {
			b.WriteString("server=" + n + "\n")
		}
		out["ntp"] = []byte(b.String())
	}
	if len(s.Settings) > 0 {
		txt, err := camopt.SettingsText2K(s.Settings)
		if err != nil {
			return nil, err
		}
		out["settings"] = []byte(txt)
	}
	return out, nil
}

// Names returns the sorted file names of a Files result (for the summary shown before the card is written; never a content).
func SetupNames(files map[string][]byte) []string {
	var n []string
	for k := range files {
		n = append(n, k)
	}
	sort.Strings(n)
	return n
}
