package appfs

import "testing"

// The speaker, two-way audio and picture answers of the install-time setup travel as the file "settings" of the first-install card (the same
// mechanism as the other optional files; seed-2k copies it to /config/akam once, never over an existing one).
func TestSetup2KSettingsFile(t *testing.T) {
	s := &Setup2K{Settings: map[string]string{"audio.out_volume": "060", "isp.denoise": "2", "audio.out_enable": "on"}}
	f, err := s.Files()
	if err != nil {
		t.Fatal(err)
	}
	if got := string(f["settings"]); got != "isp.denoise=2\naudio.out_enable=1\naudio.out_volume=60\n" || len(f) != 1 {
		t.Errorf("settings file %q (%d files)", got, len(f))
	}
	for name, m := range map[string]map[string]string{
		"range":      {"audio.out_volume": "101"},
		"not kept":   {"img.wb": "manual"},
		"unknown":    {"audio.nothing": "1"},
		"read only":  {"audio.out_state": "idle"},
		"saturation": {"isp.saturation": "-51"},
		"line feed":  {"net.hostname": "a\nb"},
	} {
		if _, err := (&Setup2K{Settings: m}).Files(); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	if f, err := (&Setup2K{}).Files(); err != nil || len(f) != 0 {
		t.Errorf("no settings: %v %v", f, err)
	}
}
