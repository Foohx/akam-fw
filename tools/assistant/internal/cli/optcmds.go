package cli

import (
	"fmt"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
)

// This file: video and ntp, the two commands of the bash wizard that the Go program lacked. Texts are
// those of steps_camera.sh. Values go through internal/camopt (validators and the one apply path).

// exchanger adapts the control-port exchange of this connection to the option engine.
func (a *App) exchanger(c *camssh.Client) camopt.Exchanger {
	return func(lines ...string) []string { return a.ctlExchange(c, lines...) }
}

// ---------------------------------------------------------------- video

func (a *App) videoShow(c *camssh.Client) error {
	// one exchange for the six values
	st := camopt.Read(a.exchanger(c), "video.bitrate_cfg", "video.fps_cfg", "video.gop_cfg", "video.cfg_pending", "video.bitrate", "video.fps_actual")
	br, fps, gop := st[0], st[1], st[2]
	if br.Unavailable || fps.Unavailable || gop.Unavailable {
		return a.UI.Fail("the camera did not answer the video settings.",
			fmt.Sprintf("it may run an older Akam image without them (%s update), or its control port may be busy; try again.", Prog))
	}
	pend, mbr, mfps := st[3].Value, st[4].Value, st[5].Value
	a.UI.Say(fmt.Sprintf("  Bitrate cap: %s kbit/s (%d to %d; the default is 1536).", br.Value, camopt.VideoKbpsMin, camopt.VideoKbpsMax))
	a.UI.Say(fmt.Sprintf("  Frame rate: %s per second (10 or 15; the default is 15).", fps.Value))
	a.UI.Say(fmt.Sprintf("  Key frame every %s second(s) (%d to %d; the default is 2).", gop.Value, camopt.VideoGopMin, camopt.VideoGopMax))
	if pend == "1" {
		a.UI.Warn("these values are saved but not running yet: they apply at the next restart of the video service.")
	}
	if mbr != "" || mfps != "" {
		if mbr == "" {
			mbr = "?"
		}
		if mfps == "" {
			mfps = "?"
		}
		a.UI.Say(fmt.Sprintf("  Measured now: %s kbit/s, %s (tenths of a frame per second).", mbr, mfps))
	}
	a.showHealth(a.readHealthOf(c, false))
	a.UI.Say("  Resolution and flip/mirror cannot be changed in this version (docs/troubleshooting.md, video settings).")
	return nil
}

func (a *App) cmdVideo(args []string) error {
	var changes []camopt.Change
	show := false
	has := func(id string) bool {
		for _, c := range changes {
			if c.ID == id {
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
		case "bitrate", "fps", "gop":
			id := map[string]string{"bitrate": "video.bitrate_cfg", "fps": "video.fps_cfg", "gop": "video.gop_cfg"}[w]
			var msg string
			switch w {
			case "bitrate":
				msg = fmt.Sprintf("video bitrate needs a whole number of kbit/s from %d to %d", camopt.VideoKbpsMin, camopt.VideoKbpsMax)
			case "fps":
				msg = "video fps needs 10 or 15"
			default:
				msg = fmt.Sprintf("video gop needs a whole number of seconds from %d to %d", camopt.VideoGopMin, camopt.VideoGopMax)
			}
			// the bash wizard accepts digits only (up to 6) here; the engine's validator also range-checks
			if next == "" || !allDigits(next) || len(next) > 6 {
				return audioUsage(msg)
			}
			v, err := camopt.Validate(id, next)
			if err != nil {
				return audioUsage(msg)
			}
			if has(id) {
				return audioUsage(fmt.Sprintf("video %s given twice", w))
			}
			changes = append(changes, camopt.Change{ID: id, Value: v})
			i += 2
		case "flip", "mirror", "resolution":
			return audioUsage(fmt.Sprintf("video %s is not available: the camera's flip registers and a second resolution are not supported yet", w))
		default:
			return audioUsage(fmt.Sprintf("video: unknown word '%s'", w))
		}
	}
	if show && len(changes) > 0 {
		return audioUsage("video: show takes no other word")
	}
	a.banner("Video settings")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 %d", ctl.Port))
		for _, c := range changes {
			o, _ := camopt.Find(c.ID)
			a.UI.Say(fmt.Sprintf("  WOULD send: SET %s %s", o.Key, c.Value))
		}
		if len(changes) > 0 {
			a.UI.Say("  WOULD ask: Restart the video service now? [y/N] (default No); if yes: ssh kill the ak_rtsp pid (the wrapper starts it again)")
		} else {
			a.UI.Say("  WOULD send (read-only): GET video.bitrate_cfg, video.fps_cfg, video.gop_cfg, video.cfg_pending, video.bitrate, video.fps_actual")
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if len(changes) > 0 {
		restart := false
		for _, ch := range changes {
			o, _ := camopt.Find(ch.ID)
			// one SET at a time, like the bash wizard: the first refusal stops the command
			res, err := camopt.Apply(a.exchanger(c), []camopt.Change{ch})
			if err != nil {
				return audioUsage(err.Error())
			}
			r := res[0]
			if err := a.ctlResult(o.Key, ch.Value, r); err != nil {
				return err
			}
			// only the last answer counts: after "bitrate 1536 fps 15 gop 2" the earlier ones may still say restart
			restart = r.Restart
		}
		a.UI.Good("  Saved on the camera.")
		if restart {
			a.UI.Say("  It applies at the next restart of the video service.")
			if a.restartVideo(c) {
				a.UI.Good(fmt.Sprintf("  The video service was asked to restart; it is back in about 20 seconds (%s video show).", Prog))
				return nil
			}
			a.UI.Say("  Not restarted: the new values apply at the next restart of the video service or the next reboot.")
		}
	}
	return a.videoShow(c)
}

// ctlResult shows what ctlSet shows for one engine result: nil when the camera accepted the value.
func (a *App) ctlResult(key, val string, r camopt.Result) error {
	switch {
	case r.OK:
		if r.NotSaved {
			a.UI.Warn(fmt.Sprintf("%s is %s now, but the camera could not save it: it will be lost at the next reboot (%s).", key, val, r.Line))
		}
		return nil
	case r.Answered:
		return a.UI.Fail(fmt.Sprintf("the camera refused %s %s: %s", key, val, r.Err), fmt.Sprintf("this image may not have that setting (%s update).", Prog))
	}
	return a.UI.Fail(fmt.Sprintf("the camera did not confirm %s %s.", key, val),
		fmt.Sprintf("it may run an older Akam image (%s update), or its control port may be busy; try again.", Prog))
}

// ---------------------------------------------------------------- ntp

func (a *App) cmdNtp(args []string) error {
	action := "status"
	if len(args) > 0 && args[0] != "" {
		action = args[0]
	}
	var servers []string
	switch action {
	case "status", "off":
		if len(args) > 1 {
			return audioUsage(fmt.Sprintf("ntp %s takes no argument", action))
		}
	case "set":
		servers = args[1:]
		if len(servers) < 1 || len(servers) > 3 {
			return audioUsage(fmt.Sprintf("ntp set needs 1 to 3 servers, for example: %s ntp set pool.ntp.org", Prog))
		}
		for _, s := range servers {
			if !camopt.ValidNTPServer(s) {
				return audioUsage(fmt.Sprintf("ntp set: '%s' is not a host name or an IPv4 address", s))
			}
		}
	default:
		return audioUsage("ntp needs status, set SERVER... or off")
	}
	list := strings.Join(servers, " ")
	a.banner("Time sync: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	remote := remoteNtp(action, servers...)
	if a.UI.Dry() {
		a.UI.Say("  WOULD run over SSH on the camera: " + remote)
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if action == "set" {
		a.UI.Say(fmt.Sprintf("  The camera will ask %s for the time at start-up and every 6 hours (UDP 123, outgoing only).", list))
		a.UI.Say(fmt.Sprintf("  Nothing is sent unless you do this; %s ntp off turns it off again.", Prog))
	}
	res, err := a.run(c, remote, nil, "")
	if err != nil {
		return a.UI.Fail(err.Error(), "")
	}
	out := strings.TrimRight(res.Combined(), "\n")
	if out != "" {
		for _, l := range strings.Split(out, "\n") {
			a.UI.Say("  " + plainText(l))
		}
	}
	if res.Status == 0 && action == "status" {
		h := a.readHealthOf(c, true)
		h.ntp, h.ntpSync = parseNtpStatus(out)
		cl, _ := clockReport(h)
		for _, l := range cl {
			a.UI.Say("  " + l)
		}
		a.clockOffer(c, h)
	}
	if res.Status != 0 {
		if strings.Contains(out, "not found") || strings.Contains(out, "No such file") {
			return a.UI.Fail(fmt.Sprintf("this camera does not have %s.", ntpCmd), fmt.Sprintf("it runs an older Akam image: %s update.", Prog))
		}
		return a.UI.Fail(fmt.Sprintf("the camera did not run %s %s.", ntpCmd, action), "read the lines above.")
	}
	return nil
}
