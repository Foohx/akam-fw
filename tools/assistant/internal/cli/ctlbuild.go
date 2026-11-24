package cli

import (
	"context"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
)

// cameraBuildID is the firmware identity the camera reports (fw.version, like "b35-2af81c5"): from the text port when it
// is open (an older image, or the LAN opt-in), else from the signed API when this PC has a key. "" when neither answers.
func (a *App) cameraBuildID(ip string) string {
	if id, err := discover.Identify(a.Ctx, ip, a.ports().CTL, a.Hooks.Discover.Dial); err == nil && id.Version != "" {
		return id.Version
	}
	k, ok, err := a.apiKey2K()
	if err != nil || !ok {
		return ""
	}
	ctx, cancel := context.WithTimeout(a.Ctx, 5*time.Second)
	defer cancel()
	if in, err := a.camAPI(ip, k).Info(ctx); err == nil {
		return in.App
	}
	return ""
}

// controlLAN is the saved opt-in of the text port ("1" open to the network, "0" closed), "" when it cannot be read (no key on this PC,
// an image without the row, the API does not answer). It never asks to create a key: this is a check, not a flow.
func (a *App) controlLAN(ip string) string {
	if _, ok, err := a.apiKey2K(); err != nil || !ok {
		return ""
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return ""
	}
	defer c.Close()
	return ctl.Get("control.lan", a.ctlExchange(c, "GET control.lan"))
}

// apiKeyOffer is the end of an install (verify): the settings of the camera go through its signed API, so the key of this PC is made
// now. Interactive runs are asked (default Yes: nothing else changes on the camera); without a terminal nothing is done and the
// command that does it is named.
func (a *App) apiKeyOffer(ip string) {
	if a.UI.Dry() {
		return
	}
	if !a.UI.Interactive() && !a.Opt.Yes {
		a.UI.Hint("settings of the camera go through its API key: run once, in a terminal: " + Prog + " api-key")
		return
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return
	}
	defer c.Close()
	_, _ = a.ctlClient(c)
}
