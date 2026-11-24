package cli

import (
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/ctl"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The 1080p is driven through the signed API of the camera (port 8092), like the 2K: its text port 8091 is closed to
// the network. This file is the one way the commands and the option screens reach a camera's settings. The commands
// still speak in the old key names (led.status, isp.contrast, video.bitrate_cfg ...); internal/camera/ctl turns each
// line into a call of the API and the answer back. The key of this PC is the file api_key of the camera's state folder
// (the same file the 2K uses), made by the camera itself over SSH once (`akamd --api-key new`), the secret never
// printed.

const (
	// keyName1080 is the label of the key this program makes on the camera.
	keyName1080 = "assistant"
	// akRtspBin is the akamd of the 1080p image; its --api-key mode only touches the key file and runs while the camera streams.
	akRtspBin = "/usr/bin/akamd"
	// akRtspBinLegacy is the same program under its 0.1.x name: a camera that still runs that firmware has no
	// /usr/bin/akamd, so the commands below are retried with this path when the shell says "not found" (127).
	akRtspBinLegacy = "/usr/bin/ak_rtsp"
	// apiKeyNewCmd1080 makes the admin key; the camera prints its id and secret once.
	apiKeyNewCmd1080 = akRtspBin + " --api-key new --scope admin --name " + keyName1080
	// apiKeyRevokeCmd1080 removes it (--force: it may be the only admin key).
	apiKeyRevokeCmd1080 = akRtspBin + " --api-key revoke " + keyName1080 + " --force"
	// the same two for a 0.1.x camera
	apiKeyNewCmd1080Legacy    = akRtspBinLegacy + " --api-key new --scope admin --name " + keyName1080
	apiKeyRevokeCmd1080Legacy = akRtspBinLegacy + " --api-key revoke " + keyName1080 + " --force"
)

// ctlTimeout bounds one exchange with the camera (a variable so that a test can shorten it).
var ctlTimeout = ctl.Timeout

// sayWouldAPI is the rehearsal line of every command that reads or changes settings.
func (a *App) sayWouldAPI(ip string) {
	a.UI.Say("  WOULD use the signed API of the camera (http://" + ip + ":8092/akam/v1): GET /settings and GET /state for the values, ONE PATCH /settings for a change.")
	a.UI.Say("  WOULD ask once, if this PC has no API key for the camera, to create one over SSH (" + akRtspBin + " --api-key new: the secret is never shown).")
}

func (a *App) apiKeyPath() string { return filepath.Join(a.St.Dir, "api_key") }

// pacedClient waits and retries a READ the camera answered 429 to (its limit of accepted requests is 0.4 a second after a burst).
// A write is never repeated: a refused PATCH may have spent a flash token, and the user should see it.
type pacedClient struct {
	ctl.Client
	sleep func(context.Context, time.Duration)
}

func isRate(err error) bool {
	var ae *akamapi.APIError
	return errors.As(err, &ae) && ae.Status == 429 && ae.Code != "flash_rate"
}

func (p pacedClient) retry(ctx context.Context, get func() (map[string]string, error)) (map[string]string, error) {
	m, err := get()
	for i := 0; i < 3 && isRate(err); i++ {
		p.sleep(ctx, 2500*time.Millisecond)
		if ctx.Err() != nil {
			break
		}
		m, err = get()
	}
	return m, err
}

func (p pacedClient) Settings(ctx context.Context) (map[string]string, error) {
	return p.retry(ctx, func() (map[string]string, error) { return p.Client.Settings(ctx) })
}

func (p pacedClient) State(ctx context.Context) (map[string]string, error) {
	return p.retry(ctx, func() (map[string]string, error) { return p.Client.State(ctx) })
}

func sleepCtx(ctx context.Context, d time.Duration) {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
	case <-t.C:
	}
}

// ctlClient is the API client of the camera c is connected to. Without a key of this PC it offers to make one over c
// (once per run; --yes answers the question), and says what to do when it cannot.
func (a *App) ctlClient(c *camssh.Client) (ctl.Client, error) {
	if a.ctlCl != nil && a.ctlHost == c.Host {
		return a.ctlCl, nil
	}
	k, ok, err := a.apiKey2K()
	if err != nil {
		a.UI.Err("the API key of this camera cannot be used: " + ui.Sanitize(err.Error()) + ".")
		a.UI.Hint("check the file (--api-key-file, or the api_key file of this camera's state folder), or delete it and run: " + Prog + " api-key")
		return nil, ui.ErrFailed
	}
	if !ok {
		if k, err = a.enrol1080(c, false); err != nil {
			return nil, err
		}
	}
	a.setCtlClient(c.Host, k)
	return a.ctlCl, nil
}

func (a *App) setCtlClient(host string, k akamapi.Key) {
	a.ctlCl = pacedClient{Client: a.camAPI(host, k), sleep: sleepCtx}
	a.ctlHost = host
}

// enrol1080 asks the camera, over SSH, for an admin key named "assistant", keeps it in the state folder and returns it.
// replace removes a key of that name first (this PC lost it, or the camera refuses it). An existing install has no key:
// this is the one-time step that gives it one; nothing else on the camera changes.
func (a *App) enrol1080(c *camssh.Client, replace bool) (akamapi.Key, error) {
	if a.ctlNoKey {
		return akamapi.Key{}, ui.ErrFailed
	}
	if !a.Opt.Yes && !a.UI.Interactive() {
		a.UI.Err("this PC has no API key for this camera, and the settings go through the camera's signed API (port 8092).")
		a.UI.Hint("run once, in a terminal: " + Prog + " api-key (it asks before it creates the key), or pass --yes.")
		return akamapi.Key{}, ui.ErrFailed
	}
	q := "This PC has no API key for this camera yet. Create one over SSH (an admin key named '" + keyName1080 + "'; it opens the control port 8092 on your network, the secret stays on this PC)?"
	if replace {
		q = "The camera refused this PC's API key. Replace it with a new one over SSH (the old key named '" + keyName1080 + "' is revoked first)?"
	}
	if !a.Opt.Yes {
		ok, err := a.UI.AskYN(q)
		if err != nil || !ok {
			a.ctlNoKey = true
			a.UI.Err("not confirmed: nothing was changed on the camera.")
			a.UI.Hint("the settings need the API key: run " + Prog + " api-key when you want to create it.")
			return akamapi.Key{}, ui.ErrFailed
		}
	}
	return a.makeKey1080(c, replace)
}

// makeKey1080 runs the key commands (the question has been answered) and stores the key.
func (a *App) makeKey1080(c *camssh.Client, replace bool) (akamapi.Key, error) {
	if replace {
		if _, err := a.runAPIKeyCmd(c, apiKeyRevokeCmd1080); err != nil && !strings.Contains(err.Error(), "no such key") {
			return akamapi.Key{}, a.UI.Fail("the camera did not revoke the old key: "+ui.Sanitize(err.Error())+".", "nothing was changed.")
		}
	}
	out, err := a.runAPIKeyCmd(c, apiKeyNewCmd1080)
	if err != nil {
		if strings.Contains(err.Error(), "a key named") {
			if replace {
				return akamapi.Key{}, a.UI.Fail("the camera still has a key named '"+keyName1080+"'.", "run: "+Prog+" api-key replace")
			}
			return a.enrol1080Exists(c)
		}
		return akamapi.Key{}, a.UI.Fail("the camera did not create the key: "+ui.Sanitize(err.Error())+".",
			"nothing secret was shown. The camera needs the Akam firmware 0.1.0 or later (it runs `akamd --api-key`); "+Prog+" update brings it.")
	}
	k, err := parseNewKeyOutput(out)
	if err == nil {
		a.Secrets.Add(hex.EncodeToString(k.Secret))
	}
	if err != nil {
		return akamapi.Key{}, a.UI.Fail(ui.Sanitize(err.Error())+".", "")
	}
	if err := storeKey2K(a.apiKeyPath(), k); err != nil {
		return akamapi.Key{}, a.UI.Fail("the camera made key "+k.ID+" but it could not be saved on this PC ("+ui.Sanitize(err.Error())+").",
			"run: "+Prog+" api-key replace")
	}
	a.UI.Good(fmt.Sprintf("  API key %s created on the camera and saved in %s (the secret is not shown).", k.ID, a.apiKeyPath()))
	a.waitAPIOpen(k, c.Host)
	return k, nil
}

// enrol1080Exists: the camera has a key named "assistant" that this PC does not have (a new PC, a deleted state folder).
func (a *App) enrol1080Exists(c *camssh.Client) (akamapi.Key, error) {
	a.UI.Warn("the camera already has a key named '" + keyName1080 + "' that this PC does not have (another PC, or a deleted state folder).")
	if !a.Opt.Yes {
		ok, err := a.UI.AskYN("Revoke it and create a new one for this PC? (another PC that uses that key will have to make its own)")
		if err != nil || !ok {
			a.ctlNoKey = true
			a.UI.Hint("nothing was changed. Copy that key's file to " + a.apiKeyPath() + " or pass --api-key-file FILE.")
			return akamapi.Key{}, ui.ErrFailed
		}
	}
	return a.makeKey1080(c, true)
}

// runAPIKeyCmd runs one of the two fixed key commands on the camera. The output (it holds the secret) is returned, never logged.
func (a *App) runAPIKeyCmd(c *camssh.Client, cmd string) ([]byte, error) {
	if why := forbiddenRemote(cmd); why != "" {
		return nil, errors.New(why)
	}
	a.Log.Line("CMD ssh root@%s %s (answer not logged: it holds the secret)", c.Host, cmd)
	ctx, cancel := context.WithTimeout(a.Ctx, 30*time.Second)
	defer cancel()
	res, err := c.Run(ctx, cmd, nil)
	if err != nil {
		return nil, err
	}
	if res.Status == 127 && strings.HasPrefix(cmd, akRtspBin+" ") {
		// no /usr/bin/akamd: a camera still on firmware 0.1.x, where the program is called ak_rtsp
		return a.runAPIKeyCmd(c, akRtspBinLegacy+strings.TrimPrefix(cmd, akRtspBin))
	}
	if res.Status != 0 {
		msg := strings.TrimSpace(string(res.Stderr))
		if msg == "" {
			msg = fmt.Sprintf("exit status %d", res.Status)
		}
		return nil, errors.New(msg)
	}
	return res.Stdout, nil
}

// waitAPIOpen waits up to a few seconds for the camera to start listening (it follows the key file within about a second).
func (a *App) waitAPIOpen(k akamapi.Key, host string) {
	cl := a.camAPI(host, k)
	for i := 0; i < 8; i++ {
		ctx, cancel := context.WithTimeout(a.Ctx, 3*time.Second)
		_, err := cl.Info(ctx)
		cancel()
		if err == nil || !errors.Is(err, akamapi.ErrClosed) {
			return
		}
		sleepCtx(a.Ctx, time.Second)
	}
}

// ctlFail says why an exchange failed, in plain words.
func (a *App) ctlFail(err error) {
	var ae *akamapi.APIError
	switch {
	case errors.Is(err, akamapi.ErrAuth):
		a.UI.Err("the camera refused this PC's API key (it was revoked, or the camera was reset).")
		a.UI.Hint("run: " + Prog + " api-key replace")
	case errors.Is(err, akamapi.ErrScope):
		a.UI.Err("the API key may only read: changing a setting needs a write or admin key.")
		a.UI.Hint("run: " + Prog + " api-key replace (it makes an admin key)")
	case errors.Is(err, akamapi.ErrClosed):
		a.UI.Err("the camera does not answer its control API on port 8092 (" + ui.Sanitize(err.Error()) + ").")
		a.UI.Hint("wait a few seconds if the video service is restarting; the API listens only while a key exists on the camera: " + Prog + " api-key creates it.")
	case errors.As(err, &ae) && ae.Status == 429:
		a.UI.Err("the camera is rate-limiting its API (" + ui.Sanitize(ae.Error()) + ").")
		a.UI.Hint("wait a few seconds and try again. Changes are limited too: a burst of 6, then one every 20 seconds.")
	case errors.As(err, &ae):
		a.UI.Err("the camera answered: " + ui.Sanitize(ae.Error()) + ".")
	default:
		a.UI.Err("the camera API failed: " + ui.Sanitize(err.Error()) + ".")
	}
}

// ctlExchange sends GET and SET lines (the old key names, one line each) through the signed API and returns the answer
// lines in the old form; see internal/camera/ctl. A failure is explained and returns what was answered (often nothing),
// which the callers already treat as "the camera did not answer".
func (a *App) ctlExchange(c *camssh.Client, lines ...string) []string {
	a.ctlReported = false
	if _, err := a.ctlClient(c); err != nil {
		a.ctlReported = true // ctlClient said why (no key, declined, refused)
		return nil
	}
	a.Log.Line("CMD api %s: %d line(s) (GET /settings, GET /state, PATCH /settings)", c.Host, len(lines))
	run := func() ([]string, error) {
		ctx, cancel := context.WithTimeout(a.Ctx, ctlTimeout)
		defer cancel()
		return ctl.Exchange(ctx, a.ctlCl, lines)
	}
	out, err := run()
	if errors.Is(err, akamapi.ErrAuth) && a.UI.Interactive() && !a.ctlNoKey {
		a.UI.Err("the camera refused this PC's API key.")
		if k, e := a.enrol1080(c, true); e == nil {
			a.setCtlClient(c.Host, k)
			out, err = run()
		}
	}
	if err != nil {
		a.Log.Line("API exchange failed: %v (kept %d answer line(s))", err, len(out))
		if !errors.Is(err, ui.ErrFailed) {
			a.ctlFail(err)
		}
		a.ctlReported = true
	}
	return out
}

// ctlNoAnswer is the "the camera did not answer" failure of a command that read or set through ctlExchange. When the
// exchange already said why it failed (no API key, refused key, port closed), nothing more is printed: a second,
// vaguer error would only mislead.
func (a *App) ctlNoAnswer(msg, hint string) error {
	if a.ctlReported {
		return ui.ErrFailed
	}
	return a.UI.Fail(msg, hint)
}

// cmdAPIKey is `api-key [replace]`: make (or replace) this PC's API key of the camera. On a 2K it is key-2k.
func (a *App) cmdAPIKey(args []string) error {
	if a.is2K() {
		return a.cmdKey2K()
	}
	replace := false
	switch {
	case len(args) == 0:
	case len(args) == 1 && args[0] == "replace":
		replace = true
	case len(args) == 1 && args[0] == "status":
	default:
		return usageErr(fmt.Sprintf("akam: api-key takes no word, status or replace (see %s help)", Prog))
	}
	a.banner("The API key of this PC for the camera")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD read the key file of this camera's state folder (" + a.apiKeyPath() + ") and ask the camera for its info (GET /info).")
		if replace {
			a.UI.Say("  WOULD ask, then run over SSH: " + apiKeyRevokeCmd1080 + " and " + apiKeyNewCmd1080 + " (the secret is stored in " + a.apiKeyPath() + ", never shown).")
		} else {
			a.UI.Say("  WOULD, if this PC has no key, ask, then run over SSH: " + apiKeyNewCmd1080 + " (the secret is stored in " + a.apiKeyPath() + ", never shown).")
		}
		return nil
	}
	k, have, err := a.apiKey2K()
	if err != nil {
		return a.UI.Fail("the API key cannot be used: "+ui.Sanitize(err.Error())+".", "delete the file "+a.apiKeyPath()+" and run this command again.")
	}
	if have && !replace {
		ctx, cancel := context.WithTimeout(a.Ctx, 10*time.Second)
		in, err := a.camAPI(ip, k).Info(ctx)
		cancel()
		if err == nil {
			a.UI.Good(fmt.Sprintf("  This PC has the API key %s and the camera accepts it (%s, firmware %s).", k.ID, ui.Sanitize(in.Model), ui.Sanitize(in.App)))
			return nil
		}
		if !errors.Is(err, akamapi.ErrAuth) && !errors.Is(err, akamapi.ErrClosed) {
			a.ctlFail(err)
			return ui.ErrFailed
		}
		a.ctlFail(err)
		replace = true
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	nk, err := a.enrol1080(c, replace)
	if err != nil {
		return err
	}
	a.setCtlClient(ip, nk)
	a.UI.Say("  The settings commands and the options screens use this key from now on.")
	return nil
}
