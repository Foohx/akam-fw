package cli

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// askSetup2K is the install-time setup of the optional services of a 2K, asked before the first-install card is written. Every question
// defaults to No (every service is off by default); the answers become the same files the camera reads later and the options flow edits
// (appfs.Setup2K). The two passwords are asked without echo, hashed or written to the card (the stream password only as its Digest hash),
// and never shown. nil means nothing was chosen.
func (a *App) askSetup2K() (*appfs.Setup2K, error) {
	yes := func(q string) bool {
		ok, err := a.UI.AskYN(q)
		return err == nil && ok
	}
	if !yes("Set up optional services now (microphone, stream password, ONVIF, MQTT / Home Assistant, time sync, speaker, picture)? All are off by default and can be changed later with " + Prog + ".") {
		return nil, nil
	}
	s := &appfs.Setup2K{}
	value := func(key, prompt, def string) (string, bool) {
		for {
			v, err := a.UI.Prompt(prompt, def)
			if err != nil {
				return "", false
			}
			n, verr := camopt.Validate2K(key, strings.TrimSpace(v))
			if verr != nil {
				a.UI.Say("  " + verr.Error() + ".")
				continue
			}
			return n, true
		}
	}
	secret := func(key, prompt string) (string, bool) {
		for {
			v, err := a.UI.Secret(prompt)
			if err != nil {
				return "", false
			}
			if v == "" && key == "mqtt.password" {
				return "", true
			}
			if _, verr := camopt.Validate2K(key, v); verr != nil {
				a.UI.Say("  " + verr.Error() + ".")
				continue
			}
			a.Secrets.Add(v)
			return v, true
		}
	}
	if yes("  Microphone (an audio track in the video streams; off by default)?") {
		s.Mic = true
	}
	if yes("  A stream password (viewers and ONVIF must log in; only its hash is stored)?") {
		u, ok := value("rtsp.user", "  Stream user name:", "")
		if !ok || u == "" {
			return nil, a.UI.Fail("no stream user name.", "nothing was written.")
		}
		p, ok := secret("rtsp.password", "  Stream password (not shown, at least 8 characters):")
		if !ok {
			return nil, a.UI.Fail("no input.", "nothing was written.")
		}
		again, err := a.UI.Secret("  Type it again:")
		if err != nil || again != p {
			return nil, a.UI.Fail("the two passwords differ.", "nothing was written.")
		}
		s.AuthUser, s.AuthPass = u, p
	}
	if s.AuthUser != "" && yes("  ONVIF (NVR and app discovery, pan and tilt; uses the stream password)?") {
		s.ONVIF = true
		if p, ok := value("onvif.port", "  ONVIF port [8000]:", "8000"); ok && p != "8000" {
			n, _ := strconv.Atoi(p)
			s.ONVIFPort = n
		}
	}
	if yes("  MQTT (publish the state to a broker such as Home Assistant)?") {
		a.UI.Say("  The camera connects out to the broker over plain TCP: the broker password travels in clear on your network.")
		h, ok := value("mqtt.host", "  Broker address (host name or IPv4 address):", "")
		if !ok || h == "" {
			return nil, a.UI.Fail("no broker address.", "nothing was written.")
		}
		port, _ := value("mqtt.port", "  Broker port [1883]:", "1883")
		user, _ := value("mqtt.user", "  Broker user name (empty for none):", "")
		s.MQTTHost, s.MQTTUser = h, user
		if n, _ := strconv.Atoi(port); n != 1883 {
			s.MQTTPort = n
		}
		if user != "" {
			s.MQTTPass, _ = secret("mqtt.password", "  Broker password (not shown, empty for none):")
		}
	}
	if yes("  Time sync (the camera has no clock; it asks an NTP server at start-up)?") {
		v, ok := value("ntp.servers", "  NTP servers, 1 to 3 separated by spaces:", "pool.ntp.org")
		if !ok || v == "" {
			return nil, a.UI.Fail("no NTP server.", "nothing was written.")
		}
		s.NTP = strings.Fields(v)
	}
	// The speaker, two-way audio and picture settings the 1080p also has (kept by the camera in /config/akam/settings). An empty answer keeps the
	// camera's own value; closed input skips the rest (like every question above). Not verified on a camera yet: the speaker stays off unless chosen.
	s.Settings = map[string]string{}
	if yes("  Speaker output (off by default on the 2K; not verified on a camera yet)?") {
		s.Settings["audio.out_enable"] = "1"
		if v, ok := a.askNumber2K("audio.out_volume", "Speaker volume"); ok {
			s.Settings["audio.out_volume"] = v
		}
		if yes("  Two-way audio (talk through the stream; needs the stream password)?") {
			s.Settings["audio.talk_enable"] = "1"
		} else {
			s.Settings["audio.talk_enable"] = "0"
		}
	}
	for _, k := range []struct{ key, label string }{{"isp.saturation", "Picture saturation"}, {"isp.denoise", "Picture denoise"}, {"isp.nr2", "Picture spatial noise reduction"}, {"isp.wdr", "Backlight (WDR) strength, 90 is the stock"}} {
		if v, ok := a.askNumber2K(k.key, k.label); ok {
			s.Settings[k.key] = v
		}
	}
	if len(s.Settings) == 0 {
		s.Settings = nil
	}
	files, err := s.Files()
	if err != nil {
		return nil, a.UI.Fail(ui.Sanitize(err.Error())+".", "nothing was written.")
	}
	if len(files) == 0 {
		return nil, nil
	}
	a.UI.Say("  The card will also carry: " + strings.Join(appfs.SetupNames(files), ", ") + " (passwords are not shown here; change any of it later with " + Prog + ").")
	return s, nil
}

// askNumber2K asks one whole-number option of the options catalogue: an empty answer, or closed input, keeps the camera's own value (false);
// a refused value is explained and asked again.
func (a *App) askNumber2K(key, label string) (string, bool) {
	o, _ := camopt.Find2K(key)
	for {
		v, err := a.UI.Prompt(fmt.Sprintf("  %s (%d to %d, empty keeps the camera's own):", label, o.Min, o.Max), "")
		if err != nil {
			return "", false
		}
		v = strings.TrimSpace(v)
		if v == "" {
			return "", false
		}
		n, verr := camopt.Validate2K(key, v)
		if verr != nil {
			a.UI.Say("  " + ui.Sanitize(verr.Error()) + ".")
			continue
		}
		return n, true
	}
}
