package camopt

import (
	"strings"
	"testing"
)

func TestSettingsText2K(t *testing.T) {
	got, err := SettingsText2K(map[string]string{"isp.denoise": "2", "audio.out_enable": "on", "audio.out_volume": "060", "isp.saturation": "-10", "audio.talk_enable": "0"})
	if err != nil {
		t.Fatal(err)
	}
	// catalogue order (Picture before Audio), canonical values
	want := "isp.saturation=-10\nisp.denoise=2\naudio.out_enable=1\naudio.out_volume=60\naudio.talk_enable=0\n"
	if got != want {
		t.Errorf("got %q want %q", got, want)
	}
	if s, err := SettingsText2K(nil); err != nil || s != "" {
		t.Errorf("no answers: %q %v", s, err)
	}
}

func TestSettingsText2KRefuses(t *testing.T) {
	for name, m := range map[string]map[string]string{
		"out of range":      {"audio.out_volume": "101"},
		"saturation":        {"isp.saturation": "60"},
		"denoise":           {"isp.denoise": "5"},
		"unknown":           {"audio.nothing": "1"},
		"read only":         {"audio.out_state": "idle"},
		"RAM only (wb)":     {"img.wb": "manual"},
		"RAM only (amp)":    {"audio.out_amp": "on"},
		"newline injection": {"net.hostname": "a\nb"},
		"bool":              {"audio.out_enable": "maybe"},
	} {
		if s, err := SettingsText2K(m); err == nil {
			t.Errorf("%s accepted as %q", name, s)
		}
	}
}

func TestSetupKeys2KAreAskable(t *testing.T) {
	for _, k := range SetupKeys2K {
		o, ok := Find2K(k)
		if !ok || o.Persisted != PersistCamera || o.ReadOnly {
			t.Errorf("%s cannot be asked at install time", k)
		}
		if strings.Contains(o.Key, "wb") {
			t.Errorf("%s is RAM only", k)
		}
	}
}
