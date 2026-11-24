package cli

import (
	"strings"
	"testing"
)

// The speaker, two-way audio and picture questions of the install-time setup of the 2K (the same flow as the services: prepare-2k-sd). They end the
// flow, every one defaults to keeping the camera's own value, and the answers become the file "settings" of the card.

// "y": set up; then No to mic, stream password, MQTT, time sync (ONVIF is not asked without the password); then the speaker questions.
const setupNoServices = "y\nn\nn\nn\nn\n"

func TestAskSetup2KSpeakerAndPicture(t *testing.T) {
	// speaker yes; volume 60; talk yes; saturation -10; denoise 2
	a, out := setupApp(setupNoServices + "y\n60\ny\n-10\n2\n")
	s, err := a.askSetup2K()
	if err != nil || s == nil {
		t.Fatalf("%v %v\n%s", s, err, out)
	}
	want := map[string]string{"audio.out_enable": "1", "audio.out_volume": "60", "audio.talk_enable": "1", "isp.saturation": "-10", "isp.denoise": "2"}
	for k, v := range want {
		if s.Settings[k] != v {
			t.Errorf("%s = %q want %q", k, s.Settings[k], v)
		}
	}
	files, ferr := s.Files()
	if ferr != nil || len(files) != 1 || string(files["settings"]) != "isp.saturation=-10\nisp.denoise=2\naudio.out_enable=1\naudio.out_volume=60\naudio.talk_enable=1\n" {
		t.Errorf("files: %q %v", files, ferr)
	}
	if !strings.Contains(out.String(), "The card will also carry: settings") {
		t.Errorf("the summary lacks the file name:\n%s", out)
	}
}

func TestAskSetup2KSpeakerOffAsksNoAudioDetails(t *testing.T) {
	a, out := setupApp(setupNoServices + "n\n5\n\n")
	s, err := a.askSetup2K()
	if err != nil || s == nil {
		t.Fatalf("%v %v", s, err)
	}
	if len(s.Settings) != 1 || s.Settings["isp.saturation"] != "5" || strings.Contains(out.String(), "Speaker volume") {
		t.Errorf("%v\n%s", s.Settings, out)
	}
}

func TestAskSetup2KBadNumbersAreAskedAgain(t *testing.T) {
	a, out := setupApp(setupNoServices + "y\n150\nabc\n40\nn\n99\n20\n9\n\n")
	s, err := a.askSetup2K()
	if err != nil || s == nil {
		t.Fatalf("%v %v", s, err)
	}
	if s.Settings["audio.out_volume"] != "40" || s.Settings["audio.talk_enable"] != "0" || s.Settings["isp.saturation"] != "20" {
		t.Errorf("%v", s.Settings)
	}
	if _, ok := s.Settings["isp.denoise"]; ok {
		t.Errorf("denoise 9 must not be kept: %v", s.Settings)
	}
	if strings.Count(out.String(), "a whole number from") < 4 {
		t.Errorf("each bad answer is explained:\n%s", out)
	}
}

func TestAskSetup2KSpeakerWithServices(t *testing.T) {
	// microphone yes, no stream password, no MQTT, no NTP, then the speaker on with defaults: one flow, two kinds of files
	a, _ := setupApp("y\ny\nn\nn\nn\ny\n\nn\n\n\n")
	s, err := a.askSetup2K()
	if err != nil || s == nil || !s.Mic || s.Settings["audio.out_enable"] != "1" {
		t.Fatalf("%+v %v", s, err)
	}
	files, _ := s.Files()
	if len(files) != 2 || files["akam.conf"] == nil || files["settings"] == nil {
		t.Errorf("files: %v", files)
	}
}
