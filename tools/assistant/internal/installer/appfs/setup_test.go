//go:build unix

package appfs

import (
	"context"
	"crypto/md5" //nolint:gosec // the expected Digest HA1
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	setupAuthPass = "stream-s3cret-pass"
	setupMqttPass = "broker-s3cret-pass"
)

func TestSetup2KFiles(t *testing.T) {
	if f, err := (*Setup2K)(nil).Files(); err != nil || len(f) != 0 {
		t.Fatalf("nil: %v %v", f, err)
	}
	if f, err := (&Setup2K{}).Files(); err != nil || len(f) != 0 {
		t.Fatalf("zero value: %v %v", f, err)
	}
	s := &Setup2K{Mic: true, ONVIF: true, ONVIFPort: 8123, MQTTHost: "broker.lan", MQTTUser: "ha", MQTTPass: setupMqttPass,
		AuthUser: "bob", AuthPass: setupAuthPass, NTP: []string{"pool.ntp.org", "time.example.net"}}
	f, err := s.Files()
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(SetupNames(f), " "); got != "akam.conf mqtt mqtt_pass ntp rtsp.conf" {
		t.Fatalf("files: %s", got)
	}
	for name, want := range map[string]string{
		"mqtt": "enabled=1\nhost=broker.lan\nport=1883\nuser=ha\n", "mqtt_pass": setupMqttPass + "\n", "ntp": "server=pool.ntp.org\nserver=time.example.net\n",
	} {
		if string(f[name]) != want {
			t.Errorf("%s: %q", name, f[name])
		}
	}
	for _, line := range []string{"audio=1", "onvif=1", "onvif_port=8123"} {
		if !strings.Contains(string(f["akam.conf"]), "\n"+line+"\n") {
			t.Errorf("akam.conf lacks %s: %q", line, f["akam.conf"])
		}
	}
	h := md5.Sum([]byte("bob:akam:" + setupAuthPass)) //nolint:gosec
	if string(f["rtsp.conf"]) != "bob:akam:"+hex.EncodeToString(h[:])+"\n" {
		t.Errorf("rtsp.conf: %q", f["rtsp.conf"])
	}
	if strings.Contains(string(f["rtsp.conf"]), setupAuthPass) {
		t.Error("the stream password itself must never be stored")
	}
}

func TestSetup2KRefusals(t *testing.T) {
	for name, s := range map[string]Setup2K{
		"onvif without credentials": {ONVIF: true},
		"onvif port":                {ONVIF: true, AuthUser: "bob", AuthPass: setupAuthPass, ONVIFPort: 80},
		"port without onvif":        {ONVIFPort: 8000},
		"bad host":                  {MQTTHost: "bad host"},
		"bad mqtt port":             {MQTTHost: "b.lan", MQTTPort: 70000},
		"bad mqtt user":             {MQTTHost: "b.lan", MQTTUser: "-"},
		"password without user":     {MQTTHost: "b.lan", MQTTPass: "x"},
		"control char":              {MQTTHost: "b.lan", MQTTUser: "ha", MQTTPass: "a\tb"},
		"mqtt options without host": {MQTTUser: "ha"},
		"short stream password":     {AuthUser: "bob", AuthPass: "short"},
		"stream user only":          {AuthUser: "bob"},
		"bad stream user":           {AuthUser: "a:b", AuthPass: setupAuthPass},
		"four ntp servers":          {NTP: []string{"a.b", "c.d", "e.f", "g.h"}},
		"bad ntp server":            {NTP: []string{"-x"}},
	} {
		if f, err := s.Files(); err == nil {
			t.Errorf("%s accepted: %v", name, SetupNames(f))
		}
	}
}

func TestBuildCardCarriesTheSetupFilesAndSeedCopiesThem(t *testing.T) {
	if !haveTools("sh", "md5sum", "cut", "grep") {
		t.Skip("sh, md5sum, cut and grep are needed")
	}
	k := cardKit(t)
	setup := &Setup2K{Mic: true, MQTTHost: "broker.lan", MQTTUser: "ha", MQTTPass: setupMqttPass, AuthUser: "bob", AuthPass: setupAuthPass, NTP: []string{"pool.ntp.org"}, Settings: map[string]string{"audio.out_enable": "1", "isp.saturation": "-10"}}
	c, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse", Setup: setup})
	if err != nil {
		t.Fatal(err)
	}
	have := map[string]string{}
	for _, f := range c.Files {
		have[f.Path] = string(f.Data)
	}
	for _, p := range []string{"akam/akam.conf", "akam/mqtt", "akam/mqtt_pass", "akam/rtsp.conf", "akam/ntp"} {
		if have[p] == "" {
			t.Errorf("the card lacks %s", p)
		}
		if _, ok := c.Verify.Files[p]; !ok {
			t.Errorf("%s is not verified after the write", p)
		}
	}
	// a card with no setup carries none of them
	plain, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse"})
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range plain.Files {
		for _, n := range SetupFileNames {
			if f.Path == "akam/"+n {
				t.Errorf("a card without setup carries %s", f.Path)
			}
		}
	}
	if _, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse", Setup: &Setup2K{MQTTHost: "bad host"}}); err == nil {
		t.Error("a bad setup built a card")
	}
	// seed-2k copies the optional files too, once, 0600, never overwriting a differing file, saying names only
	files := map[string]string{"mnt/sdcard/akam/wpa.conf": seedWpa, "mnt/sdcard/akam/authorized_keys": seedKey}
	for p, s := range have {
		if strings.HasPrefix(p, "akam/") && contains(SetupFileNames, strings.TrimPrefix(p, "akam/")) {
			files["mnt/sdcard/"+p] = s
		}
	}
	root := seedRoot(t, true, files)
	r, err := seedWith(context.Background(), localSh{}, root, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.Join(r.Lines, "|"), "rtsp.conf: seeded") || !strings.Contains(strings.Join(r.Lines, "|"), "mqtt_pass: seeded") {
		t.Fatalf("lines: %q", r.Lines)
	}
	for _, n := range SetupFileNames {
		p := filepath.Join(root, "config/akam", n)
		b, err := os.ReadFile(p)
		if err != nil || string(b) != have["akam/"+n] {
			t.Errorf("%s: %v", n, err)
		}
		if st, _ := os.Stat(p); st == nil || st.Mode().Perm() != 0o600 {
			t.Errorf("%s mode", n)
		}
	}
	for _, secret := range []string{setupMqttPass, setupAuthPass} {
		if strings.Contains(strings.Join(r.Lines, "\n"), secret) {
			t.Error("a password reached the output")
		}
	}
	if err := os.WriteFile(filepath.Join(root, "config/akam/mqtt"), []byte("enabled=0\nhost=other.lan\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r, err = seedWith(context.Background(), localSh{}, root, nil)
	if err != nil || !strings.Contains(strings.Join(r.Lines, "|"), "mqtt: kept, differs") {
		t.Fatalf("second run: %v %q", err, r.Lines)
	}
}

func contains(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}
