package camopt

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCatalog2KKeysAreUniqueAndInTheFirmwareTable(t *testing.T) {
	var fw strings.Builder
	for _, f := range []string{"settings.c", "netreg.c", "cfgreg.c"} {
		b, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "firmware", "lsc-rotatable-2k-ak3918av130", f))
		if err != nil {
			t.Skip("skip: the firmware sources are not next to the assistant")
		}
		fw.Write(b)
	}
	src := fw.String()
	seen := map[string]bool{}
	for _, o := range Catalog2K() {
		if seen[o.Key] || o.ID != o.Key || o.Label == "" || o.Group == "" {
			t.Errorf("bad or duplicate row %q", o.Key)
		}
		seen[o.Key] = true
		k := o.Key
		switch {
		case strings.HasPrefix(k, "video0.") || strings.HasPrefix(k, "video1."):
			// built from VP_NAME: check the name part
			if !strings.Contains(src, `"`+k[len("video0."):]+`"`) {
				t.Errorf("%s: video parameter not in the firmware", k)
			}
		case strings.HasPrefix(k, "led.") && k != "led.mode":
			if !strings.Contains(src, `"`+k[len("led."):]+`"`) {
				t.Errorf("%s: LED rule not in the firmware", k)
			}
		default:
			if !strings.Contains(src, `"`+k+`"`) {
				t.Errorf("%s is not a row of the firmware table", k)
			}
		}
		if o.Kind == Secret && k != "mqtt.password" && k != "rtsp.password" {
			t.Errorf("%s: only the two passwords are write-only rows of the 2K registry", k)
		}
	}
	for _, must := range []string{"net.hostname", "net.wifi_country", "net.wifi_band", "image.flip", "image.mirror", "image.ceiling", "isp.saturation", "isp.denoise", "img.wb", "audio.out_enable", "audio.out_volume", "audio.out_gain", "audio.talk_enable", "audio.talk_duplex", "track.enabled", "night.ir_led", "led.mode"} {
		if !seen[must] {
			t.Errorf("%s missing from the catalogue", must)
		}
	}
}

func TestValidate2K(t *testing.T) {
	ok := map[[2]string]string{
		{"net.hostname", "cam-1a"}: "cam-1a", {"net.hostname", "A"}: "A", {"net.hostname", strings.Repeat("a", 32)}: strings.Repeat("a", 32),
		{"net.wifi_country", "fr"}: "FR", {"net.wifi_country", "None"}: "none", {"net.wifi_band", "5"}: "5", {"net.wifi_band", "2.4"}: "2.4",
		{"isp.saturation", "-50"}: "-50", {"isp.saturation", "50"}: "50", {"isp.saturation", "007"}: "7", {"isp.denoise", "0"}: "0", {"isp.denoise", "4"}: "4", {"img.wb", "manual"}: "manual", {"img.wb", "conf"}: "conf",
		{"audio.out_enable", "on"}: "1", {"audio.out_volume", "0"}: "0", {"audio.out_volume", "100"}: "100", {"audio.out_gain", "5"}: "5", {"audio.out_amp", "auto"}: "auto", {"audio.talk_enable", "false"}: "0", {"audio.talk_duplex", "full"}: "full",
		{"image.flip", "on"}: "1", {"image.flip", "0"}: "0", {"image.ceiling", "TRUE"}: "1",
		{"image.brightness", "007"}: "7", {"video0.max_kbps", "0"}: "0", {"video0.max_kbps", "64"}: "64", {"video1.max_kbps", "32"}: "32",
		{"track.lag_ms", "-150"}: "-150", {"led.idle", "3:250:250:5000"}: "3:250:250:5000", {"led.idle", "off"}: "off", {"led.idle", "1:50:50:0"}: "1:50:50:0",
		{"track.lost_policy", "backsweep,exit,hold:30,home"}: "backsweep,exit,hold:30,home", {"ptz.patrol.route_default", ""}: "", {"ptz.patrol.route_default", "entrance"}: "entrance",
	}
	for in, want := range ok {
		if got, err := Validate2K(in[0], in[1]); err != nil || got != want {
			t.Errorf("%v: got %q, %v want %q", in, got, err, want)
		}
	}
	bad := [][2]string{
		{"net.hostname", ""}, {"net.hostname", "-a"}, {"net.hostname", "a-"}, {"net.hostname", "123"}, {"net.hostname", "a_b"}, {"net.hostname", "a.b"}, {"net.hostname", strings.Repeat("a", 33)},
		{"net.wifi_country", "F"}, {"net.wifi_country", "F1"}, {"net.wifi_country", "FRA"}, {"net.wifi_band", "6"},
		{"isp.saturation", "51"}, {"isp.saturation", "-51"}, {"isp.saturation", "1.5"}, {"isp.denoise", "5"}, {"isp.denoise", "-1"}, {"img.wb", "auto"}, {"img.wb", "1"},
		{"audio.out_enable", "2"}, {"audio.out_volume", "101"}, {"audio.out_volume", "-1"}, {"audio.out_gain", "6"}, {"audio.out_amp", "loud"}, {"audio.talk_duplex", "both"}, {"audio.out_state", "idle"}, {"audio.talk_state", "idle"},
		{"image.flip", "2"}, {"image.brightness", "101"}, {"image.brightness", "-1"}, {"image.brightness", "1.5"}, {"image.brightness", ""},
		{"video0.fps", "16"}, {"video0.fps", "0"}, {"video0.kbps", "63"}, {"video1.kbps", "1001"}, {"video0.max_kbps", "63"}, {"video1.max_kbps", "31"}, {"video0.rc", "abr"},
		{"led.idle", "2:49:50:0"}, {"led.idle", "11:100:100:0"}, {"led.idle", "1:100:100:49"}, {"led.idle", "10:60000:60000:0"}, {"led.idle", "blink"},
		{"track.lost_policy", ""}, {"track.lost_policy", "a b"}, {"track.lost_policy", "a,b,c,d,e,f,g,h,i"},
		{"ptz.patrol.route_default", "Bad"}, {"ptz.patrol.route_default", strings.Repeat("a", 17)},
		{"ptz.state", "x"}, {"sys.cpu_pct", "1"}, {"mqtt.host", "bad host"}, {"api.keys", "1"},
	}
	for _, in := range bad {
		if got, err := Validate2K(in[0], in[1]); err == nil {
			t.Errorf("%v accepted as %q", in, got)
		}
	}
}

func TestCheck2K(t *testing.T) {
	cur := map[string]string{"net.wifi_band": "2.4", "net.wifi_country": "none", "video0.min_qp": "28", "video0.max_qp": "43", "video0.kbps": "1536", "video0.max_kbps": "0"}
	cases := []struct {
		name    string
		pending map[string]string
		bad     bool
	}{
		{"band 5 without country", map[string]string{"net.wifi_band": "5"}, true},
		{"band 5 with a country in the same change", map[string]string{"net.wifi_band": "5", "net.wifi_country": "FR"}, false},
		{"country none with band 5 in the same change", map[string]string{"net.wifi_band": "5", "net.wifi_country": "none"}, true},
		{"country alone", map[string]string{"net.wifi_country": "FR"}, false},
		{"min above stored max", map[string]string{"video0.min_qp": "44"}, true},
		{"max below stored min", map[string]string{"video0.max_qp": "27"}, true},
		{"both qp ok", map[string]string{"video0.min_qp": "20", "video0.max_qp": "30"}, false},
		{"peak below target", map[string]string{"video0.max_kbps": "1000"}, true},
		{"target above stored peak", map[string]string{"video0.kbps": "2000", "video0.max_kbps": "1500"}, true},
		{"auto peak", map[string]string{"video0.kbps": "4000"}, false},
	}
	for _, c := range cases {
		if got := len(Check2K(c.pending, cur)) > 0; got != c.bad {
			t.Errorf("%s: refused=%v want %v", c.name, got, c.bad)
		}
	}
	on5 := map[string]string{"net.wifi_band": "5", "net.wifi_country": "FR"}
	if len(Check2K(map[string]string{"net.wifi_country": "none"}, on5)) == 0 {
		t.Error("country none while on band 5 accepted")
	}
	if len(Check2K(map[string]string{"net.wifi_band": "5"}, nil)) != 0 {
		t.Error("with no current values the camera decides")
	}
}

func TestDisplay2KHidesSecrets(t *testing.T) {
	sec := Option{Key: "x.token", Kind: Secret}
	if got := Display2K(sec, "hunter2"); strings.Contains(got, "hunter2") {
		t.Errorf("%q", got)
	}
	if got := Display2K(Option{Key: "mqtt.password", Kind: String}, "hunter2"); strings.Contains(got, "hunter2") {
		t.Errorf("a credential key is shown: %q", got)
	}
	if got := Display2K(Option{Key: "image.flip", Kind: Bool}, "1"); got != "on" {
		t.Errorf("bool %q", got)
	}
	if got := Display2K(Option{Key: "net.hostname", Kind: String}, "cam"); got != "cam" {
		t.Errorf("plain %q", got)
	}
	for _, o := range Catalog2K() {
		if looksSecret(o.Key) != (o.Kind == Secret) {
			t.Errorf("%s: a credential key must be a Secret row (masked), and a plain setting must not look like one", o.Key)
		}
	}
}

func TestValidate2KServices(t *testing.T) {
	ok := map[[2]string]string{
		{"mqtt.host", "broker.lan"}: "broker.lan", {"mqtt.host", ""}: "", {"mqtt.port", "01883"}: "1883", {"mqtt.user", "ha_1@x.y+z"}: "ha_1@x.y+z", {"mqtt.user", ""}: "",
		{"mqtt.enabled", "on"}: "1", {"onvif.enabled", "0"}: "0", {"onvif.port", "8000"}: "8000", {"audio.mic_enable", "yes"}: "1",
		{"rtsp.user", "bob"}: "bob", {"rtsp.user", ""}: "", {"rtsp.password", "long enough"}: "long enough", {"mqtt.password", ""}: "", {"mqtt.password", "a b"}: "a b",
		{"ntp.servers", "pool.ntp.org  time.example.net"}: "pool.ntp.org time.example.net", {"ntp.servers", ""}: "",
	}
	for in, want := range ok {
		if got, err := Validate2K(in[0], in[1]); err != nil || got != want {
			t.Errorf("%v: got %q, %v want %q", in, got, err, want)
		}
	}
	bad := [][2]string{
		{"mqtt.host", "bad host"}, {"mqtt.host", "a_b"}, {"mqtt.host", strings.Repeat("a", 64)}, {"mqtt.port", "0"}, {"mqtt.port", "65536"}, {"mqtt.user", "-"}, {"mqtt.user", "a b"},
		{"mqtt.user", strings.Repeat("a", 33)}, {"mqtt.password", "a\tb"}, {"mqtt.password", strings.Repeat("a", 129)}, {"onvif.port", "80"}, {"onvif.enabled", "2"},
		{"rtsp.user", "a:b"}, {"rtsp.password", "short"}, {"rtsp.password", ""}, {"rtsp.password", "bad\x01password"},
		{"ntp.servers", "a b c d"}, {"ntp.servers", "-x"}, {"ntp.servers", "x_y"}, {"audio.mic_enable", "2"}, {"rtsp.auth", "on"}, {"cfg.apply", "ok"},
	}
	for _, in := range bad {
		if got, err := Validate2K(in[0], in[1]); err == nil {
			t.Errorf("%v accepted as %q", in, got)
		}
	}
}

func TestCheck2KServices(t *testing.T) {
	cur := map[string]string{"mqtt.host": "", "mqtt.user": "", "rtsp.user": ""}
	cases := []struct {
		name    string
		pending map[string]string
		bad     bool
	}{
		{"MQTT on with no host", map[string]string{"mqtt.enabled": "1"}, true},
		{"MQTT on with the host in the same change", map[string]string{"mqtt.enabled": "1", "mqtt.host": "b.lan"}, false},
		{"MQTT password without a user", map[string]string{"mqtt.password": "x"}, true},
		{"MQTT password with its user", map[string]string{"mqtt.password": "x", "mqtt.user": "ha"}, false},
		{"MQTT empty password needs no user", map[string]string{"mqtt.password": ""}, false},
		{"a new stream user alone", map[string]string{"rtsp.user": "bob"}, true},
		{"a stream user with its password", map[string]string{"rtsp.user": "bob", "rtsp.password": "long enough"}, false},
		{"an empty user with a password", map[string]string{"rtsp.user": "", "rtsp.password": "long enough"}, true},
		{"ONVIF without a stream user", map[string]string{"onvif.enabled": "1"}, true},
		{"ONVIF with the credentials in the same change", map[string]string{"onvif.enabled": "1", "rtsp.user": "bob", "rtsp.password": "long enough"}, false},
		{"ONVIF off needs nothing", map[string]string{"onvif.enabled": "0"}, false},
	}
	for _, c := range cases {
		if got := len(Check2K(c.pending, cur)) > 0; got != c.bad {
			t.Errorf("%s: refused=%v want %v", c.name, got, c.bad)
		}
	}
	if len(Check2K(map[string]string{"rtsp.user": "bob"}, map[string]string{"rtsp.user": "bob"})) != 0 {
		t.Error("the same stream user again is a no-op")
	}
	if len(Check2K(map[string]string{"mqtt.enabled": "1"}, nil)) != 0 {
		t.Error("with no current values the camera decides")
	}
}
