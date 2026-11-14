package cli

import (
	"bytes"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"regexp"
	"strconv"
	"strings"
	"unicode"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// This file: debug, onvif, hostname, rtsp-auth, the commands that run a camera helper script
// over SSH (steps_camera.sh). Secrets go on stdin only.

// ---------------------------------------------------------------- debug

func (a *App) cmdDebug(args []string) error {
	action := "status"
	if len(args) > 0 {
		action = args[0]
	}
	min := ""
	switch action {
	case "on":
		if len(args) > 2 {
			return usageErr(fmt.Sprintf("akam: debug on takes at most one number of minutes (see %s help)", Prog))
		}
		if len(args) == 2 {
			min = args[1]
			n, err := strconv.Atoi(min)
			if err != nil || !allDigits(min) || n < 1 || n > 1440 {
				return usageErr(fmt.Sprintf("akam: debug on: MINUTES must be a whole number from 1 to 1440 (see %s help)", Prog))
			}
		}
	case "off", "status":
		if len(args) > 1 {
			return usageErr(fmt.Sprintf("akam: debug %s takes no argument (see %s help)", action, Prog))
		}
	default:
		return usageErr(fmt.Sprintf("akam: debug needs on [MINUTES], off or status (see %s help)", Prog))
	}
	remote := remoteDebug(action, min)
	a.banner("Debug mode of the control port: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		if action == "on" {
			a.UI.Say("  WOULD ask: Let anyone on your network change the camera settings through port 8091? [y/N]")
		}
		a.UI.Say("  WOULD run over SSH on the camera: " + remote)
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if action == "on" {
		if err := a.UI.RequireTTY("allowing writes on the control port"); err != nil {
			return err
		}
		ok, _ := a.UI.AskYN("  Let anyone on your network change the camera settings through port 8091 for a while?")
		if !ok {
			a.UI.Say("  Left as it was (read-only).")
			return nil
		}
	}
	res, err := a.run(c, remote, nil, "")
	if err != nil {
		return err
	}
	out := strings.TrimRight(res.Combined(), "\n")
	if res.Status != 0 {
		if out != "" {
			a.UI.Say("  " + out)
		}
		return a.UI.Fail(fmt.Sprintf("the camera did not run %s %s.", debugCmd, action),
			fmt.Sprintf("it may run an older Akam image without the debug command (%s update), or ak_rtsp may not be running.", Prog))
	}
	a.UI.Say("  " + out)
	switch action {
	case "on":
		a.UI.Warn(fmt.Sprintf("anyone on your network can now change the camera settings through port 8091 until it is switched off: %s debug off.", Prog))
	case "off":
		a.UI.Good("  Port 8091 is read-only for the network again.")
	}
	return nil
}

func allDigits(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------- onvif

func (a *App) onvifRemote(c *camssh.Client, args ...string) error {
	remote := remoteOnvif(args...)
	res, err := a.run(c, remote, nil, "")
	if err != nil {
		return err
	}
	out := strings.TrimRight(res.Combined(), "\n")
	if out != "" {
		for _, l := range strings.Split(out, "\n") {
			a.UI.Say("  " + l)
		}
	}
	if res.Status != 0 {
		if strings.Contains(out, "not found") || strings.Contains(out, "No such file") {
			return a.UI.Fail(fmt.Sprintf("this camera does not have %s.", onvifCmd),
				fmt.Sprintf("it runs an older Akam image: %s update (ONVIF arrives in the same release as this command).", Prog))
		}
		return a.UI.Fail(fmt.Sprintf("the camera did not run %s %s.", onvifCmd, strings.Join(args, " ")), "read the lines above.")
	}
	return nil
}

func (a *App) cmdOnvif(args []string) error {
	action := "status"
	if len(args) > 0 {
		action = args[0]
	}
	port := ""
	switch action {
	case "enable":
		if len(args) > 2 {
			return usageErr(fmt.Sprintf("akam: onvif enable takes at most one PORT (see %s help)", Prog))
		}
		if len(args) == 2 {
			port = args[1]
			n, err := strconv.Atoi(port)
			if !allDigits(port) || len(port) > 5 || err != nil || n < 1 || n > 65535 {
				return usageErr(fmt.Sprintf("akam: onvif enable: PORT must be a whole number from 1 to 65535 (see %s help)", Prog))
			}
		}
	case "disable", "status":
		if len(args) > 1 {
			return usageErr(fmt.Sprintf("akam: onvif %s takes no argument (see %s help)", action, Prog))
		}
	default:
		return usageErr(fmt.Sprintf("akam: onvif needs enable [PORT], disable or status (see %s help)", Prog))
	}
	a.banner("ONVIF: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	rargs := []string{action}
	if port != "" {
		rargs = append(rargs, port)
	}
	if a.UI.Dry() {
		if action == "enable" {
			a.UI.Say("  WOULD ask: Make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager)? [y/N]")
		}
		a.UI.Say("  WOULD run over SSH on the camera: " + remoteOnvif(rargs...))
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	if action == "enable" {
		if err := a.UI.RequireTTY("turning ONVIF on"); err != nil {
			return err
		}
		p := port
		if p == "" {
			p = "80"
		}
		a.UI.Say(fmt.Sprintf("  This opens port %s (TCP) and UDP 3702 on your network, and needs the stream password.", p))
		ok, _ := a.UI.AskYN("  Make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager)?")
		if !ok {
			a.UI.Say("  Left as it was (ONVIF off).")
			return nil
		}
	}
	if err := a.onvifRemote(c, rargs...); err != nil {
		return err
	}
	switch action {
	case "enable":
		p := port
		if p == "" {
			p = "80"
		}
		a.UI.Good(fmt.Sprintf("  ONVIF is on. In your app, add the camera by its address (port %s unless you chose another), with your stream user and password.", p))
	case "disable":
		a.UI.Good("  ONVIF is off: the ONVIF ports are closed again.")
	}
	return nil
}

// ---------------------------------------------------------------- hostname

const hostnameMax = 32

func hostnameValid(n string) bool {
	if n == "" || strings.HasPrefix(n, "-") || strings.HasSuffix(n, "-") || len(n) > hostnameMax {
		return false
	}
	for _, r := range n {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '-') {
			return false
		}
	}
	return true
}

// hostnameApply is cam_hostname_apply; returns nil also when the firmware lacks the command.
func (a *App) hostnameApply(c *camssh.Client, name string) error {
	if !hostnameValid(name) {
		a.UI.Warn(fmt.Sprintf("'%s' is not a valid network name (lowercase letters, digits and '-', at most %d).", name, hostnameMax))
		return ui.ErrFailed
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD run over SSH: test -x %s, then %s set %s (applies after the next power cycle).", hostnameCmd, hostnameCmd, name))
		return nil
	}
	res, err := a.run(c, remoteTestX(hostnameCmd), nil, "")
	if err != nil {
		return err
	}
	if res.Status != 0 {
		a.UI.Say(fmt.Sprintf("  Note: this firmware does not have %s yet, so the camera keeps its default network name.", hostnameCmd))
		a.UI.Say(fmt.Sprintf("  Update to a newer Akam image, then run: %s hostname %s", Prog, name))
		return nil
	}
	res, err = a.run(c, remoteHostnameSet(name), nil, "")
	if err != nil {
		return err
	}
	if res.Status != 0 {
		a.UI.Warn("setting the network name failed: " + lastLines(res.Combined(), 3))
		return ui.ErrFailed
	}
	a.UI.Good(fmt.Sprintf("  The camera's network name is set to '%s'. It applies after the next power cycle (unplug, wait 5 seconds, plug back).", name))
	return nil
}

// lastLines is `tail -n N | tr '\n' ' '` with the output's trailing newline dropped first, and
// the final text trimmed of nothing else (the bash command substitution removed the newline).
func lastLines(s string, n int) string {
	s = strings.TrimRight(s, "\n")
	if s == "" {
		return ""
	}
	l := strings.Split(s, "\n")
	if len(l) > n {
		l = l[len(l)-n:]
	}
	return strings.Join(l, " ")
}

func (a *App) cmdHostname(name string) error {
	a.banner("Set the camera's network name")
	a.UI.Say("  Why: your router will list the camera under this name instead of a number. Risk: none (a small file on the camera).")
	if name == "" && a.Cams.Name != "default" {
		name = a.Cams.Name
	}
	if name == "" {
		if a.UI.Dry() {
			name = "<name>"
		} else {
			v, err := a.UI.Prompt("  Network name for the camera (lowercase letters, digits, '-'):", "")
			if err != nil {
				return a.UI.Fail("no input.", fmt.Sprintf("run %s hostname NAME", Prog))
			}
			name = v
		}
	}
	if !a.UI.Dry() && !hostnameValid(name) {
		return a.UI.Fail(fmt.Sprintf("'%s' is not a valid network name.", name),
			fmt.Sprintf("use lowercase letters, digits and '-' (at most %d, not starting or ending with '-').", hostnameMax))
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		return a.hostnameApply(nil, name)
	}
	c, err := a.connect(ip,
		fmt.Sprintf("SSH to root@%s with the key %s does not work.", ip, a.keyPath()),
		"the camera must run Akam and be on the network (--identity FILE, --ip ADDRESS).")
	if err != nil {
		return err
	}
	defer c.Close()
	return a.hostnameApply(c, name)
}

// ---------------------------------------------------------------- rtsp-auth

const (
	rtspMinPass = 8
	rtspMaxPass = 128
)

var rtspUserRe = regexp.MustCompile(`^[A-Za-z0-9._@+-]{1,32}$`)

func rtspUserValid(u string) bool { return rtspUserRe.MatchString(u) }

func (a *App) rtspRemote(c *camssh.Client, stdin []byte, args ...string) (res camssh.Result, err error) {
	var in *bytes.Reader
	if stdin != nil {
		in = bytes.NewReader(stdin)
	}
	cmd := remoteRtspAuth(args...)
	if in == nil {
		return a.run(c, cmd, nil, "")
	}
	return a.run(c, cmd, in, "")
}

// rtspStatus is rtsp_status: state is enabled | disabled | invalid | unknown, text the camera's words.
func (a *App) rtspStatus(c *camssh.Client) (state, text string) {
	res, err := a.rtspRemote(c, nil, "status")
	if err != nil {
		return "unknown", ""
	}
	text = strings.TrimRight(res.Combined(), "\n")
	switch {
	case strings.HasPrefix(text, "enabled"):
		state = "enabled"
	case strings.HasPrefix(text, "disabled"):
		state = "disabled"
	case strings.HasPrefix(text, "INVALID"):
		state = "invalid"
	default:
		state = "unknown"
	}
	return
}

func (a *App) rtspStreamHint(user, ip string) {
	a.UI.Say(fmt.Sprintf("  Stream URL: rtsp://%s:PASSWORD@%s:%d/", user, ip, a.Env.PortRTSP))
	a.UI.Say("  Characters such as @ : / ? # % and spaces in the password must be URL-encoded in that address")
	a.UI.Say("  (@ = %40, : = %3A, / = %2F, ? = %3F, # = %23, % = %25): see docs/troubleshooting.md.")
}

func hasControl(s string) bool {
	for _, r := range s {
		if unicode.IsControl(r) {
			return true
		}
	}
	return false
}

// rtspAuthSet asks a user name and a password twice (no echo) and sends them on stdin to
// akam_rtsp_auth.sh set-creds. c may be nil in a rehearsal.
func (a *App) rtspAuthSet(c *camssh.Client, ip string) error {
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD ask a user name and a password (twice, not shown, %d to %d characters).", rtspMinPass, rtspMaxPass))
		a.UI.Say("  WOULD send the user name and the password (two lines, on stdin, never in a command line) to the camera:")
		a.UI.Say(fmt.Sprintf("  WOULD run: ssh root@%s %s set-creds", ip, rtspAuthCmd))
		a.UI.Say("  WOULD tell you that the password is now also stored on the camera so that ONVIF can verify it.")
		return nil
	}
	user, err := a.UI.Prompt("  User name for the video stream [viewer]:", "viewer")
	if err != nil {
		return a.UI.Fail("no input.", "")
	}
	if !rtspUserValid(user) {
		return a.UI.Fail("invalid user name.", "use 1 to 32 letters, digits or . _ @ + - (no spaces, no colon).")
	}
	pw, err := a.UI.Secret(fmt.Sprintf("  Password (%d to %d bytes, not shown):", rtspMinPass, rtspMaxPass))
	if err != nil {
		return a.UI.Fail("no input.", "")
	}
	pw2, err := a.UI.Secret("  Type it again:")
	if err != nil {
		return a.UI.Fail("no input.", "")
	}
	if pw != pw2 {
		return a.UI.Fail("the two passwords differ.", "nothing was changed; try again.")
	}
	if len(pw) < rtspMinPass {
		return a.UI.Fail("the password is too short.", fmt.Sprintf("use at least %d bytes; nothing was changed.", rtspMinPass))
	}
	if len(pw) > rtspMaxPass {
		return a.UI.Fail("the password is too long.", fmt.Sprintf("use at most %d bytes; nothing was changed.", rtspMaxPass))
	}
	if hasControl(pw) {
		return a.UI.Fail("the password contains a control character (tab, ...).", "use printable characters only; nothing was changed.")
	}
	a.Secrets.Add(pw)
	a.UI.Say("  Sending the login over SSH ...")
	payload := []byte(user + "\n" + pw + "\n")
	res, err := a.run(c, remoteRtspAuth("set-creds"), bytes.NewReader(payload), "")
	for i := range payload {
		payload[i] = 0
	}
	if err != nil || res.Status != 0 {
		errText := ""
		if err == nil {
			errText = headLines(string(res.Stderr), 3)
		}
		return a.UI.Fail(fmt.Sprintf("the camera refused it: %s", errText),
			fmt.Sprintf("does this image have %s set-creds? Build and flash a current image (%s update).", rtspAuthCmd, Prog))
	}
	a.UI.Good(fmt.Sprintf("  The video stream now asks for a user name and password (user: %s).", user))
	a.secretsAfterSet(profile.AuthPassword, "", user, pw, true) // a copy saved for this camera is now out of date
	a.UI.Say("  This password is now also stored on the camera (a file only root can read) so that ONVIF can verify it.")
	a.UI.Say("  Players that are watching are disconnected within a second and ask again.")
	a.rtspStreamHint(user, ip)
	return nil
}

// headLines is `head -n N | tr '\n' ' '`: every line followed by a space.
func headLines(s string, n int) string {
	if s == "" {
		return ""
	}
	l := strings.Split(strings.TrimRight(s, "\n"), "\n")
	if len(l) > n {
		l = l[:n]
	}
	return strings.Join(l, " ") + " "
}

func (a *App) rtspAuthDisable(c *camssh.Client, ip string) error {
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask: Switch the password off? Anyone on your network could then watch the camera. [y/N]")
		a.UI.Say(fmt.Sprintf("  WOULD run: ssh root@%s %s disable (removes the RTSP login and the stored password copy)", ip, rtspAuthCmd))
		return nil
	}
	if err := a.UI.RequireTTY("switching the stream password off"); err != nil {
		return err
	}
	ok, _ := a.UI.Danger("Switch the video stream password off", "Switch the video stream password off?",
		fmt.Sprintf("Camera: %s", ip),
		"Anyone on your network could then watch the camera.",
		"This removes the RTSP login and the copy of the password stored for ONVIF.")
	if !ok {
		a.UI.Say("  Left as it was.")
		return nil
	}
	res, err := a.rtspRemote(c, nil, "disable")
	if err == nil {
		// the helper's own words go to the terminal, like ssh without redirection
		// (cleaned: a camera must not be able to move the cursor or hide lines on the terminal)
		if o := strings.TrimRight(textsafe.Clean(string(res.Stdout)+string(res.Stderr)), "\n"); o != "" {
			a.UI.Say(o)
		}
	}
	if err != nil || res.Status != 0 {
		return a.UI.Fail("the camera could not remove the credentials.", fmt.Sprintf("does this image have %s? (%s update)", rtspAuthCmd, Prog))
	}
	a.UI.Good("  The video stream is open again (no password; the copy stored for ONVIF is removed too).")
	a.secretsAfterSet(profile.AuthPassword, "", "", "", false)
	return nil
}

func (a *App) cmdRtspAuth(action string) error {
	if action != "set" && action != "disable" && action != "status" {
		return usageErr(fmt.Sprintf("akam: rtsp-auth needs set, disable or status (see %s help)", Prog))
	}
	a.banner("RTSP password: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		switch action {
		case "status":
			a.UI.Say(fmt.Sprintf("  WOULD run (read-only): ssh root@%s %s status", ip, rtspAuthCmd))
		case "set":
			return a.rtspAuthSet(nil, ip)
		case "disable":
			return a.rtspAuthDisable(nil, ip)
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	switch action {
	case "status":
		state, text := a.rtspStatus(c)
		switch state {
		case "enabled":
			a.UI.Say(fmt.Sprintf("  RTSP password: %s. Players must log in (Digest).", text))
			if strings.Contains(text, "not ONVIF-ready") {
				a.UI.Say(fmt.Sprintf("  No stored copy of the password for ONVIF: run %s rtsp-auth set again to make it ONVIF-ready.", Prog))
			}
		case "disabled":
			a.UI.Say("  RTSP password: disabled. The stream is open to anyone on your network.")
		case "invalid":
			a.UI.Warn(fmt.Sprintf("the credentials file is invalid: the camera refuses every protected request. Run %s rtsp-auth set or disable.", Prog))
			return ui.ErrFailed
		default:
			return a.UI.Fail(fmt.Sprintf("the camera did not answer like Akam (%s missing?).", rtspAuthCmd),
				fmt.Sprintf("update the image (%s update) to get the RTSP password command.", Prog))
		}
	case "set":
		return a.rtspAuthSet(c, ip)
	case "disable":
		return a.rtspAuthDisable(c, ip)
	}
	return nil
}
