package cli

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The service commands of a 2K (audio, mqtt, onvif, ntp, rtsp-auth): the same words as on the 1080p, served through the signed
// API (the user-file rows of the camera, cfgreg.h) instead of helper scripts over SSH. They need an ADMIN key. The two passwords are
// write-only rows: asked without echo, sent once in the PATCH body, never shown, never saved by the assistant, never read back.

// services2KCommands are the commands this file serves on a 2K.
var services2KCommands = map[string]bool{"audio": true, "mqtt": true, "onvif": true, "ntp": true, "rtsp-auth": true}

func (a *App) dispatch2KServices(cmd string, args []string) error {
	action := ""
	if len(args) > 0 {
		action = args[0]
	}
	switch cmd {
	case "audio":
		return a.cmdAudio2K(args)
	case "mqtt":
		switch action {
		case "", "status":
			return a.run2KOpts("MQTT: status", nil, []string{"mqtt.enabled", "mqtt.host", "mqtt.port", "mqtt.user", "mqtt.password"})
		case "set":
			return a.mqttSet2K()
		case "disable":
			return a.run2KOpts("MQTT: disable", []camopt.Change{{ID: "mqtt.enabled", Value: "0"}}, []string{"mqtt.enabled"})
		case "forget":
			return a.mqttForget2K()
		case "test":
			return usageErr("akam: mqtt test is not available on the 2K (the camera has no broker test); watch the broker or the camera log instead")
		}
		return usageErr(fmt.Sprintf("akam: mqtt needs status, set, disable or forget on a 2K (see %s help)", Prog))
	case "onvif":
		switch action {
		case "", "status":
			return a.run2KOpts("ONVIF: status", nil, []string{"onvif.enabled", "onvif.port", "rtsp.user"})
		case "disable":
			return a.run2KOpts("ONVIF: disable", []camopt.Change{{ID: "onvif.enabled", Value: "0"}}, []string{"onvif.enabled"})
		case "enable":
			ch := []camopt.Change{{ID: "onvif.enabled", Value: "1"}}
			if len(args) > 2 {
				return usageErr(fmt.Sprintf("akam: onvif enable takes at most one PORT (see %s help)", Prog))
			}
			if len(args) == 2 {
				v, err := camopt.Validate2K("onvif.port", args[1])
				if err != nil {
					return usageErr("akam: onvif enable: " + err.Error() + " (the 2K ONVIF port is 1024 to 65535)")
				}
				ch = append(ch, camopt.Change{ID: "onvif.port", Value: v})
			}
			return a.run2KOpts("ONVIF: enable", ch, []string{"onvif.enabled", "onvif.port"})
		}
		return usageErr(fmt.Sprintf("akam: onvif needs enable [PORT], disable or status (see %s help)", Prog))
	case "ntp":
		switch action {
		case "", "status":
			return a.run2KOpts("Time sync: status", nil, []string{"ntp.servers"})
		case "off":
			if len(args) > 1 {
				return audioUsage("ntp off takes no argument")
			}
			return a.run2KOpts("Time sync: off", []camopt.Change{{ID: "ntp.servers", Value: ""}}, []string{"ntp.servers"})
		case "set":
			if len(args) < 2 || len(args) > 4 {
				return audioUsage(fmt.Sprintf("ntp set needs 1 to 3 servers, for example: %s ntp set ntp.example.net", Prog))
			}
			v, err := camopt.Validate2K("ntp.servers", strings.Join(args[1:], " "))
			if err != nil {
				return audioUsage("ntp set: " + err.Error())
			}
			return a.run2KOpts("Time sync: set", []camopt.Change{{ID: "ntp.servers", Value: v}}, []string{"ntp.servers"})
		}
		return audioUsage("ntp needs status, set SERVER... or off")
	case "rtsp-auth":
		switch action {
		case "", "status":
			return a.run2KOpts("Stream password: status", nil, []string{"rtsp.user", "rtsp.password"})
		case "disable":
			return a.run2KOpts("Stream password: disable", []camopt.Change{{ID: "rtsp.user", Value: ""}}, []string{"rtsp.user"})
		case "set":
			return a.rtspAuthSet2K()
		}
		return usageErr(fmt.Sprintf("akam: rtsp-auth needs set, disable or status (see %s help)", Prog))
	}
	return usageErr("akam: " + cmd + " is not available on the 2K")
}

// cmdAudio2K is `audio [show | volume N | beep | on | off | talk on|off | mic on|off]` on a 2K: the settings go through the signed API
// (audio.out_volume, audio.out_enable, audio.talk_enable, audio.mic_enable), the beep through POST /actions/audio.beep.
func (a *App) cmdAudio2K(args []string) error {
	show := []string{"audio.out_enable", "audio.out_volume", "audio.talk_enable", "audio.mic_enable"}
	onoff := map[string]string{"on": "1", "off": "0"}
	var changes []camopt.Change
	beep := false
	if len(args) > 0 {
		switch args[0] {
		case "show":
			if len(args) != 1 {
				return audioUsage("audio show takes no more words")
			}
		case "beep":
			if len(args) != 1 {
				return audioUsage("audio beep takes no more words")
			}
			beep = true
		case "on", "off":
			if len(args) != 1 {
				return audioUsage("audio " + args[0] + " takes no more words")
			}
			changes = []camopt.Change{{ID: "audio.out_enable", Value: onoff[args[0]]}}
		case "volume":
			if len(args) != 2 || !digitsRe.MatchString(args[1]) {
				return audioUsage("audio volume needs a number from 0 to 100")
			}
			v, err := camopt.Validate2K("audio.out_volume", args[1])
			if err != nil {
				return audioUsage("audio volume needs a number from 0 to 100")
			}
			changes = []camopt.Change{{ID: "audio.out_volume", Value: v}}
		case "talk", "mic":
			if len(args) != 2 || onoff[args[1]] == "" {
				return audioUsage("audio " + args[0] + " needs on or off")
			}
			changes = []camopt.Change{{ID: "audio." + args[0] + "_enable", Value: onoff[args[1]]}}
		default:
			return audioUsage(fmt.Sprintf("audio: unknown word '%s'", args[0]))
		}
	}
	if beep {
		return a.beep2K()
	}
	return a.run2KOpts("Camera speaker and microphone", changes, show)
}

// beep2K plays the short beep of the speaker (POST /actions/audio.beep, write scope).
func (a *App) beep2K() error {
	a.banner("Camera speaker: beep")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD use the signed API of the camera (http://" + ip + ":8092/akam/v1, key from key-2k): POST /actions/audio.beep")
		return nil
	}
	cl, err := a.api2K(ip)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(a.Ctx, 10*time.Second)
	defer cancel()
	if err := cl.Action(ctx, "audio.beep", nil); err != nil {
		var ae *akamapi.APIError
		if errors.As(err, &ae) && ae.Status == 409 {
			return a.UI.Fail("the speaker is not running: "+ui.Sanitize(ae.Detail)+".", "turn it on first: "+Prog+" audio on (then audio show to see it).")
		}
		return a.api2KFail(err)
	}
	a.UI.Good("  Done. A short beep plays on the speaker (if you heard nothing: volume, docs/troubleshooting.md, audio output).")
	return nil
}

// ask2K asks one line (validated by the catalogue) and returns it; ok is false on no input or a refused value.
func (a *App) ask2K(key, prompt, def string) (string, bool) {
	s, err := a.UI.Prompt(prompt, def)
	if err != nil {
		a.UI.Fail("no input.", "nothing was changed.")
		return "", false
	}
	v, verr := camopt.Validate2K(key, strings.TrimSpace(s))
	if verr != nil {
		a.UI.Fail(ui.Sanitize(verr.Error())+".", "nothing was changed.")
		return "", false
	}
	return v, true
}

// askSecret2K asks a write-only value without echo and validates it by the catalogue; the value joins the scrubber.
func (a *App) askSecret2K(key, prompt string) (string, bool) {
	s, err := a.UI.Secret(prompt)
	if err != nil {
		a.UI.Fail("no input.", "nothing was changed.")
		return "", false
	}
	v, verr := camopt.Validate2K(key, s)
	if verr != nil {
		a.UI.Fail(ui.Sanitize(verr.Error())+".", "nothing was changed.")
		return "", false
	}
	if v != "" {
		a.Secrets.Add(v)
	}
	return v, true
}

func (a *App) mqttSet2K() error {
	if a.UI.Dry() {
		a.UI.Banner("MQTT: set")
		a.UI.Say("  WOULD ask the broker address, port, user name and password (the password is not shown and goes in the signed PATCH body only).")
		a.UI.Say("  WOULD send: PATCH /settings {\"mqtt.enabled\":\"1\",\"mqtt.host\":\"<host>\",\"mqtt.password\":\"***\",\"mqtt.port\":\"<port>\",\"mqtt.user\":\"<user>\"}")
		return nil
	}
	if err := a.UI.RequireTTY("turning MQTT on"); err != nil {
		return err
	}
	a.UI.Say("  MQTT lets the camera publish its state to a broker (for example Home Assistant). The camera connects out to the broker over plain TCP,")
	a.UI.Say("  so the broker password travels in clear on your network. Use an IP address or a name your router's DNS knows (not .local).")
	host, ok := a.ask2K("mqtt.host", "  Broker address (host name or IPv4 address):", "")
	if !ok {
		return ui.ErrFailed
	}
	if host == "" {
		return a.UI.Fail("no broker address.", "nothing was changed.")
	}
	port, ok := a.ask2K("mqtt.port", "  Broker port [1883]:", "1883")
	if !ok {
		return ui.ErrFailed
	}
	user, ok := a.ask2K("mqtt.user", "  Broker user name (empty for none):", "")
	if !ok {
		return ui.ErrFailed
	}
	ch := []camopt.Change{{ID: "mqtt.host", Value: host}, {ID: "mqtt.port", Value: port}, {ID: "mqtt.user", Value: user}}
	pw := ""
	if user != "" {
		if pw, ok = a.askSecret2K("mqtt.password", "  Broker password (not shown, empty for none):"); !ok {
			return ui.ErrFailed
		}
	}
	ch = append(ch, camopt.Change{ID: "mqtt.password", Value: pw}, camopt.Change{ID: "mqtt.enabled", Value: "1"})
	if !a.confirm2K("  Turn MQTT on with these settings?") {
		a.UI.Say("  Left as it was (MQTT off).")
		return nil
	}
	return a.run2KOpts("MQTT: set", ch, []string{"mqtt.enabled", "mqtt.host", "mqtt.port", "mqtt.user"})
}

func (a *App) mqttForget2K() error {
	if !a.UI.Dry() && !a.confirm2K("  Delete the saved MQTT settings and the broker password from the camera?") {
		a.UI.Say("  Left as it was.")
		return nil
	}
	return a.run2KOpts("MQTT: forget", []camopt.Change{{ID: "mqtt.enabled", Value: "0"}, {ID: "mqtt.host", Value: ""}, {ID: "mqtt.user", Value: ""}, {ID: "mqtt.password", Value: ""}}, []string{"mqtt.enabled", "mqtt.host"})
}

func (a *App) rtspAuthSet2K() error {
	if a.UI.Dry() {
		a.UI.Banner("Stream password: set")
		a.UI.Say("  WOULD ask the user name and the password twice (not shown) and send both in ONE signed PATCH (rtsp.user, rtsp.password).")
		return nil
	}
	if err := a.UI.RequireTTY("setting the stream password"); err != nil {
		return err
	}
	a.UI.Say("  Players and ONVIF will have to log in (Digest). Only the hash of the password is stored on the camera.")
	user, ok := a.ask2K("rtsp.user", "  Stream user name:", "")
	if !ok {
		return ui.ErrFailed
	}
	if user == "" {
		return a.UI.Fail("no user name.", "nothing was changed.")
	}
	pw, ok := a.askSecret2K("rtsp.password", "  Stream password (not shown, at least 8 characters):")
	if !ok {
		return ui.ErrFailed
	}
	again, err := a.UI.Secret("  Type it again:")
	if err != nil || again != pw {
		return a.UI.Fail("the two passwords differ.", "nothing was changed.")
	}
	return a.run2KOpts("Stream password: set", []camopt.Change{{ID: "rtsp.user", Value: user}, {ID: "rtsp.password", Value: pw}}, []string{"rtsp.user"})
}
