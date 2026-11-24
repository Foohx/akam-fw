package camopt

import (
	"strings"
	"testing"
)

// fakeCtl is a control port in memory: one call of the Exchanger is one exchange.
type fakeCtl struct {
	vals      map[string]string
	exchanges [][]string
}

func (f *fakeCtl) ex(lines ...string) []string {
	f.exchanges = append(f.exchanges, lines)
	var out []string
	for _, l := range lines {
		w := strings.SplitN(l, " ", 3)
		switch w[0] {
		case "GET":
			if v, ok := f.vals[w[1]]; ok {
				out = append(out, w[1]+"="+v)
			} else {
				out = append(out, "ERR unknown param "+w[1])
			}
		case "SET":
			f.vals[w[1]] = w[2]
			note := ""
			if strings.HasPrefix(w[1], "video.") {
				note = " (restart to apply)"
			}
			out = append(out, "OK "+w[1]+"="+w[2]+note)
		}
	}
	return out
}

func TestReadAllGroupsInOneExchange(t *testing.T) {
	f := &fakeCtl{vals: map[string]string{"led.status": "auto", "video.bitrate_cfg": "1536", "audio.mic_enable": "1"}}
	st := Read(f.ex)
	if len(f.exchanges) != 1 {
		t.Fatalf("%d exchanges, want 1", len(f.exchanges))
	}
	by := map[string]OptionState{}
	for _, s := range st {
		by[s.ID] = s
	}
	if by["led.status"].Value != "auto" || by["video.bitrate_cfg"].Value != "1536" || by["audio.mic_enable"].Value != "1" {
		t.Errorf("values: %+v", by)
	}
	// an option missing on an old image is reported unavailable, not an error
	if !by["video.gop_cfg"].Unavailable || by["video.gop_cfg"].Value != "" {
		t.Errorf("gop on an old image: %+v", by["video.gop_cfg"])
	}
}

func TestApplyReportsAppliesAndSaved(t *testing.T) {
	f := &fakeCtl{vals: map[string]string{"video.bitrate_cfg": "1536", "audio.mic_enable": "1", "night.mode": "auto"}}
	res, err := Apply(f.ex, []Change{{"video.bitrate_cfg", "02000"}, {"audio.mic_enable", "0"}, {"night.mode", "day"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(f.exchanges) != 1 || f.exchanges[0][0] != "SET video.bitrate_cfg 2000" {
		t.Fatalf("exchanges: %v", f.exchanges)
	}
	if r := res[0]; !r.OK || !r.Restart || r.Applies != VideoRestart || r.Persisted != PersistCamera {
		t.Errorf("bitrate: %+v", r)
	}
	if r := res[1]; r.Applies != NextPowerCycle || r.Persisted != PersistCamera {
		t.Errorf("mic: %+v", r)
	}
	if r := res[2]; r.Applies != Now || r.Persisted != PersistRAM || r.Restart {
		t.Errorf("night: %+v", r)
	}
}

func TestValidatorsRejectBadValues(t *testing.T) {
	bad := []Change{
		{"video.bitrate_cfg", "499"}, {"video.bitrate_cfg", "3001"}, {"video.bitrate_cfg", "12e3"}, {"video.bitrate_cfg", ""},
		{"video.bitrate_cfg", "1 ; reboot"}, {"video.fps_cfg", "12"}, {"video.fps_cfg", "30"}, {"video.fps_cfg", "5"}, {"video.fps_cfg", "010"}, {"video.gop_cfg", "0"}, {"video.gop_cfg", "5"},
		{"audio.mic_enable", "on"}, {"led.status", "blue"}, {"isp.contrast", "51"}, {"net.ssid", "x"}, {"nope", "1"},
	}
	for _, c := range bad {
		f := &fakeCtl{vals: map[string]string{}}
		if _, err := Apply(f.ex, []Change{{"led.status", "on"}, c}); err == nil {
			t.Errorf("%v accepted", c)
		}
		if len(f.exchanges) != 0 {
			t.Errorf("%v: something was sent although a change was invalid", c)
		}
	}
	for _, c := range []Change{{"video.bitrate_cfg", "500"}, {"video.bitrate_cfg", "3000"}, {"video.fps_cfg", "10"}, {"video.fps_cfg", "15"}, {"video.fps_cfg", "20"}, {"video.fps_cfg", "25"}, {"video.gop_cfg", "4"}, {"isp.sharpness", "-50"}} {
		if _, err := Validate(c.ID, c.Value); err != nil {
			t.Errorf("%v: %v", c, err)
		}
	}
}

func TestNTPServerGrammar(t *testing.T) {
	good := []string{"pool.ntp.org", "192.0.2.1", "a", "0.fr.pool.ntp.org", "a-b.c", strings.Repeat("a", 63)}
	bad := []string{"", "-x", ".x", "x.", "a..b", "a_b", "a b", "a;b", "a$(b)", "a'b", "a\nb", "x\x00", "-", "a-", "-a.b", "é.org", strings.Repeat("a", 64), strings.Repeat("a.", 127) + "a"}
	for _, s := range good {
		if !ValidNTPServer(s) {
			t.Errorf("%q refused", s)
		}
	}
	for _, s := range bad {
		if ValidNTPServer(s) {
			t.Errorf("%q accepted", s)
		}
	}
	if ValidNTPServers(nil) == nil || ValidNTPServers([]string{"a", "b", "c", "d"}) == nil || ValidNTPServers([]string{"a", "b c"}) == nil {
		t.Error("bad lists accepted")
	}
	if err := ValidNTPServers([]string{"a", "b", "c"}); err != nil {
		t.Error(err)
	}
}

func TestCameraValuesWithControlCharsAreDropped(t *testing.T) {
	for _, evil := range []string{"1\x1b[31m", "x\x07", "a'b", "a\"b", "$(reboot)", "a`b`", "a\\b", "é", strings.Repeat("9", 70), "1;reboot"} {
		f := &fakeCtl{vals: map[string]string{"video.bitrate_cfg": evil, "led.status": evil}}
		for _, s := range Read(f.ex, "video.bitrate_cfg", "led.status") {
			if !s.Unavailable || s.Value != "" {
				t.Errorf("%q kept: %+v", evil, s)
			}
		}
	}
	// a number must be a number even when it looks safe
	f := &fakeCtl{vals: map[string]string{"video.bitrate_cfg": "abc"}}
	if s := Read(f.ex, "video.bitrate_cfg")[0]; !s.Unavailable {
		t.Errorf("non-number kept: %+v", s)
	}
}

func TestCatalogCoversTheCommandsAndWiFiIsReadOnly(t *testing.T) {
	seen := map[string]bool{}
	for _, o := range Catalog() {
		if seen[o.ID] {
			t.Errorf("duplicate %s", o.ID)
		}
		seen[o.ID] = true
		if o.Key == "" && o.Helper == "" {
			t.Errorf("%s has no mechanism", o.ID)
		}
	}
	for _, id := range []string{"net.ssid", "net.wifi_rssi"} {
		o, _ := Find(id)
		if !o.ReadOnly {
			t.Errorf("%s is writable", id)
		}
		if _, err := Validate(id, "x"); err == nil {
			t.Errorf("%s validated for writing", id)
		}
	}
}

// The orientation options of the 1080p (image.flip, image.mirror, image.ceiling): the same names and 0/1 values as the 2K rows, sent on the
// control port, applied at once and saved on the camera.
func TestOrientationOptions(t *testing.T) {
	for _, id := range []string{"image.flip", "image.mirror", "image.ceiling"} {
		o, ok := Find(id)
		if !ok || o.Key != id || o.Kind != Enum || o.Applies != Now || o.Persisted != PersistCamera || o.ReadOnly {
			t.Errorf("%s: %+v (found %v)", id, o, ok)
		}
		for _, v := range []string{"0", "1"} {
			if _, err := Validate(id, v); err != nil {
				t.Errorf("%s %s: %v", id, v, err)
			}
		}
		for _, v := range []string{"2", "on", "", "-1", "01", "1 ; reboot"} {
			if _, err := Validate(id, v); err == nil {
				t.Errorf("%s %q accepted", id, v)
			}
		}
	}
	f := &fakeCtl{vals: map[string]string{}}
	res, err := Apply(f.ex, []Change{{"image.flip", "1"}, {"image.mirror", "1"}})
	if err != nil || len(res) != 2 || res[0].Applies != Now || res[0].Persisted != PersistCamera || res[0].Restart {
		t.Errorf("apply: %v %+v", err, res)
	}
}

// The opt-in that opens the 1080p's text port to the network replaced the network debug mode in the catalogue.
func TestControlLanOptionReplacedTheDebugMode(t *testing.T) {
	if _, ok := Find("debug"); ok {
		t.Error("the network debug mode is gone")
	}
	o, ok := Find("control.lan")
	if !ok || o.Kind != Enum || o.Key != "control.lan" || o.Applies != VideoRestart || o.Persisted != PersistCamera || o.ReadOnly {
		t.Fatalf("%+v %v", o, ok)
	}
	for _, v := range []string{"0", "1"} {
		if got, err := Validate("control.lan", v); err != nil || got != v {
			t.Errorf("%q: %q %v", v, got, err)
		}
	}
	if _, err := Validate("control.lan", "2"); err == nil {
		t.Error("2 accepted")
	}
}
