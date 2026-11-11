package cli

import (
	"fmt"
	"regexp"
	"strconv"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// This file: led, audio, picture, the commands that talk to the control port (8091) through an
// SSH forward to the camera's own loopback. Texts are those of steps_camera.sh.

// ctlExchange is led_remote: send GET/SET lines to 127.0.0.1:8091 on the camera, return the
// answers without the LOG lines.
func (a *App) ctlExchange(c *camssh.Client, lines ...string) []string {
	a.Log.Line("CMD ssh root@%s printf <%d> | nc -w 2 127.0.0.1 %d", c.Host, len(lines), ctl.Port)
	conn, err := c.Forward("127.0.0.1", ctl.Port)
	if err != nil {
		a.Log.Line("forward to 127.0.0.1:%d failed: %v", ctl.Port, err)
		return nil
	}
	return ctl.Exchange(conn, lines, ctl.Idle)
}

// connectTools is rtsp_connect with the wording of the "tools" commands.
func (a *App) connectTools(ip string) (*camssh.Client, error) {
	return a.connect(ip,
		fmt.Sprintf("cannot log in to %s over SSH with %s.", ip, a.keyPath()),
		fmt.Sprintf("the camera must run Akam (see %s step verify); pass --identity FILE if your key is elsewhere.", Prog))
}

// ---------------------------------------------------------------- led

func (a *App) ledShow(c *camssh.Client) error {
	out := a.ctlExchange(c, "GET led.status", "GET led.state", "GET night.ir_led")
	st, state, ir := ctl.Get("led.status", out), ctl.Get("led.state", out), ctl.Get("night.ir_led", out)
	if st == "" || ir == "" {
		return a.UI.Fail("the camera did not answer the LED settings.",
			fmt.Sprintf("it may run an older Akam image without them (%s update), or its control port may be busy; try again.", Prog))
	}
	switch st {
	case "on":
		a.UI.Say("  Front LED: on (left as the camera sets it).")
	case "off":
		a.UI.Say("  Front LED: off.")
	case "auto":
		if state == "" {
			state = "unknown"
		}
		a.UI.Say(fmt.Sprintf("  Front LED: auto (now: %s). Blinks while the camera starts or has no Wi-Fi, blinks slowly while someone watches the video, solid otherwise.", state))
	default:
		a.UI.Say(fmt.Sprintf("  Front LED: %s.", st))
	}
	switch ir {
	case "auto":
		a.UI.Say("  IR LEDs at night: auto (they light in night mode).")
	case "off":
		a.UI.Say("  IR LEDs at night: off (never lit; the night image still switches on).")
	default:
		a.UI.Say(fmt.Sprintf("  IR LEDs at night: %s.", ir))
	}
	a.UI.Say("  Both settings are saved on the camera and kept after a reboot.")
	return nil
}

// ledCheckSet is led_check_set.
func (a *App) ledCheckSet(key, val string, answers []string) error {
	r := ctl.ParseLedSet(key, val, answers)
	switch {
	case r.OK:
		if r.NotSaved {
			a.UI.Warn(fmt.Sprintf("%s is %s now, but the camera could not save it: it will be lost at the next reboot (%s).", key, val, r.Line))
		}
		return nil
	case r.Answered:
		return a.UI.Fail(fmt.Sprintf("the camera refused %s %s: %s", key, val, r.Err), fmt.Sprintf("this image may not have the LED settings (%s update).", Prog))
	}
	return a.UI.Fail(fmt.Sprintf("the camera did not confirm %s %s.", key, val),
		fmt.Sprintf("it may run an older Akam image without the LED settings (%s update), or its control port may be busy; try again.", Prog))
}

func (a *App) cmdLed(args []string) error {
	status, ir := "", ""
	show := false
	for i := 0; i < len(args); {
		w := args[i]
		next := ""
		if i+1 < len(args) {
			next = args[i+1]
		}
		switch w {
		case "show":
			show = true
			i++
		case "status":
			if next != "on" && next != "off" && next != "auto" {
				return usageErr(fmt.Sprintf("akam: led status needs on, off or auto (see %s help)", Prog))
			}
			if status != "" {
				return usageErr(fmt.Sprintf("akam: led status given twice (see %s help)", Prog))
			}
			status = next
			i += 2
		case "ir":
			if next != "auto" && next != "off" {
				return usageErr(fmt.Sprintf("akam: led ir needs auto or off (see %s help)", Prog))
			}
			if ir != "" {
				return usageErr(fmt.Sprintf("akam: led ir given twice (see %s help)", Prog))
			}
			ir = next
			i += 2
		default:
			return usageErr(fmt.Sprintf("akam: led: unknown word '%s' (see %s help)", w, Prog))
		}
	}
	if show && (status != "" || ir != "") {
		return usageErr(fmt.Sprintf("akam: led: show cannot be combined with status or ir (see %s help)", Prog))
	}
	a.banner("Front LED and IR LEDs")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 %d", ctl.Port))
		if status != "" {
			a.UI.Say("  WOULD send: SET led.status " + status)
		}
		if ir != "" {
			a.UI.Say("  WOULD send: SET night.ir_led " + ir)
		}
		if status == "" && ir == "" {
			a.UI.Say("  WOULD send (read-only): GET led.status, GET led.state, GET night.ir_led")
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if status != "" || ir != "" {
		var lines []string
		if status != "" {
			lines = append(lines, "SET led.status "+status)
		}
		if ir != "" {
			lines = append(lines, "SET night.ir_led "+ir)
		}
		out := a.ctlExchange(c, lines...)
		if status != "" {
			if err := a.ledCheckSet("led.status", status, out); err != nil {
				return err
			}
		}
		if ir != "" {
			if err := a.ledCheckSet("night.ir_led", ir, out); err != nil {
				return err
			}
		}
		a.UI.Good("  Done.")
	}
	return a.ledShow(c)
}

// ---------------------------------------------------------------- audio, picture

// ctlSet is ctl_set: one SET; false (with the reason shown) when the camera did not answer OK.
func (a *App) ctlSet(c *camssh.Client, key, val string) (restart bool, err error) {
	out := a.ctlExchange(c, "SET "+key+" "+val)
	r := ctl.ParseSet(out)
	switch {
	case r.OK:
		if r.NotSaved {
			a.UI.Warn(fmt.Sprintf("%s is %s now, but the camera could not save it: it will be lost at the next reboot (%s).", key, val, r.Line))
		}
		return r.Restart, nil
	case r.Answered:
		return false, a.UI.Fail(fmt.Sprintf("the camera refused %s %s: %s", key, val, r.Err), fmt.Sprintf("this image may not have that setting (%s update).", Prog))
	}
	return false, a.UI.Fail(fmt.Sprintf("the camera did not confirm %s %s.", key, val),
		fmt.Sprintf("it may run an older Akam image (%s update), or its control port may be busy; try again.", Prog))
}

func ctlOnOff(v string) string {
	switch v {
	case "1":
		return "on"
	case "0":
		return "off"
	case "":
		return "unknown"
	}
	return plainText(v)
}

func (a *App) audioShow(c *camssh.Client) error {
	out := a.ctlExchange(c, "GET audio.out_volume", "GET audio.out_enable", "GET audio.talk_enable", "GET audio.out_state", "GET audio.out_underruns", "GET audio.out_drops", "GET audio.mic_enable")
	mic := ctl.Get("audio.mic_enable", out)
	vol, en := ctl.Get("audio.out_volume", out), ctl.Get("audio.out_enable", out)
	talk, state := ctl.Get("audio.talk_enable", out), ctl.Get("audio.out_state", out)
	under, drops := ctl.Get("audio.out_underruns", out), ctl.Get("audio.out_drops", out)
	if vol == "" || en == "" {
		return a.UI.Fail("the camera did not answer the audio settings.",
			fmt.Sprintf("it may run an older Akam image without audio output (%s update), or its control port may be busy; try again.", Prog))
	}
	if state == "" {
		state = "unknown"
	}
	a.UI.Say(fmt.Sprintf("  Speaker output: %s (state: %s); the on/off choice is read at start-up.", ctlOnOff(en), state))
	a.UI.Say(fmt.Sprintf("  Volume: %s (0 to 100).", vol))
	a.UI.Say(fmt.Sprintf("  Talk (RTSP audio backchannel): %s.", ctlOnOff(talk)))
	if mic != "" {
		a.UI.Say(fmt.Sprintf("  Microphone: %s (off = never opened and no audio track in the stream; read at start-up).", ctlOnOff(mic)))
	}
	if under != "" || drops != "" {
		if under == "" {
			under = "?"
		}
		if drops == "" {
			drops = "?"
		}
		a.UI.Say(fmt.Sprintf("  Playback counters: underruns %s, drops %s.", under, drops))
	}
	a.UI.Say("  Volume, speaker, talk and microphone switches are saved on the camera and kept after a reboot.")
	return nil
}

func audioUsage(msg string) error {
	return usageErr(fmt.Sprintf("akam: %s (see %s help)", msg, Prog))
}

var digitsRe = regexp.MustCompile(`^[0-9]{1,3}$`)

func (a *App) cmdAudio(args []string) error {
	action, value := "show", ""
	if len(args) > 0 {
		action = args[0]
		switch action {
		case "show", "beep", "on", "off":
			if len(args) != 1 {
				return audioUsage(fmt.Sprintf("audio %s takes no more words", action))
			}
		case "volume":
			if len(args) != 2 || !digitsRe.MatchString(args[1]) {
				return audioUsage("audio volume needs a number from 0 to 100")
			}
			n, _ := strconv.Atoi(args[1])
			if n > 100 {
				return audioUsage("audio volume needs a number from 0 to 100")
			}
			value = strconv.Itoa(n)
		case "talk":
			if len(args) != 2 {
				return audioUsage("audio talk needs on or off")
			}
			if args[1] != "on" && args[1] != "off" {
				return audioUsage("audio talk needs on or off")
			}
			value = args[1]
		case "mic":
			if len(args) != 2 || (args[1] != "on" && args[1] != "off") {
				return audioUsage("audio mic needs on or off")
			}
			value = args[1]
		default:
			return audioUsage(fmt.Sprintf("audio: unknown word '%s'", args[0]))
		}
	}
	key, val := "", ""
	switch action {
	case "volume":
		key, val = "audio.out_volume", value
	case "beep":
		key, val = "audio.out_test", "1"
	case "on":
		key, val = "audio.out_enable", "1"
	case "off":
		key, val = "audio.out_enable", "0"
	case "mic":
		key = "audio.mic_enable"
		val = "0"
		if value == "on" {
			val = "1"
		}
	case "talk":
		key = "audio.talk_enable"
		val = "0"
		if value == "on" {
			val = "1"
		}
	}
	a.banner("Camera speaker")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 %d", ctl.Port))
		if key != "" {
			a.UI.Say(fmt.Sprintf("  WOULD send: SET %s %s", key, val))
		} else {
			a.UI.Say("  WOULD send (read-only): GET audio.out_volume, audio.out_enable, audio.talk_enable, audio.out_state, audio.out_underruns, audio.out_drops")
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if key != "" {
		restart, err := a.ctlSet(c, key, val)
		if err != nil {
			return err
		}
		switch action {
		case "beep":
			a.UI.Good("  Done. A short beep plays on the speaker (if you heard nothing: volume, docs/troubleshooting.md, audio output).")
		case "on":
			if restart {
				a.UI.Warn("the speaker is enabled and saved, but it only starts after the camera application restarts: reboot the camera (power cycle) to apply.")
			} else {
				a.UI.Good("  Done. The speaker is enabled.")
			}
		case "mic":
			if restart {
				a.UI.Warn("the microphone setting is saved, but the camera reads it at start-up: reboot the camera (power cycle) to apply. With the microphone off the stream has no audio track at all.")
			} else {
				a.UI.Good("  Done. The microphone setting is saved.")
			}
		case "off":
			a.UI.Good(fmt.Sprintf("  Done. The speaker is off now and stays off after a reboot (%s audio on to enable it again).", Prog))
		default:
			a.UI.Good("  Done.")
		}
	}
	return a.audioShow(c)
}

func (a *App) pictureShow(c *camssh.Client) error {
	out := a.ctlExchange(c, "GET isp.brightness", "GET isp.contrast", "GET isp.saturation", "GET isp.sharpness", "GET img.wb", "GET night.mode")
	br, co := ctl.Get("isp.brightness", out), ctl.Get("isp.contrast", out)
	sa, sh := ctl.Get("isp.saturation", out), ctl.Get("isp.sharpness", out)
	wb, nm := ctl.Get("img.wb", out), ctl.Get("night.mode", out)
	if br == "" || co == "" || sa == "" || sh == "" {
		return a.UI.Fail("the camera did not answer the picture settings.", "its control port may be busy; try again.")
	}
	if wb == "" {
		wb = "unknown"
	}
	if nm == "" {
		nm = "unknown"
	}
	a.UI.Say(fmt.Sprintf("  Brightness %s, contrast %s, saturation %s, sharpness %s (each -50 to 50, 0 = unchanged). Saved on the camera.", br, co, sa, sh))
	a.UI.Say(fmt.Sprintf("  White balance: %s (conf = the camera's calibration, manual = Akam's own block). Saved on the camera.", wb))
	a.UI.Say(fmt.Sprintf("  Day/night: %s (auto, day or night). Saved on the camera.", nm))
	return nil
}

var picNumRe = regexp.MustCompile(`^-?[0-9]{1,3}$`)

// pictureNum is picture_num + picture_norm: a whole number from -50 to 50, normalised.
func pictureNum(w string) (string, bool) {
	if !picNumRe.MatchString(w) {
		return "", false
	}
	n, _ := strconv.Atoi(w)
	if n < -50 || n > 50 {
		return "", false
	}
	return strconv.Itoa(n), true
}

func (a *App) cmdPicture(args []string) error {
	var keys, vals []string
	reset, show := false, false
	has := func(k string) bool {
		for _, x := range keys {
			if x == k {
				return true
			}
		}
		return false
	}
	for i := 0; i < len(args); {
		w := args[i]
		next := ""
		if i+1 < len(args) {
			next = args[i+1]
		}
		switch w {
		case "show":
			show = true
			i++
		case "reset":
			reset = true
			i++
		case "brightness", "contrast", "saturation", "sharpness":
			v, ok := pictureNum(next)
			if !ok {
				return audioUsage(fmt.Sprintf("picture %s needs a whole number from -50 to 50", w))
			}
			if has("isp." + w) {
				return audioUsage(fmt.Sprintf("picture %s given twice", w))
			}
			keys, vals = append(keys, "isp."+w), append(vals, v)
			i += 2
		case "wb":
			if next != "conf" && next != "manual" {
				return audioUsage("picture wb needs conf or manual")
			}
			if has("img.wb") {
				return audioUsage("picture wb given twice")
			}
			keys, vals = append(keys, "img.wb"), append(vals, next)
			i += 2
		case "night":
			if next != "auto" && next != "day" && next != "night" {
				return audioUsage("picture night needs auto, day or night")
			}
			if has("night.mode") {
				return audioUsage("picture night given twice")
			}
			keys, vals = append(keys, "night.mode"), append(vals, next)
			i += 2
		default:
			return audioUsage(fmt.Sprintf("picture: unknown word '%s'", w))
		}
	}
	n := 0
	if show {
		n++
	}
	if reset {
		n++
	}
	if n > 0 && (n > 1 || len(keys) > 0) {
		return audioUsage("picture: show and reset take no other word")
	}
	if reset {
		keys = []string{"isp.brightness", "isp.contrast", "isp.saturation", "isp.sharpness"}
		vals = []string{"0", "0", "0", "0"}
	}
	a.banner("Picture settings")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 %d", ctl.Port))
		for i := range keys {
			a.UI.Say(fmt.Sprintf("  WOULD send: SET %s %s", keys[i], vals[i]))
		}
		if len(keys) == 0 {
			a.UI.Say("  WOULD send (read-only): GET isp.brightness, isp.contrast, isp.saturation, isp.sharpness, img.wb, night.mode")
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if len(keys) > 0 {
		for i := range keys {
			if _, err := a.ctlSet(c, keys[i], vals[i]); err != nil {
				return err
			}
		}
		if reset {
			a.UI.Good("  Done. Brightness, contrast, saturation and sharpness are back to 0 and the saved file is removed.")
		} else {
			a.UI.Good("  Done.")
		}
	}
	return a.pictureShow(c)
}
