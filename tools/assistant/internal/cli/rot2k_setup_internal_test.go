package cli

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The install-time questions of the services of a 2K (prepare-2k-sd): every one defaults to No, wrong values are asked again, the two
// passwords never reach the output, and a typo in the second password writes nothing.

const (
	setupStreamPw = "stream-pass-1234"
	setupBrokerPw = "broker-pass-1234"
)

func setupApp(input string) (*App, *strings.Builder) {
	a, _, _ := asksApp(input, false, false, true)
	out := &strings.Builder{}
	a.UI = &ui.Plain{In: a.UI.(*ui.Plain).In, Out: out, ErrOut: out, ForceTT: true}
	a.Secrets = &state.Secrets{}
	return a, out
}

func TestAskSetup2KDefaultsToNothing(t *testing.T) {
	for _, in := range []string{"\n", "n\n", "", "no\n"} {
		a, _ := setupApp(in)
		s, err := a.askSetup2K()
		if err != nil || s != nil {
			t.Errorf("%q: %v %v", in, s, err)
		}
	}
	// "yes" to the first question but No to every service: still nothing
	a, _ := setupApp("y\n\n\n\n\n\n")
	if s, err := a.askSetup2K(); err != nil || s != nil {
		t.Errorf("all No: %v %v", s, err)
	}
}

func TestAskSetup2KEverything(t *testing.T) {
	in := strings.Join([]string{
		"y",                                      // set up services
		"y",                                      // microphone
		"y", "bob", setupStreamPw, setupStreamPw, // stream password
		"y", "8123", // ONVIF + port
		"y", "bad host", "broker.lan", "1884", "ha", setupBrokerPw, // MQTT: the first host is refused and asked again
		"y", "pool.ntp.org time.example.net", // NTP
		"",
	}, "\n")
	a, out := setupApp(in)
	s, err := a.askSetup2K()
	if err != nil || s == nil {
		t.Fatalf("%v %v\n%s", s, err, out)
	}
	if !s.Mic || !s.ONVIF || s.ONVIFPort != 8123 || s.MQTTHost != "broker.lan" || s.MQTTPort != 1884 || s.MQTTUser != "ha" || s.MQTTPass != setupBrokerPw ||
		s.AuthUser != "bob" || s.AuthPass != setupStreamPw || strings.Join(s.NTP, " ") != "pool.ntp.org time.example.net" {
		t.Errorf("setup: %+v", s)
	}
	files, ferr := s.Files()
	if ferr != nil || len(files) != 5 {
		t.Errorf("files: %v %v", files, ferr)
	}
	for _, pw := range []string{setupStreamPw, setupBrokerPw} {
		if strings.Contains(out.String(), pw) {
			t.Errorf("a password reached the output:\n%s", out)
		}
	}
	if !strings.Contains(out.String(), "bad host") && !strings.Contains(out.String(), "host name") {
		t.Errorf("the refused host was not explained:\n%s", out)
	}
	if !strings.Contains(out.String(), "akam.conf, mqtt, mqtt_pass, ntp, rtsp.conf") {
		t.Errorf("the summary lacks the file names:\n%s", out)
	}
}

func TestAskSetup2KRefusals(t *testing.T) {
	for name, in := range map[string]string{
		"two different passwords": "y\nn\ny\nbob\n" + setupStreamPw + "\nsomething-else\n",
		"no stream user":          "y\nn\ny\n\n",
		"no broker":               "y\nn\nn\ny\n\n",
	} {
		a, out := setupApp(in)
		if s, err := a.askSetup2K(); err == nil || s != nil {
			t.Errorf("%s: accepted: %v\n%s", name, s, out)
		}
	}
	// ONVIF is only offered once a stream password was chosen
	a, out := setupApp("y\nn\nn\nn\nn\nn\n")
	if s, _ := a.askSetup2K(); s != nil || strings.Contains(out.String(), "ONVIF (NVR") {
		t.Errorf("ONVIF was offered without a stream password:\n%s", out)
	}
}

func TestAskSetup2KNTPDefault(t *testing.T) {
	a, _ := setupApp("y\nn\nn\nn\ny\n\n") // an empty answer takes the suggested pool
	s, err := a.askSetup2K()
	if err != nil || s == nil || strings.Join(s.NTP, " ") != "pool.ntp.org" {
		t.Errorf("%+v %v", s, err)
	}
}
