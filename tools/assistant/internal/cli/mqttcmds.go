package cli

import (
	"bytes"
	"fmt"
	"regexp"
	"strconv"
	"strings"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
)

// akam mqtt [status | set | disable | forget | test]: the optional MQTT client of the camera (Home Assistant
// discovery, specs/mqtt.md), managed over SSH with akam_mqtt.sh. Off by default. The broker password is
// typed without echo and travels on the SSH channel's standard input only: never in a command line, a log or
// the state file.

const (
	mqttPassMax = 128
	mqttUserMax = 32
)

var (
	mqttHostRe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9.-]{0,62}$`)
	mqttUserRe = regexp.MustCompile(`^[A-Za-z0-9._@+-]{1,32}$`)
)

func mqttHostValid(h string) bool { return mqttHostRe.MatchString(h) }

func mqttPortValid(p string) bool {
	n, err := strconv.Atoi(p)
	return allDigits(p) && len(p) <= 5 && err == nil && n >= 1 && n <= 65535
}

// mqttRemote runs akam_mqtt.sh with args and the optional stdin, prints what it says (indented) and returns the
// camera's status as an error when it is not 0.
func (a *App) mqttRemote(c *camssh.Client, stdin []byte, args ...string) error {
	remote := remoteMqtt(args...)
	var in *bytes.Reader
	if stdin != nil {
		in = bytes.NewReader(stdin)
	}
	var res camssh.Result
	var err error
	if in == nil {
		res, err = a.run(c, remote, nil, "")
	} else {
		res, err = a.run(c, remote, in, "")
	}
	for i := range stdin {
		stdin[i] = 0
	}
	if err != nil {
		return a.UI.Fail(err.Error(), "")
	}
	out := strings.TrimRight(res.Combined(), "\n")
	if out != "" {
		for _, l := range strings.Split(out, "\n") {
			a.UI.Say("  " + plainText(l))
		}
	}
	if res.Status != 0 {
		if strings.Contains(out, "not found") || strings.Contains(out, "No such file") {
			return a.UI.Fail(fmt.Sprintf("this camera does not have %s.", mqttCmd),
				fmt.Sprintf("it runs an older Akam image: %s update (MQTT arrives in the same release as this command).", Prog))
		}
		return a.UI.Fail(fmt.Sprintf("the camera did not run %s %s.", mqttCmd, args[0]), "read the lines above.")
	}
	return nil
}

// mqttState reads "MQTT: enabled" / "MQTT: disabled" from the helper: on, off or unknown (read-only).
func (a *App) mqttState(c *camssh.Client) string {
	res, err := a.run(c, remoteMqtt("status"), nil, "")
	if err != nil {
		return "unknown"
	}
	switch first := strings.TrimSpace(strings.SplitN(string(res.Stdout), "\n", 2)[0]); {
	case strings.HasPrefix(first, "MQTT: enabled"):
		return "on"
	case strings.HasPrefix(first, "MQTT: disabled"):
		return "off"
	}
	return "unknown"
}

func (a *App) cmdMqtt(args []string) error {
	action := "status"
	if len(args) > 0 {
		action = args[0]
	}
	switch action {
	case "status", "disable", "forget", "test", "set":
		if len(args) > 1 {
			return usageErr(fmt.Sprintf("akam: mqtt %s takes no argument (see %s help)", action, Prog))
		}
	default:
		return usageErr(fmt.Sprintf("akam: mqtt needs status, set, disable, forget or test (see %s help)", Prog))
	}
	a.banner("MQTT: " + action)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		switch action {
		case "set":
			a.UI.Say("  WOULD ask the broker address, port, user name and password (the password is not shown and goes on stdin, never in a command line).")
			a.UI.Say("  WOULD ask: Turn MQTT on with these settings? [y/N] (default: No)")
			a.UI.Say("  WOULD run over SSH on the camera: " + mqttCmd + " set <host> <port> <user> (password on stdin)")
		case "forget":
			a.UI.Say("  WOULD ask: Delete the saved MQTT settings and password from the camera? [y/N] (default: No)")
			a.UI.Say("  WOULD run over SSH on the camera: " + remoteMqtt(action))
		default:
			a.UI.Say("  WOULD run over SSH on the camera: " + remoteMqtt(action))
		}
		return nil
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	switch action {
	case "set":
		return a.mqttSet(c)
	case "forget":
		if err := a.UI.RequireTTY("deleting the MQTT settings"); err != nil {
			return err
		}
		ok, _ := a.UI.AskYN("  Delete the saved MQTT settings and the broker password from the camera?")
		if !ok {
			a.UI.Say("  Left as it was.")
			return nil
		}
		if err := a.mqttRemote(c, nil, "forget"); err != nil {
			return err
		}
		a.UI.Good("  MQTT is off and the settings are deleted. In Home Assistant, delete the device by hand if you do not want it any more.")
		a.secretsAfterSet(profile.MQTTPassword, "", "", "", false)
	case "disable":
		if err := a.mqttRemote(c, nil, "disable"); err != nil {
			return err
		}
		a.UI.Good("  MQTT is off: the camera makes no connection to the broker any more.")
	case "test":
		if err := a.mqttRemote(c, nil, "test"); err != nil {
			return err
		}
	default:
		return a.mqttRemote(c, nil, "status")
	}
	return nil
}

// mqttSet asks for the broker, shows what turning MQTT on means, asks (default No) and sends the settings; the
// password goes on stdin of the camera helper.
func (a *App) mqttSet(c *camssh.Client) error {
	if err := a.UI.RequireTTY("turning MQTT on"); err != nil {
		return err
	}
	a.UI.Say("  MQTT lets the camera publish its state to a broker (for example Home Assistant) and take a few commands from it.")
	a.UI.Say("  The camera connects out to the broker: no new port is opened on the camera. The connection is plain TCP,")
	a.UI.Say("  so the broker password travels in clear on your network. The broker must be on your own network.")
	a.UI.Say("  Use its IP address, or a name your router's DNS knows: names ending in .local (mDNS) do not work on the camera.")
	host, err := a.UI.Prompt("  Broker address (host name or IPv4 address):", "")
	if err != nil || host == "" {
		return a.UI.Fail("no broker address.", "nothing was changed.")
	}
	if !mqttHostValid(host) {
		return a.UI.Fail("invalid broker address.", "use a host name or an IPv4 address (letters, digits, . and -, at most 63); nothing was changed.")
	}
	port, err := a.UI.Prompt("  Broker port [1883]:", "1883")
	if err != nil {
		return a.UI.Fail("no input.", "nothing was changed.")
	}
	if !mqttPortValid(port) {
		return a.UI.Fail("invalid port.", "use a whole number from 1 to 65535; nothing was changed.")
	}
	user, err := a.UI.Prompt("  Broker user name (empty for none):", "")
	if err != nil {
		return a.UI.Fail("no input.", "nothing was changed.")
	}
	if user != "" && (user == "-" || !mqttUserRe.MatchString(user)) {
		return a.UI.Fail("invalid user name.", fmt.Sprintf("use 1 to %d letters, digits or . _ @ + - (no spaces, no colon); nothing was changed.", mqttUserMax))
	}
	pw := ""
	if user != "" {
		pw, err = a.UI.Secret(fmt.Sprintf("  Broker password (not shown, at most %d characters, empty for none):", mqttPassMax))
		if err != nil {
			return a.UI.Fail("no input.", "nothing was changed.")
		}
		if len(pw) > mqttPassMax {
			return a.UI.Fail("the password is too long.", fmt.Sprintf("use at most %d characters; nothing was changed.", mqttPassMax))
		}
		if hasControl(pw) {
			return a.UI.Fail("the password contains a control character (tab, ...).", "use printable characters only; nothing was changed.")
		}
		if pw != "" {
			a.Secrets.Add(pw)
		}
	}
	ok, _ := a.UI.AskYN("  Turn MQTT on with these settings?")
	if !ok {
		a.UI.Say("  Left as it was (MQTT off).")
		return nil
	}
	userArg := user
	if userArg == "" {
		userArg = "-"
	}
	a.UI.Say("  Sending the settings over SSH ...")
	payload := []byte(pw + "\n")
	if err := a.mqttRemote(c, payload, "set", host, port, userArg); err != nil {
		return err
	}
	a.UI.Good("  MQTT is on. Home Assistant finds the camera by itself (MQTT integration, discovery on).")
	a.secretsAfterSet(profile.MQTTPassword, host, user, pw, user != "") // a copy saved for this camera is now out of date
	a.UI.Say(fmt.Sprintf("  Check it with: %s mqtt test    Turn it off with: %s mqtt disable", Prog, Prog))
	if ok, _ := a.UI.AskYN("  Test the connection to the broker now?"); ok {
		return a.mqttRemote(c, nil, "test")
	}
	return nil
}
