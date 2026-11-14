package cli_test

import (
	"strings"
	"testing"
)

// home 3 = camera options; groups: 1 LED, 2 Picture, 3 Video, 4 Audio, 5 Network, 6 Security, 7 Reload, 8 Back.
func TestOptionsMainScreenShowsEveryGroupInOneRead(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("3\n8\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Camera options - salon (127.0.0.1)   Akam b35-2af81c5",
		"LED and night", "front LED: auto   IR LEDs: auto   night: auto*",
		"brightness 0  contrast 0  saturation 0  sharpness 0",
		"bitrate 1536 kbit/s   15 fps   key frame every 2 s",
		"ONVIF: off   NTP: off   name: ", "(read-only)", "video password: off",
		"* not saved: back to the default at the next reboot")
	// the control port got one batch of GET lines for the whole screen, and nothing else
	for _, w := range e.cam.CtlWords() {
		if w != "GET" && w != "QUIT" {
			t.Errorf("control port received %q", w)
		}
	}
	if len(e.cam.CtlSets()) != 0 {
		t.Errorf("sets: %v", e.cam.CtlSets())
	}
}

// Values are edited locally, validated, and only sent on apply; the video restart is asked, default No.
func TestOptionsVideoEditApplyAndRestartDefaultsToNo(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	// bitrate: 9999 is refused (range), 2000 kept; apply; restart question: Enter = No
	r := e.run("3\n3\n1\n9999\n2000\n4\n\n6\n8\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Bitrate (kbit/s) needs a whole number from 500 to 3000.", "-> 2000 (pending)",
		"WILL send: SET video.bitrate_cfg 2000", "Bitrate (kbit/s): 2000 (applies at the next restart of the video service; saved on the camera).",
		"It applies when the video service restarts.",
		"Not restarted: the new values apply at the next restart of the video service or the next reboot.")
	contains(t, r.err, "Restart the video service now? The stream stops for a few seconds and starts again by itself. [y/N]")
	if got := strings.Join(e.cam.CtlSets(), "|"); got != "SET video.bitrate_cfg 2000" {
		t.Errorf("sets: %s", got)
	}
	if e.cam.VideoRestarts() != 0 {
		t.Error("restarted without a yes")
	}
	// yes: the single audited restart, and only then
	r = e.run("3\n3\n2\n1\n4\ny\n6\n8\n10\n", "--camera", "salon") // fps > 2nd value (15)... apply, yes
	expectRC(t, r, 0)
	if e.cam.VideoRestarts() != 1 {
		t.Errorf("restarts: %d\n%s", e.cam.VideoRestarts(), r.out)
	}
	contains(t, r.out, "The video service was asked to restart")
	// no terminal: nothing is asked and nothing restarts
	t.Setenv("AKAM_FORCE_TTY", "")
	r = e.run("3\n", "--camera", "salon")
	if e.cam.VideoRestarts() != 1 {
		t.Error("restarted without a terminal")
	}
}

// Pending changes are not sent when the user leaves; nothing is written without apply.
func TestOptionsDiscardAndLeaveSendNothing(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("3\n1\n1\n2\n6\n8\n10\n", "--camera", "salon") // LED > front LED > off > Back (pending discarded)
	expectRC(t, r, 0)
	contains(t, r.out, "Nothing was sent: the pending changes are discarded.")
	r = e.run("3\n1\n1\n2\n5\n6\n8\n10\n", "--camera", "salon") // ... > Discard
	expectRC(t, r, 0)
	contains(t, r.out, "Discarded.")
	if len(e.cam.CtlSets()) != 0 {
		t.Errorf("sets: %v", e.cam.CtlSets())
	}
}

func TestOptionsLedMicAndAnOptionTheImageLacks(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	e.cam.SetCtl("video.gop_cfg", "-") // an older image: ERR for that key
	r := e.run("3\n1\n1\n2\n4\n6\n8\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Front LED: off (applies now; saved on the camera).")
	r = e.run("3\n4\n4\n1\n5\n6\n8\n10\n", "--camera", "salon") // audio > microphone > off > apply
	expectRC(t, r, 0)
	contains(t, r.out, "Microphone: 0 (applies at the next power cycle; saved on the camera).", "the assistant never does it for you")
	r = e.run("3\n3\n3\n6\n8\n10\n", "--camera", "salon") // video > key frames (not available)
	expectRC(t, r, 0)
	contains(t, r.out, "not available on this image (update first)", "does not have that option")
	got := strings.Join(e.cam.CtlSets(), "|")
	if got != "SET led.status off|SET audio.mic_enable 0" {
		t.Errorf("sets: %s", got)
	}
}

// Wi-Fi is shown and never editable; NTP and the host name go through their helper commands.
func TestOptionsNetworkAndSecurity(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("3\n5\n4\n8\n10\n", "--camera", "salon") // network > Wi-Fi
	expectRC(t, r, 0)
	contains(t, r.out, "Wi-Fi: ", "(read-only: changing it needs the flash card)", "cannot be changed from here")
	r = e.run("3\n5\n2\nbad_name\n8\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "is not a host name or an IPv4 address")
	r = e.run("3\n5\n2\npool.ntp.org\n8\n10\n", "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Time sync on, servers: pool.ntp.org")
	r = e.run("3\n6\n3\n8\n10\n", "--camera", "salon") // security > back
	expectRC(t, r, 0)
	contains(t, r.out, "video password: off")
}

func TestOptionsRehearsalSendsNothing(t *testing.T) {
	e := newEnv(t)
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("", "--dry-run", "--camera", "salon")
	expectRC(t, r, 0)
	if len(e.cam.Commands()) != 0 || len(e.cam.CtlSets()) != 0 {
		t.Errorf("commands in a rehearsal: %v", e.cam.Commands())
	}
}
