package cli

import (
	"fmt"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
)

// control-port [status | open | close]: the text port 8091 of the 1080p listens on the camera's own loopback only
// (the SSH helpers and the on-camera scripts use it there). The saved opt-in `control.lan` opens it to the local network
// for monitoring tools (metrics, LED and picture values; writes from the network stay refused). The assistant itself never
// needs it: it uses the signed API (port 8092).

func (a *App) cmdControlPort(args []string) error {
	action := "status"
	if len(args) > 0 {
		action = args[0]
	}
	if len(args) > 1 || (action != "status" && action != "open" && action != "close") {
		return usageErr(fmt.Sprintf("akam: control-port needs status, open or close (see %s help)", Prog))
	}
	a.banner("Text control port 8091 of the camera: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.sayWouldAPI(ip)
		switch action {
		case "open":
			a.UI.Say("  WOULD ask: Open the text port 8091 to your network? [y/N]; then send: PATCH /settings {\"control.lan\":\"1\"} and offer to restart the video service (the port is bound when it starts).")
		case "close":
			a.UI.Say("  WOULD send: PATCH /settings {\"control.lan\":\"0\"} and offer to restart the video service.")
		default:
			a.UI.Say("  WOULD read (read-only): GET /settings control.lan, and probe the port from here.")
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	out := a.ctlExchange(c, "GET control.lan")
	saved := ctl.Get("control.lan", out)
	if saved == "" {
		return a.ctlNoAnswer("the camera did not report the setting.",
			fmt.Sprintf("it may run an older Akam image without it (%s update), or its API may be busy; try again.", Prog))
	}
	want := map[string]string{"open": "1", "close": "0"}[action]
	if want != "" && want != saved {
		if action == "open" {
			a.UI.Warn("the text port has no password and no encryption: anyone on your network could read its values (LED, picture, Wi-Fi level, counters). Writes from the network stay refused.")
			if err := a.UI.RequireTTY("opening the text port"); err != nil {
				return err
			}
			ok, _ := a.UI.AskYN("  Open the text port 8091 to your network?")
			if !ok {
				a.UI.Say("  Left as it was (closed to the network).")
				return nil
			}
		}
		res := a.ctlExchange(c, "SET control.lan "+want)
		r := ctl.ParseSet(res)
		if !r.OK {
			if r.Answered {
				return a.UI.Fail("the camera refused the change: "+r.Err, "nothing was changed.")
			}
			return a.ctlNoAnswer("the camera did not confirm the change.", "try again.")
		}
		saved = want
		a.UI.Good("  Saved on the camera.")
		if r.Restart {
			a.UI.Say("  The port is bound when the video service starts, so it applies at its next restart.")
			if a.restartVideo(c) {
				a.UI.Good("  The video service was asked to restart; the port is as chosen in about 20 seconds.")
			} else {
				a.UI.Say("  Not restarted: it applies at the next restart of the video service or the next reboot.")
			}
		}
	}
	saveText := map[string]string{"1": "open to your network", "0": "closed to the network (the camera itself still reaches it on 127.0.0.1)"}[saved]
	a.UI.Say("  Saved choice: " + saveText + ".")
	open := a.probe(ip, a.Env.PortCTL)
	if len(open) > 0 {
		a.UI.Say(fmt.Sprintf("  Right now port %d answers from this computer.", a.Env.PortCTL))
	} else {
		a.UI.Say(fmt.Sprintf("  Right now port %d does not answer from this computer.", a.Env.PortCTL))
	}
	return nil
}
