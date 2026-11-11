package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The MQTT commands run the REAL akam_mqtt.sh of this checkout in the fake camera's root (the fake ak_rtsp answers
// MQTT reload/test), so these tests cover the whole path: prompts, validation, stdin password, helper, files.

const mqttPass = "S3NTINEL-br0ker-pw"

func mqttRoot(e *env) string { return filepath.Join(e.cam.Root, "etc", "config", "akam") }

func mqttFile(t *testing.T, e *env, name string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(mqttRoot(e), name))
	if err != nil {
		return ""
	}
	return string(b)
}

func sentToCamera(e *env, sub string) bool {
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, sub) {
			return true
		}
	}
	return false
}

func TestMqttStatusSetTestDisableForget(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	r := e.run("", "mqtt")
	expectRC(t, r, 0)
	contains(t, r.out, "  MQTT: disabled (default)", "  password: none")

	// set: host, port (Enter = 1883), user, password, yes, then "test now": Enter = No
	r = e.run("192.0.2.10\n\nbob\n"+mqttPass+"\ny\n\n", "mqtt", "set")
	expectRC(t, r, 0)
	contains(t, r.out, "plain TCP", "  MQTT enabled: broker 192.0.2.10:1883, user bob, password set.",
		"  MQTT is on. Home Assistant finds the camera by itself")
	if got := mqttFile(t, e, "mqtt"); got != "enabled=1\nhost=192.0.2.10\nport=1883\nuser=bob\n" {
		t.Errorf("settings file: %q", got)
	}
	if got := mqttFile(t, e, "mqtt_pass"); got != mqttPass+"\n" {
		t.Errorf("password file: %q", got)
	}
	st, err := os.Stat(filepath.Join(mqttRoot(e), "mqtt_pass"))
	if err != nil || st.Mode().Perm() != 0o600 {
		t.Errorf("password file mode: %v %v", st, err)
	}
	// the password is on stdin only: not in any command line, output, state or log
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, mqttPass) {
			t.Errorf("the password is in a command line: %q", c)
		}
	}
	if !sentToCamera(e, "/usr/sbin/akam_mqtt.sh set 192.0.2.10 1883 bob") {
		t.Errorf("no set command: %v", e.cam.Commands())
	}
	if strings.Contains(r.out+r.err, mqttPass) || strings.Contains(e.logText(), mqttPass) {
		t.Error("the password leaked into the output or the log")
	}
	filepath.Walk(e.state, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() {
			if b, _ := os.ReadFile(p); strings.Contains(string(b), mqttPass) {
				t.Errorf("the password is in %s", p)
			}
		}
		return nil
	})

	r = e.run("", "mqtt", "status")
	expectRC(t, r, 0)
	contains(t, r.out, "  MQTT: enabled", "  broker: 192.0.2.10:1883, user bob", "  password: set", "  state: connected")
	if strings.Contains(r.out, mqttPass) {
		t.Error("status shows the password")
	}

	// test: ok, then a broker that refuses this password
	r = e.run("", "mqtt", "test")
	expectRC(t, r, 0)
	contains(t, r.out, "  MQTT test: ok (the broker at 192.0.2.10:1883 accepted the login).")
	expectRC(t, e.run("192.0.2.10\n1883\nbob\nreject-me-please\ny\ny\n", "mqtt", "set"), 1)
	r = e.run("", "mqtt", "test")
	expectRC(t, r, 1)
	contains(t, r.out, "MQTT test: FAILED: the broker refused the user name or password.")
	contains(t, r.err, "the camera did not run /usr/sbin/akam_mqtt.sh test.")

	// disable keeps the settings; forget asks (default No) and deletes
	r = e.run("", "mqtt", "disable")
	expectRC(t, r, 0)
	contains(t, r.out, "  MQTT disabled (the broker settings are kept", "  MQTT is off: the camera makes no connection to the broker any more.")
	if !strings.HasPrefix(mqttFile(t, e, "mqtt"), "enabled=0\nhost=192.0.2.10\n") || mqttFile(t, e, "mqtt_pass") == "" {
		t.Errorf("disable: %q", mqttFile(t, e, "mqtt"))
	}
	for _, in := range []string{"", "\n", "n\n", "q\n", "maybe\n"} {
		r = e.run(in, "mqtt", "forget")
		expectRC(t, r, 0)
		contains(t, r.out, "  Left as it was.")
	}
	if mqttFile(t, e, "mqtt_pass") == "" {
		t.Error("forget without a yes deleted the password")
	}
	r = e.run("y\n", "mqtt", "forget")
	expectRC(t, r, 0)
	if mqttFile(t, e, "mqtt") != "" || mqttFile(t, e, "mqtt_pass") != "" {
		t.Error("forget did not delete the files")
	}
}

func TestMqttSetDefaultsToNoAndNeedsATerminal(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	for _, last := range []string{"", "\n", "n\n", "N\n", "q\n", "no\n", "maybe\n"} {
		r := e.run("192.0.2.10\n1883\nbob\n"+mqttPass+"\n"+last, "mqtt", "set")
		expectRC(t, r, 0)
		contains(t, r.out, "  Left as it was (MQTT off).")
	}
	if sentToCamera(e, "akam_mqtt.sh set") || mqttFile(t, e, "mqtt") != "" || mqttFile(t, e, "mqtt_pass") != "" {
		t.Error("something was sent or written without a yes")
	}
	t.Setenv("AKAM_FORCE_TTY", "")
	r := e.run("192.0.2.10\n1883\nbob\n"+mqttPass+"\ny\n", "mqtt", "set")
	expectRC(t, r, 1)
	contains(t, r.err, "needs an interactive terminal and refuses to run without one.")
	r = e.run("y\n", "mqtt", "forget")
	expectRC(t, r, 1)
	t.Setenv("AKAM_FORCE_TTY", "1")
	if len(e.cam.Commands()) > 0 {
		for _, c := range e.cam.Commands() {
			if strings.Contains(c, "akam_mqtt.sh set") || strings.Contains(c, "akam_mqtt.sh forget") {
				t.Errorf("sent without a terminal: %q", c)
			}
		}
	}
}

func TestMqttInputIsValidatedBeforeAnythingIsSent(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	long := strings.Repeat("p", 129)
	cases := []struct{ in, msg string }{
		{"\n", "no broker address."},
		{"bad host\n", "invalid broker address."},
		{"-h\n", "invalid broker address."},
		{"a/b\n", "invalid broker address."},
		{strings.Repeat("a", 64) + "\n", "invalid broker address."},
		{"h\n0\n", "invalid port."},
		{"h\n65536\n", "invalid port."},
		{"h\nabc\n", "invalid port."},
		{"h\n1883\nbad user\n", "invalid user name."},
		{"h\n1883\na:b\n", "invalid user name."},
		{"h\n1883\n" + strings.Repeat("u", 33) + "\n", "invalid user name."},
		{"h\n1883\nbob\n" + long + "\n", "the password is too long."},
		{"h\n1883\nbob\nab\tc\n", "control character"},
	}
	for _, c := range cases {
		r := e.run(c.in+"y\n", "mqtt", "set")
		expectRC(t, r, 1)
		contains(t, r.err, c.msg)
	}
	if sentToCamera(e, "akam_mqtt.sh set") {
		t.Error("an invalid value reached the camera")
	}
	// the longest allowed values go through
	r := e.run(strings.Repeat("a", 63)+"\n65535\n"+strings.Repeat("u", 32)+"\n"+strings.Repeat("p", 128)+"\ny\n\n", "mqtt", "set")
	expectRC(t, r, 0)
	// no user: no password is asked
	r = e.run("broker.lan\n\n\ny\n\n", "mqtt", "set")
	expectRC(t, r, 0)
	contains(t, r.out, "user none, password none")
	if strings.Contains(r.err+r.out, "Broker password") {
		t.Error("a password was asked without a user name")
	}
	for _, bad := range [][]string{{"mqtt", "frob"}, {"mqtt", "set", "x"}, {"mqtt", "status", "x"}, {"mqtt", "disable", "now"}, {"mqtt", "forget", "x"}, {"mqtt", "test", "x"}} {
		expectRC(t, e.run("", bad...), 2)
	}
}

func TestMqttOlderImageHasNoHelper(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.NoMqttHelper = true })
	r := e.run("", "mqtt", "status")
	expectRC(t, r, 1)
	contains(t, r.err, "this camera does not have /usr/sbin/akam_mqtt.sh.")
	r = e.run("192.0.2.10\n1883\nbob\n"+mqttPass+"\ny\n", "mqtt", "set")
	expectRC(t, r, 1)
	if strings.Contains(r.out+r.err+e.logText(), mqttPass) {
		t.Error("the password leaked")
	}
}

func TestMqttDryRunSendsNothing(t *testing.T) {
	e := newEnv(t)
	for _, a := range []string{"status", "set", "disable", "forget", "test"} {
		r := e.run("", "--dry-run", "mqtt", a)
		expectRC(t, r, 0)
		contains(t, r.out, "WOULD")
	}
	if len(e.cam.Commands()) != 0 {
		t.Errorf("a rehearsal sent: %v", e.cam.Commands())
	}
}

func TestOptionsNetworkScreenOffersMqtt(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("3\n8\n9\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "ONVIF: off", "MQTT: off")
	// network > MQTT: the broker questions, default No at the end, nothing sent
	r = e.run("3\n5\n5\n192.0.2.10\n\nbob\n"+mqttPass+"\n\n8\n9\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "MQTT / Home Assistant: off", "Left as it was (MQTT off).")
	if sentToCamera(e, "akam_mqtt.sh set") {
		t.Error("set was sent without a yes")
	}
}
