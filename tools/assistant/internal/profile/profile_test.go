package profile

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func mustSet(t *testing.T, f *File, kv ...string) {
	t.Helper()
	for i := 0; i < len(kv); i += 2 {
		if err := f.Set(kv[i], kv[i+1]); err != nil {
			t.Fatalf("Set(%s): %v", kv[i], err)
		}
	}
}

func TestRoundTripKeepsEveryKnownKeyAndTheUnknownOnes(t *testing.T) {
	f := New(Settings)
	mustSet(t, f, "video.bitrate_cfg", "2000", "video.fps_cfg", "20", "led.status", "off", "audio.mic_enable", "0",
		OnvifEnabled, "1", OnvifPort, "8080", NTPEnabled, "1", NTPServers, "pool.ntp.org,192.0.2.1",
		MQTTEnabled, "1", MQTTHost, "broker.lan", MQTTPort, "1883", MQTTUser, "bob", MQTTBaseTopic, "home/akam/salon", MQTTDiscovery, "off",
		AuthEnabled, "1", AuthUser, "viewer", KeyKind, "custom", KeyPath, "/keys/mine.pub", SavedAt, "2026-11-13 10:02 UTC")
	f.Extra["future.setting"] = "kept as it is"
	b := f.Marshal()
	if !strings.HasPrefix(string(b), "# ") || !strings.Contains(string(b), "\nakam_config_version=1\n") {
		t.Fatalf("no version line:\n%s", b)
	}
	g, err := Parse(b, Settings)
	if err != nil {
		t.Fatal(err)
	}
	if string(g.Marshal()) != string(b) {
		t.Errorf("round trip differs:\n%s\n---\n%s", b, g.Marshal())
	}
	if g.Extra["future.setting"] != "kept as it is" || g.Get(MQTTBaseTopic) != "home/akam/salon" {
		t.Errorf("lost data: %v %v", g.Extra, g.Vals)
	}
	// an unknown key is ignored by the readers: it is not a known value
	if g.Has("future.setting") {
		t.Error("an unknown key became a value")
	}
}

func TestParseIsStrict(t *testing.T) {
	cases := map[string]string{
		"no version":        "video.fps_cfg=15\n",
		"empty":             "",
		"version not first": "video.fps_cfg=15\nakam_config_version=1\n",
		"newer version":     "akam_config_version=2\n",
		"bad version":       "akam_config_version=x\n",
		"duplicate":         "akam_config_version=1\nvideo.fps_cfg=15\nvideo.fps_cfg=20\n",
		"not key=value":     "akam_config_version=1\nhello\n",
		"bad value":         "akam_config_version=1\nvideo.fps_cfg=99\n",
		"out of range":      "akam_config_version=1\nvideo.bitrate_cfg=100\n",
		"bad port":          "akam_config_version=1\nmqtt.port=70000\n",
		"bad topic":         "akam_config_version=1\nmqtt.base_topic=/a//b\n",
		"bad ntp":           "akam_config_version=1\nntp.servers=-evil\n",
		"CRLF":              "akam_config_version=1\r\nvideo.fps_cfg=15\r\n",
		"control":           "akam_config_version=1\nauth.user=a\x01b\n",
		"bad unknown key":   "akam_config_version=1\nBad Key=1\n",
		"too long":          "akam_config_version=1\nsaved.from=" + strings.Repeat("a", 600) + "\n",
	}
	for name, in := range cases {
		if _, err := Parse([]byte(in), Settings); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if _, err := Parse([]byte("akam_config_version=2\n"), Settings); !errors.Is(err, ErrNewer) {
		t.Errorf("a newer file must say so: %v", err)
	}
	if _, err := Parse([]byte(strings.Repeat("# x\n", 20000)), Settings); err == nil {
		t.Error("a file over 64 KiB was accepted")
	}
}

func TestCommentsBlankLinesAndMissingFinalNewline(t *testing.T) {
	g, err := Parse([]byte("# hello\n\nakam_config_version=1\n# note\nled.status=on"), Settings)
	if err != nil || g.Get("led.status") != "on" {
		t.Fatalf("%v %v", g, err)
	}
}

func TestSecretsFileHasItsOwnKeys(t *testing.T) {
	s := New(Secrets)
	mustSet(t, s, AuthPassword, "correct horse", MQTTPassword, "p@ss=word with spaces", WifiSSID, "My Net", WifiPassword, "wifi-pass-123")
	g, err := Parse(s.Marshal(), Secrets)
	if err != nil {
		t.Fatal(err)
	}
	if g.Get(MQTTPassword) != "p@ss=word with spaces" || g.Get(WifiSSID) != "My Net" {
		t.Errorf("values not kept verbatim: %v", g.Vals)
	}
	if err := s.Set("video.fps_cfg", "15"); err == nil {
		t.Error("a setting went into the secrets file")
	}
	if err := New(Settings).Set(AuthPassword, "correct horse"); err == nil {
		t.Error("a secret went into the settings file")
	}
	if err := s.Set(AuthPassword, "short"); err == nil {
		t.Error("a 5 character stream password was accepted")
	}
}

func TestGroupsAndOnly(t *testing.T) {
	for k, g := range map[string]string{"video.fps_cfg": "video", "isp.contrast": "picture", "led.status": "led", "audio.out_volume": "audio",
		OnvifPort: "onvif", NTPServers: "ntp", MQTTHost: "mqtt", AuthUser: "auth", KeyKind: "key", SavedAt: ""} {
		if got := GroupOf(k); got != g {
			t.Errorf("GroupOf(%s) = %q, want %q", k, got, g)
		}
	}
	m, err := ParseOnly("video, mqtt")
	if err != nil || !m["video"] || !m["mqtt"] || len(m) != 2 {
		t.Errorf("%v %v", m, err)
	}
	if _, err := ParseOnly("video,wifi"); err == nil {
		t.Error("wifi is not a group: it is never applied to a camera")
	}
	if m, _ := ParseOnly(""); m != nil {
		t.Error("no --only means every group")
	}
}

func TestSaveLoadForgetAndProtection(t *testing.T) {
	base := t.TempDir()
	p := Global(filepath.Join(base, "state"))
	if _, err := Load(p, Settings); !errors.Is(err, ErrNone) {
		t.Fatalf("missing file: %v", err)
	}
	f := New(Settings)
	mustSet(t, f, "led.status", "off")
	if err := Save(p, Settings, f); err != nil {
		t.Fatal(err)
	}
	s := New(Secrets)
	mustSet(t, s, WifiSSID, "net", WifiPassword, "hunter2hunter2")
	if err := Save(p, Secrets, s); err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		for _, path := range []string{p.Settings, p.Secrets} {
			if st, err := os.Stat(path); err != nil || st.Mode().Perm() != 0o600 {
				t.Errorf("%s: %v %v", path, st, err)
			}
		}
		if st, _ := os.Stat(filepath.Dir(p.Settings)); st.Mode().Perm() != 0o700 {
			t.Errorf("directory mode %v", st.Mode().Perm())
		}
	}
	// nothing but the two files is left behind by the atomic writes
	ents, _ := os.ReadDir(filepath.Dir(p.Settings))
	if len(ents) != 2 {
		t.Errorf("leftovers: %v", ents)
	}
	g, err := Load(p, Settings)
	if err != nil || g.Get("led.status") != "off" {
		t.Fatalf("%v %v", g, err)
	}
	// a corrupt file is an error that names the file, never a guess
	if err := os.WriteFile(p.Settings, []byte("akam_config_version=1\nled.status=purple\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(p, Settings); err == nil || !strings.Contains(err.Error(), "global.conf") {
		t.Errorf("corrupt file: %v", err)
	}
	had, err := Forget(p)
	if err != nil || !had {
		t.Fatalf("forget: %v %v", had, err)
	}
	if Exists(p, Secrets) || !Exists(p, Settings) {
		t.Error("forget must delete the secrets and keep the settings")
	}
	if had, _ := Forget(p); had {
		t.Error("nothing left to forget")
	}
	// an empty secrets file is removed, not written
	if err := Save(p, Secrets, s); err != nil {
		t.Fatal(err)
	}
	if err := Save(p, Secrets, New(Secrets)); err != nil || Exists(p, Secrets) {
		t.Errorf("empty secrets file kept: %v", err)
	}
}

func TestCameraPlacesAreSeparate(t *testing.T) {
	a, b := Camera("salon", "/s/cameras/salon"), Camera("garage", "/s/cameras/garage")
	if a.Settings == b.Settings || a.Secrets == b.Secrets {
		t.Error("two cameras share a file")
	}
	if g := Global("/s"); g.Settings != filepath.Join("/s", "profiles", "global.conf") || g.Secrets != filepath.Join("/s", "profiles", "global.secrets") {
		t.Errorf("%v", g)
	}
}
