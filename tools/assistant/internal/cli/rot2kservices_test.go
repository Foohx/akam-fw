package cli_test

import (
	"strings"
	"testing"
)

// MQTT, ONVIF, the stream password, NTP and the microphone of a 2K through the signed API (the user-file rows of the camera), against the
// in-memory fake of the camera. Host-tested only. The two passwords are write-only: asked without echo, sent in the PATCH body, never shown.

const (
	pw2KMqtt = "broker-s3cret-pass"
	pw2KAuth = "stream-s3cret-pass"
)

func TestServices2KMicrophone(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("", "audio")
	expectRC(t, r, 0)
	contains(t, r.out, "Microphone", "off")
	r = e.run("", "audio", "mic", "on")
	expectRC(t, r, 0)
	contains(t, r.out, "needs a restart")
	if p := f.Patches(); len(p) != 1 || p[0] != `{"audio.mic_enable":"1"}` {
		t.Fatalf("bodies %q", p)
	}
	for _, bad := range [][]string{{"audio", "mic"}, {"audio", "mic", "maybe"}, {"audio", "volume"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	allSigned(t, f)
}

func TestServices2KMqttSetStatusDisableForget(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("broker.lan\n\nha\n"+pw2KMqtt+"\ny\n", "mqtt", "set")
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, pw2KMqtt)
	contains(t, r.out, "needs a restart")
	p := f.Patches()
	if len(p) != 1 || p[0] != `{"mqtt.enabled":"1","mqtt.host":"broker.lan","mqtt.password":"`+pw2KMqtt+`","mqtt.user":"ha"}` /* the port already is 1883: not sent */ {
		t.Fatalf("bodies %q", p)
	}
	if f.Secrets["mqtt.password"] != pw2KMqtt {
		t.Error("the password did not reach the camera")
	}
	r = e.run("", "mqtt")
	expectRC(t, r, 0)
	contains(t, r.out, "broker.lan", "1883", "ha", "write-only")
	notContains(t, r.out+r.err, pw2KMqtt)
	// declining sends nothing
	n := len(f.Patches())
	r = e.run("other.lan\n\n\nn\n", "mqtt", "set")
	expectRC(t, r, 0)
	contains(t, r.out, "Left as it was")
	if len(f.Patches()) != n {
		t.Error("sent without a confirmation")
	}
	r = e.run("", "mqtt", "disable")
	expectRC(t, r, 0)
	if f.Values["mqtt.enabled"] != "0" || f.Values["mqtt.host"] != "broker.lan" {
		t.Errorf("disable keeps the settings: %v", f.Values)
	}
	r = e.run("", "--yes", "mqtt", "forget")
	expectRC(t, r, 0)
	if f.Values["mqtt.host"] != "" || f.Values["mqtt.user"] != "" || f.Secrets["mqtt.password"] != "" {
		t.Errorf("forget: %v %v", f.Values, f.Secrets)
	}
	expectRC(t, e.run("", "mqtt", "test"), 2)
	expectRC(t, e.run("", "mqtt", "bogus"), 2)
	allSigned(t, f)
}

func TestServices2KMqttRefusalsSendNothing(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	for name, in := range map[string]string{
		"bad host":     "bad host\n",
		"empty host":   "\n",
		"bad port":     "b.lan\n70000\n",
		"bad user":     "b.lan\n\n-\n",
		"long pass":    "b.lan\n\nha\n" + strings.Repeat("a", 129) + "\n",
		"control char": "b.lan\n\nha\nbad\tpw\n",
	} {
		r := e.run(in, "mqtt", "set")
		if r.rc == 0 {
			t.Errorf("%s: accepted:\n%s", name, r.out)
		}
	}
	if len(f.Patches()) != 0 {
		t.Errorf("a refused value reached the camera: %v", f.Patches())
	}
}

func TestServices2KOnvifNtpAndStreamPassword(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	// ONVIF without a stream password is refused client-side (the camera refuses it too)
	r := e.run("", "onvif", "enable")
	if r.rc == 0 {
		t.Fatalf("ONVIF enabled without a stream password:\n%s", r.out)
	}
	contains(t, r.out+r.err, "stream")
	if len(f.Patches()) != 0 {
		t.Fatal("sent")
	}
	// the stream password: user, password twice, the confirmation of the danger
	r = e.run("bob\n"+pw2KAuth+"\n"+pw2KAuth+"\ny\n", "rtsp-auth", "set")
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, pw2KAuth)
	p := f.Patches()
	if len(p) != 1 || p[0] != `{"rtsp.password":"`+pw2KAuth+`","rtsp.user":"bob"}` {
		t.Fatalf("bodies %q", p)
	}
	// two different passwords: nothing sent
	r = e.run("bob\n"+pw2KAuth+"\nsomething-else\n", "rtsp-auth", "set")
	if r.rc == 0 || len(f.Patches()) != 1 {
		t.Errorf("a mismatch was accepted:\n%s", r.out)
	}
	r = e.run("", "rtsp-auth")
	expectRC(t, r, 0)
	contains(t, r.out, "bob", "write-only")
	notContains(t, r.out+r.err, pw2KAuth)
	// ONVIF now works, with a port
	r = e.run("", "onvif", "enable", "8123")
	expectRC(t, r, 0)
	if f.Values["onvif.enabled"] != "1" || f.Values["onvif.port"] != "8123" {
		t.Errorf("onvif: %v", f.Values)
	}
	expectRC(t, e.run("", "onvif", "enable", "80"), 2)
	r = e.run("", "onvif", "disable")
	expectRC(t, r, 0)
	if f.Values["onvif.enabled"] != "0" {
		t.Error("onvif not disabled")
	}
	// NTP
	r = e.run("", "ntp", "set", "pool.ntp.org", "time.example.net")
	expectRC(t, r, 0)
	if f.Values["ntp.servers"] != "pool.ntp.org time.example.net" {
		t.Errorf("ntp: %q", f.Values["ntp.servers"])
	}
	expectRC(t, e.run("", "ntp", "set", "a.b", "c.d", "e.f", "g.h"), 2)
	expectRC(t, e.run("", "ntp", "set", "-bad"), 2)
	expectRC(t, e.run("", "ntp", "off"), 0)
	if f.Values["ntp.servers"] != "" {
		t.Error("ntp not off")
	}
	// removing the stream password
	r = e.run("y\n", "rtsp-auth", "disable")
	expectRC(t, r, 0)
	if f.Values["rtsp.user"] != "" {
		t.Error("the stream user was not removed")
	}
	allSigned(t, f)
}

func TestServices2KNeedAnAdminKey(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	f.Scope = "write"
	r := e.run("", "audio", "mic", "on")
	if r.rc == 0 {
		t.Fatalf("a write key changed a user-file row:\n%s", r.out)
	}
	contains(t, r.out+r.err, "scope")
	if f.Values["audio.mic_enable"] != "0" {
		t.Error("changed")
	}
}

func TestServices2KDryRunMasksPasswords(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("", "--dry-run", "--model", rotModel, "mqtt", "set")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD")
	r = e.run("", "--dry-run", "--model", rotModel, "rtsp-auth", "set")
	expectRC(t, r, 0)
	r = e.run("", "--dry-run", "--model", rotModel, "ntp", "set", "pool.ntp.org")
	expectRC(t, r, 0)
	contains(t, r.out, `"ntp.servers":"pool.ntp.org"`)
	if len(f.Patches()) != 0 {
		t.Error("a dry run sent a change")
	}
}

// The Camera options screen reaches the same rows: a write-only row is asked without echo and shown as "(set, hidden)".
func TestServices2KOptionsScreen(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	// 3 options; 8 Security; 1 Stream user -> bob; 2 Stream password -> secret; 3 apply; y; 5 back; 10 back (groups); 10 quit
	r := e.run("3\n8\n1\nbob\n2\n" + pw2KAuth + "\n3\ny\n5\n10\n10\n")
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, pw2KAuth)
	contains(t, r.out, "Stream password", "(set, hidden)", "About to change:")
	p := f.Patches()
	if len(p) != 1 || p[0] != `{"rtsp.user":"bob","rtsp.password":"`+pw2KAuth+`"}` && p[0] != `{"rtsp.password":"`+pw2KAuth+`","rtsp.user":"bob"}` {
		t.Fatalf("bodies %q", p)
	}
	// the microphone in the Audio group
	r = e.run("3\n7\n1\n2\n8\ny\n10\n10\n10\n") // Audio now also holds the six speaker rows: apply is 8 and back 10 (was 2 and 4 with the microphone alone)
	expectRC(t, r, 0)
	if f.Values["audio.mic_enable"] != "1" {
		t.Errorf("microphone: %v\n%s", f.Values["audio.mic_enable"], r.out)
	}
	allSigned(t, f)
}
