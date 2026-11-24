package cli_test

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
)

// audio, say and talk on a 2K: the settings through the signed API, the beep through POST /actions/audio.beep, say and talk through
// the same RTSP backchannel client as the 1080p. Host-tested only (fake API, fake RTSP endpoint).

func TestAudio2KSettingsAndBeep(t *testing.T) {
	e := newEnv(t)
	f := e.with2K(nil)
	r := e.run("", "--camera", "default", "audio")
	expectRC(t, r, 0)
	contains(t, r.out, "Speaker", "Speaker volume", "Talk (two-way audio)", "Microphone")
	r = e.run("", "audio", "on")
	expectRC(t, r, 0)
	if f.Values["audio.out_enable"] != "1" {
		t.Fatalf("speaker not on: %v", f.Patches())
	}
	r = e.run("", "audio", "volume", "030")
	expectRC(t, r, 0)
	if f.Values["audio.out_volume"] != "30" {
		t.Errorf("volume %q", f.Values["audio.out_volume"])
	}
	for _, c := range [][]string{{"talk", "off", "audio.talk_enable", "0"}, {"talk", "on", "audio.talk_enable", "1"}, {"mic", "on", "audio.mic_enable", "1"}, {"off", "", "audio.out_enable", "0"}} {
		args := []string{"audio", c[0]}
		if c[1] != "" {
			args = append(args, c[1])
		}
		expectRC(t, e.run("", args...), 0)
		if f.Values[c[2]] != c[3] {
			t.Errorf("%v: %s = %q", args, c[2], f.Values[c[2]])
		}
	}
	// the beep: refused while the speaker is off (the camera's own answer, with the way out), played once it is on
	r = e.run("", "audio", "beep")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "the speaker is not running", "audio on")
	expectRC(t, e.run("", "audio", "on"), 0)
	r = e.run("", "audio", "beep")
	expectRC(t, r, 0)
	contains(t, r.out, "A short beep plays on the speaker")
	if f.Beeps != 1 {
		t.Errorf("%d beeps", f.Beeps)
	}
	allSigned(t, f)
	noSecret(t, r)
	for _, bad := range [][]string{{"audio", "volume"}, {"audio", "volume", "101"}, {"audio", "volume", "x"}, {"audio", "talk", "maybe"}, {"audio", "loud"}, {"audio", "beep", "now"}, {"audio", "mic"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	r = e.run("", "--dry-run", "--model", rotModel, "audio", "beep")
	expectRC(t, r, 0)
	contains(t, r.out, "POST /actions/audio.beep")
}

func tone2K(t *testing.T, e *env) {
	t.Helper()
	e.with2K(map[string]string{"rtsp.user": "viewer", "audio.out_enable": "1", "audio.talk_enable": "1"})
	e.cam.SetStreamLogin("viewer", talkPass)
}

func TestTalk2KUsesTheAPIAndTheBackchannel(t *testing.T) {
	e := newEnv(t)
	tone2K(t, e)
	e.pass = talkPass
	r := e.run("", "talk", "test")
	expectRC(t, r, 0)
	contains(t, r.out, "Sending a 2 second test tone to the speaker")
	if n := len(e.cam.RTSP.Packets()); n != 100 {
		t.Errorf("%d RTP packets", n)
	}
	if len(e.cam.Commands()) != 0 {
		t.Errorf("a 2K talk needs no SSH (the card-less layout): %v", e.cam.Commands())
	}
	notContains(t, r.out+r.err+e.logText(), talkPass)
	// the stream password comes from the secrets file of this camera's config when it was saved for this very user
	e.pass = ""
	e.writeSecrets(profile.Camera("default", e.state), profile.AuthPassword, talkPass, profile.AuthPasswordUser, "viewer")
	r = e.run("", "talk", "test")
	expectRC(t, r, 0)
	notContains(t, r.err, "Stream password of user")
	// ... and never for another user: then it is asked
	e.writeSecrets(profile.Camera("default", e.state), profile.AuthPassword, talkPass, profile.AuthPasswordUser, "other")
	r = e.run(talkPass+"\n", "talk", "test")
	expectRC(t, r, 0)
	contains(t, r.err, "Stream password of user viewer (not shown):")
}

func TestTalk2KNeedsThePasswordAndTheSpeaker(t *testing.T) {
	e := newEnv(t)
	e.with2K(map[string]string{"rtsp.user": "", "audio.out_enable": "1"})
	r := e.run("\n", "talk", "test") // no stream password: asked, default No
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no stream password, no talk.")
	e2 := newEnv(t)
	e2.with2K(map[string]string{"rtsp.user": "viewer", "audio.out_enable": "0"})
	e2.pass = talkPass
	r = e2.run("", "talk", "test")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "the speaker or the talk switch is off", "audio on")
	if len(e2.cam.RTSP.Requests()) != 0 {
		t.Error("RTSP was contacted with the speaker off")
	}
}

func TestSay2KPlaysAFile(t *testing.T) {
	e := newEnv(t)
	tone2K(t, e)
	e.pass = talkPass
	wav := wavFile(t, 8000, 1, make([]int16, 1600))
	r := e.run("", "say", wav)
	expectRC(t, r, 0)
	contains(t, r.out, "Playing", "Done.")
	if n := len(e.cam.RTSP.Packets()); n != 10 {
		t.Errorf("%d RTP packets, want 10 (0.2 s)", n)
	}
	if strings.Contains(r.out+r.err, "not yet supported for this model") {
		t.Error("say refused on a 2K")
	}
	r = e.run("", "--dry-run", "--model", rotModel, "talk", "test")
	expectRC(t, r, 0)
	contains(t, r.out, "signed API of the 2K")
}
