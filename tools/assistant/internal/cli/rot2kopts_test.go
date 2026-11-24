package cli_test

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The options of the 2K through its signed API, against the in-memory fake of the camera (testfake.API2K).
// Host-tested only: nothing here talks to a real 2K.

const secret2K = "3fa91c07:" + "ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01"

// with2K makes the env a 2K with an API key on this PC and a fake camera API holding the registry defaults.
func (e *env) with2K(overrides map[string]string) *testfake.API2K {
	e.t.Helper()
	k, err := akamapi.ParseKey(secret2K)
	if err != nil {
		e.t.Fatal(err)
	}
	vals := map[string]string{}
	wo := map[string]bool{}
	for _, o := range camopt.Catalog2K() {
		switch o.Kind {
		case camopt.Int:
			vals[o.Key] = strconv.Itoa(min(max(50, o.Min), o.Max))
		case camopt.Enum, camopt.Bool:
			vals[o.Key] = o.Values[0]
		case camopt.Pattern:
			vals[o.Key] = "on"
		case camopt.Secret:
			wo[o.Key] = true // write-only rows: the camera never reports them
		default:
			vals[o.Key] = "x"
		}
	}
	for k, v := range map[string]string{"video0.kbps": "1536", "video0.max_kbps": "0", "video0.fps": "15", "video0.min_qp": "28", "video0.max_qp": "43",
		"net.hostname": "akam-rot2k", "net.wifi_country": "none", "net.wifi_band": "2.4", "track.lost_policy": "backsweep,exit,hold:30,home",
		"ptz.patrol.route_default": "", "led.mode": "on", "track.return": "home", "image.mains_hz": "50",
		"mqtt.host": "", "mqtt.user": "", "mqtt.port": "1883", "mqtt.enabled": "0", "onvif.enabled": "0", "onvif.port": "8000", "rtsp.user": "", "ntp.servers": "", "audio.mic_enable": "0"} {
		vals[k] = v
	}
	for k, v := range overrides {
		vals[k] = v
	}
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	if err := os.MkdirAll(e.state, 0o700); err != nil {
		e.t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(e.state, "api_key"), []byte(secret2K+"\n"), 0o600); err != nil {
		e.t.Fatal(err)
	}
	f := &testfake.API2K{Key: k, Values: vals, WriteOnly: wo}
	e.api2k = f
	return f
}

func noSecret(t *testing.T, r result) {
	t.Helper()
	notContains(t, r.out+r.err, strings.TrimPrefix(secret2K, "3fa91c07:"))
}

func allSigned(t *testing.T, f *testfake.API2K) {
	t.Helper()
	for _, r := range f.Requests {
		if r.Path != "/akam/v1/hello" && !r.SignedOK {
			t.Errorf("an unsigned or badly signed request reached the camera: %s %s", r.Method, r.Path)
		}
	}
}

func TestOptions2KReads(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(map[string]string{"track.enabled": "1", "image.flip": "1"})
	r := e.run("", "hostname")
	expectRC(t, r, 0)
	contains(t, r.out, "Host name", "akam-rot2k")
	r = e.run("", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "Front LED", "on", "IR night LEDs", "auto", "[auto, on, off]")
	r = e.run("", "picture", "show")
	contains(t, r.out, "Brightness", "[0 to 100]", "Flip (vertical)")
	r = e.run("", "settings", "get", "track.enabled")
	expectRC(t, r, 0)
	contains(t, r.out, "Person tracking")
	r = e.run("", "settings", "list")
	expectRC(t, r, 0)
	contains(t, r.out, "Wi-Fi band", "Lost-person policy", "Default patrol route")
	r = e.run("", "video", "show")
	contains(t, r.out, "Bitrate, main stream", "1536", "[64 to 8000]")
	notContains(t, r.out, "second stream")
	r = e.run("", "video", "stream", "1", "show")
	contains(t, r.out, "second stream")
	if len(f.Patches()) != 0 {
		t.Error("a read sent a PATCH")
	}
	allSigned(t, f)
	noSecret(t, r)
}

func TestOptions2KWrites(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("", "led", "status", "off", "ir", "off")
	expectRC(t, r, 0)
	contains(t, r.out, `WILL send: PATCH /settings {"led.mode":"off","night.ir_led":"off"}`, "Front LED: off (applied now, saved on the camera)")
	if got := f.Patches(); len(got) != 1 || got[0] != `{"led.mode":"off","night.ir_led":"off"}` {
		t.Fatalf("PATCH bodies %q", got)
	}
	if f.Values["led.mode"] != "off" {
		t.Error("the fake did not receive the value")
	}
	r = e.run("", "picture", "brightness", "70", "flip", "on")
	expectRC(t, r, 0)
	contains(t, r.out, "not saved: back to the default at the next restart", "Flip (vertical): on")
	r = e.run("", "video", "bitrate", "2000", "fps", "10", "gop", "030")
	expectRC(t, r, 0)
	if p := f.Patches(); p[len(p)-1] != `{"video0.fps":"10","video0.gop":"30","video0.kbps":"2000"}` {
		t.Errorf("video body %q", p)
	}
	r = e.run("", "settings", "set", "track.enabled", "on", "track.return", "stay", "led.person", "3:250:250:5000")
	expectRC(t, r, 0)
	if p := f.Patches(); p[len(p)-1] != `{"led.person":"3:250:250:5000","track.enabled":"1","track.return":"stay"}` {
		t.Errorf("settings set body %q", p)
	}
	r = e.run("", "track", "on")
	expectRC(t, r, 0)
	contains(t, r.out, "is already on: not sent")
	n := len(f.Patches())
	r = e.run("", "led", "status", "off") // already off
	expectRC(t, r, 0)
	contains(t, r.out, "already off")
	if len(f.Patches()) != n {
		t.Error("an unchanged value was sent")
	}
	allSigned(t, f)
	noSecret(t, r)
}

func TestOptions2KNetworkAsksFirst(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("n\n", "wifi", "country", "fr")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, `"net.wifi_country":"FR"`, "not confirmed", "nothing was changed")
	if len(f.Patches()) != 0 {
		t.Fatal("sent without a confirmation")
	}
	r = e.run("y\n", "wifi", "country", "fr", "band", "5")
	expectRC(t, r, 0)
	if p := f.Patches(); len(p) != 1 || p[0] != `{"net.wifi_band":"5","net.wifi_country":"FR"}` {
		t.Fatalf("bodies %q", p)
	}
	contains(t, r.out, "queued", "reverted by the camera")
	r = e.run("", "--yes", "hostname", "cam-salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Host name: cam-salon (queued")
	r = e.run("", "hostname", "cam-two") // no terminal answer: end of input is a refusal
	expectRC(t, r, 1)
}

func TestOptions2KValidationRefusals(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	for name, tc := range map[string]struct {
		args []string
		want string
	}{
		"hostname dash":      {[]string{"hostname", "bad-"}, "1 to 32 letters, digits"},
		"hostname digits":    {[]string{"hostname", "12345"}, "not only digits"},
		"hostname long":      {[]string{"hostname", strings.Repeat("a", 33)}, "1 to 32"},
		"hostname space":     {[]string{"hostname", "a b"}, "1 to 32"},
		"band":               {[]string{"wifi", "band", "7"}, "one of 2.4, 5"},
		"country":            {[]string{"wifi", "country", "FRA"}, "two capital letters"},
		"band5 no country":   {[]string{"--yes", "wifi", "band", "5"}, "needs a Wi-Fi country"},
		"range":              {[]string{"picture", "brightness", "101"}, "from 0 to 100"},
		"not a number":       {[]string{"picture", "contrast", "abc"}, "whole number"},
		"enum":               {[]string{"led", "status", "blink"}, "one of auto, on, off"},
		"pattern":            {[]string{"settings", "set", "led.boot", "9:10:10:0"}, "on, off or N:on_ms"},
		"pattern cycle":      {[]string{"settings", "set", "led.boot", "10:60000:60000:0"}, "on, off or N:on_ms"},
		"qp order offline":   {[]string{"video", "minqp", "40", "maxqp", "30"}, "min_qp (40) must not be above"},
		"qp vs current":      {[]string{"video", "maxqp", "20"}, "must not be above"},
		"peak below":         {[]string{"video", "bitrate", "4000", "maxbitrate", "3000"}, "must be 0 (automatic)"},
		"max_kbps small":     {[]string{"video", "maxbitrate", "10"}, "0 (automatic) or 64"},
		"unknown key":        {[]string{"settings", "set", "rtsp.auth", "on"}, "not a setting of the 2K"},
		"odd pairs":          {[]string{"settings", "set", "led.mode"}, "needs pairs"},
		"twice":              {[]string{"led", "status", "on", "status", "off"}, "given twice"},
		"night mode":         {[]string{"picture", "night", "day"}, "not available on the 2K"},
		"saturation range":   {[]string{"picture", "saturation", "51"}, "from -50 to 50"},
		"denoise range":      {[]string{"picture", "denoise", "5"}, "from 0 to 4"},
		"wb word":            {[]string{"picture", "wb", "auto"}, "one of conf, manual"},
		"volume range":       {[]string{"settings", "set", "audio.out_volume", "101"}, "from 0 to 100"},
		"gain range":         {[]string{"settings", "set", "audio.out_gain", "6"}, "from 0 to 5"},
		"duplex word":        {[]string{"settings", "set", "audio.talk_duplex", "both"}, "one of half, full"},
		"state is read-only": {[]string{"settings", "set", "audio.out_state", "idle"}, "not a setting of the 2K"},
		"lost policy":        {[]string{"settings", "set", "track.lost_policy", "a b"}, "up to 8 steps"},
		"route":              {[]string{"settings", "set", "ptz.patrol.route_default", "Bad-Name"}, "a route name"},
		"unknown word":       {[]string{"led", "party"}, "unknown word"},
	} {
		r := e.run("", tc.args...)
		if r.rc == 0 || !strings.Contains(r.out+r.err, tc.want) {
			t.Errorf("%s: rc %d, want %q:\n%s", name, r.rc, tc.want, r.out+r.err)
		}
		if len(f.Patches()) != 0 {
			t.Fatalf("%s: a refused value reached the camera", name)
		}
	}
	// country none while the camera is on 5 GHz
	f.Values["net.wifi_band"], f.Values["net.wifi_country"] = "5", "FR"
	r := e.run("", "--yes", "wifi", "country", "none")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "cannot be none while the band is 5 GHz")
	if len(f.Patches()) != 0 {
		t.Fatal("sent")
	}
	// the offline refusals never even connect
	e2 := newEnv(t)
	f2 := e2.with2K(nil)
	e2.run("", "hostname", "bad-")
	if len(f2.Requests) != 0 {
		t.Errorf("%d requests for a value refused on this PC", len(f2.Requests))
	}
}

func TestOptions2KShowsTheCamerasOwnRefusal(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	f.Refuse = map[string]string{"video0.fps": "range|[1,15]"}
	r := e.run("", "video", "fps", "12", "gop", "5")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the camera refused video0.fps: range [1,15]", "video0.gop", "not applied", "all or none", "nothing was applied")
	if f.Values["video0.gop"] != "50" {
		t.Errorf("a refused change was applied: gop %s", f.Values["video0.gop"])
	}
	// a key the camera does not have
	delete(f.Values, "track.lag_ms")
	r = e.run("", "settings", "set", "track.lag_ms", "10")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "unknown_key")
}

func TestOptions2KNeedsRestartIsReported(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	f.Apply = map[string]string{"video0.kbps": "restart_component", "track.motion": "pending_device_restart"}
	r := e.run("", "video", "bitrate", "2000")
	expectRC(t, r, 0)
	contains(t, r.out, "needs a restart of that part of the camera")
	r = e.run("", "settings", "set", "track.motion", "steps")
	expectRC(t, r, 0)
	contains(t, r.out, "takes effect after the camera restarts", "the assistant never does")
}

func TestOptions2KWithoutAKeyPointsToKey2K(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	if err := os.Remove(filepath.Join(e.state, "api_key")); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{{"hostname"}, {"led", "status", "off"}, {"settings", "list"}, {"wifi"}} {
		r := e.run("", args...)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "no API key for this camera", "key-2k")
	}
	if _, err := os.Stat(filepath.Join(e.state, "api_key")); err == nil {
		t.Error("a key was created silently")
	}
	if len(f.Requests) != 0 {
		t.Error("the camera was contacted without a key")
	}
	r := e.run("3\n10\n") // the menu entry says the same
	contains(t, r.out+r.err, "no API key for this camera", "key-2k")
}

func TestOptions2KScopeAndClosedAPI(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	f.Scope = "read"
	r := e.run("", "led", "status", "off")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "may only read")
	r = e.run("", "led") // a read key reads
	expectRC(t, r, 0)
	f.Closed = true
	r = e.run("", "led")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "does not answer its control API", "key-2k")
	noSecret(t, r)
}

func TestOptions2KDryRunPrintsTheBodyAndTouchesNothing(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	if err := os.Remove(filepath.Join(e.state, "api_key")); err != nil { // a rehearsal needs no key
		t.Fatal(err)
	}
	r := e.run("", "--dry-run", "--model", rotModel, "led", "status", "off", "ir", "off")
	expectRC(t, r, 0)
	contains(t, r.out, `WOULD send: PATCH /settings {"led.mode":"off","night.ir_led":"off"}`)
	r = e.run("", "--dry-run", "--model", rotModel, "settings", "set", "net.wifi_country", "fr")
	contains(t, r.out, `{"net.wifi_country":"FR"}`, "confirmation")
	r = e.run("", "--dry-run", "--model", rotModel, "settings", "get", "net.hostname")
	expectRC(t, r, 0)
	if len(f.Requests) != 0 {
		t.Errorf("a dry run contacted the camera: %v", f.Requests)
	}
	noSecret(t, r)
}

func TestOptions2KNeverShowsACredential(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	f.Extra = map[string]string{"mqtt.password": "hunter2-pass", "api.secret_x": "hunter2-pass"} // hypothetical, not in the catalogue
	for _, args := range [][]string{{"settings", "list"}, {"settings", "get", "mqtt."}, {"settings", "get", "api."}, {"settings", "set", "mqtt.password", "hunter2-pass"}} {
		r := e.run("", args...)
		notContains(t, r.out+r.err, "hunter2-pass")
	}
	if len(f.Patches()) != 0 {
		t.Error("a credential was sent")
	}
}

// The 1080p-only words and the commands without a 2K equivalent stay refused, and the 2K commands are refused on a 1080p.
func TestOptions2KCommandsOnOtherModels(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + lsc1080 + "\ncamera_ip=192.0.2.10\n")
	for _, args := range [][]string{{"settings", "list"}, {"wifi"}, {"track", "on"}} {
		r := e.run("", args...)
		expectRC(t, r, 2)
		contains(t, r.out+r.err, "command of the Rotatable 2K")
	}
}

// The Camera options screen of a 2K: groups, current values, an edit, a confirmation, one PATCH.
func TestOptions2KMenu(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	// 3 options; 6 Network; 1 Host name -> new name; 12 apply; y confirm; 14 back; 10 back (groups); 10 quit
	r := e.run("3\n6\n1\ncam-menu\n12\ny\n14\n10\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out, "Rotatable 2K, through its signed API", "Host name", "akam-rot2k", "cam-menu", "(pending)", "About to change:", "queued")
	if p := f.Patches(); len(p) != 1 || p[0] != `{"net.hostname":"cam-menu"}` {
		t.Fatalf("bodies %q", p)
	}
	// declining the confirmation sends nothing
	f2 := e.with2K(nil)
	r = e.run("3\n6\n1\ncam-no\n12\nn\n14\n10\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out, "Not confirmed: nothing was sent")
	if len(f2.Patches()) != 0 {
		t.Error("sent without a confirmation")
	}
	// an invalid value is refused on the spot and asked again; an empty answer cancels
	r = e.run("3\n6\n1\n-bad\n\n14\n10\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out, "1 to 32 letters")
	// an enum edit through the list: LED group, front LED -> off
	r = e.run("3\n1\n1\n3\n9\ny\n11\n10\n10\n")
	expectRC(t, r, 0)
	if p := f2.Patches(); len(p) != 1 || p[0] != `{"led.mode":"off"}` {
		t.Fatalf("led bodies %q", p)
	}
	allSigned(t, f)
	noSecret(t, r)
}

// The speaker, two-way audio and picture settings the 1080p also has, on the 2K through the signed API.
func TestOptions2KSpeakerAndPicture(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(map[string]string{"audio.out_enable": "0", "audio.out_volume": "100", "isp.saturation": "0", "isp.denoise": "0", "img.wb": "conf"})
	r := e.run("", "picture", "saturation", "-15", "denoise", "2", "wb", "manual")
	expectRC(t, r, 0)
	contains(t, r.out, `WILL send: PATCH /settings {"img.wb":"manual","isp.denoise":"2","isp.saturation":"-15"}`, "Saturation: -15 (applied now, saved on the camera)", "White balance: manual (applied now, not saved")
	r = e.run("", "settings", "set", "audio.out_enable", "on", "audio.out_volume", "60", "audio.talk_enable", "1", "audio.talk_duplex", "full")
	expectRC(t, r, 0)
	contains(t, r.out, `"audio.out_enable":"1"`, `"audio.out_volume":"60"`, `"audio.talk_duplex":"full"`)
	if got := f.Patches(); len(got) != 2 {
		t.Fatalf("PATCH bodies %q", got)
	}
	r = e.run("", "picture", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "Saturation", "Denoise", "White balance", "[-50 to 50]", "[0 to 4]", "[conf, manual]")
	r = e.run("", "settings", "get", "audio.")
	expectRC(t, r, 0)
	contains(t, r.out, "Speaker", "Speaker volume", "Talk (two-way audio)", "Talk duplex")
	allSigned(t, f)
	noSecret(t, r)
}
