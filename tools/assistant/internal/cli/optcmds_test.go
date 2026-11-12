package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

func TestVideoShowAndSet(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "video")
	expectRC(t, r, 0)
	contains(t, r.out, "Video settings", "  Bitrate cap: 1536 kbit/s (500 to 3000; the default is 1536).",
		"  Frame rate: 15 per second (10 or 15; the default is 15).", "  Key frame every 2 second(s) (1 to 4; the default is 2).",
		"  Measured now: 1480 kbit/s, 150 (tenths of a frame per second).",
		"  Resolution and flip/mirror cannot be changed in this version")
	notContains(t, r.out, "not running yet")

	// default No: saved, nothing restarted, and the new values are shown as pending
	r = e.run("\n", "video", "bitrate", "02000", "gop", "3")
	expectRC(t, r, 0)
	contains(t, r.out, "  Saved on the camera.", "  It applies at the next restart of the video service.",
		"  Not restarted: the new values apply at the next restart of the video service or the next reboot.",
		"  Bitrate cap: 2000 kbit/s", "not running yet")
	contains(t, r.err, "Restart the video service now? The stream stops for a few seconds and starts again by itself. [y/N]")
	if e.cam.VideoRestarts() != 0 {
		t.Error("restarted without a yes")
	}
	got := strings.Join(e.cam.CtlSets(), "|")
	if got != "SET video.bitrate_cfg 2000|SET video.gop_cfg 3" {
		t.Errorf("sets: %s", got)
	}

	// no terminal: refused to ask, so not restarted either
	t.Setenv("AKAM_FORCE_TTY", "")
	r = e.run("y\n", "video", "fps", "10")
	expectRC(t, r, 0)
	if e.cam.VideoRestarts() != 0 {
		t.Error("restarted without a terminal")
	}
	t.Setenv("AKAM_FORCE_TTY", "1")

	// yes: the audited kill, once
	r = e.run("y\n", "video", "fps", "15")
	expectRC(t, r, 0)
	contains(t, r.out, "The video service was asked to restart; it is back in about 20 seconds (./akam video show).")
	if e.cam.VideoRestarts() != 1 {
		t.Errorf("restarts: %d", e.cam.VideoRestarts())
	}
	// back to the running values: nothing pending, so no question
	e.cam.SetCtl("video.bitrate_cfg", "1536")
	for _, in := range []string{"", "\n", "n\n", "q\n", "no\n"} {
		r = e.run(in, "video", "bitrate", "1536", "gop", "3")
		expectRC(t, r, 0)
	}
}

func TestVideoRestartIsTheOnlyKillAndMatchesTheExactCommand(t *testing.T) {
	e := newEnv(t)
	expectRC(t, e.run("y\n", "video", "bitrate", "700"), 0)
	var kills []string
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "kill") {
			kills = append(kills, c)
		}
		if strings.Contains(strings.ToLower(c), "watchdog") {
			t.Errorf("watchdog in %q", c)
		}
	}
	if len(kills) != 1 || kills[0] != `for p in $(pidof ak_rtsp); do grep -qa akam_klog /proc/$p/cmdline || kill $p; done` {
		t.Errorf("kill commands: %q", kills)
	}
}

func TestVideoRefusesBadWordsBeforeTouchingTheCamera(t *testing.T) {
	e := newEnv(t)
	for _, bad := range [][]string{{"video", "bitrate", "499"}, {"video", "bitrate", "3001"}, {"video", "bitrate", "x"}, {"video", "bitrate"},
		{"video", "bitrate", "1234567"}, {"video", "fps", "12"}, {"video", "fps", "010"}, {"video", "gop", "0"}, {"video", "gop", "5"},
		{"video", "fps", "10", "fps", "15"}, {"video", "show", "gop", "2"}, {"video", "flip"}, {"video", "mirror", "on"}, {"video", "resolution"}, {"video", "zoom"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	if len(e.cam.CtlSets()) != 0 || len(e.cam.Commands()) != 0 {
		t.Error("the camera was reached")
	}
}

func TestVideoOldImageAndRefusal(t *testing.T) {
	e := newEnv(t)
	e.cam.SetCtl("video.fps_cfg", "-")
	r := e.run("", "video")
	expectRC(t, r, 1)
	contains(t, r.err, "the camera did not answer the video settings.")
	// an unsafe value from the camera is not shown
	e = newEnv(t)
	e.cam.SetCtl("video.gop_cfg", "2\x1b[31m")
	r = e.run("", "video")
	expectRC(t, r, 1)
	notContains(t, r.out+r.err, "\x1b[31m")
}

func TestNtp(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "ntp")
	expectRC(t, r, 0)
	contains(t, r.out, "Time sync: status", "  time sync: off (default)")
	r = e.run("", "ntp", "set", "pool.ntp.org", "192.0.2.1")
	expectRC(t, r, 0)
	contains(t, r.out, "  The camera will ask pool.ntp.org 192.0.2.1 for the time at start-up and every 6 hours (UDP 123, outgoing only).",
		"  Nothing is sent unless you do this; ./akam ntp off turns it off again.", "  Time sync on, servers: pool.ntp.org 192.0.2.1.")
	r = e.run("", "ntp", "status")
	contains(t, r.out, "  time sync: on, servers: pool.ntp.org 192.0.2.1")
	r = e.run("", "ntp", "off")
	contains(t, r.out, "  Time sync off:")
	var ntp []string
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "akam_ntp.sh") {
			ntp = append(ntp, c)
		}
	}
	want := "/usr/sbin/akam_ntp.sh status|/usr/sbin/akam_ntp.sh set pool.ntp.org 192.0.2.1|/usr/sbin/akam_ntp.sh status|/usr/sbin/akam_ntp.sh off"
	if strings.Join(ntp, "|") != want {
		t.Errorf("commands: %q", ntp)
	}
}

func TestNtpBadNamesNeverReachTheCamera(t *testing.T) {
	e := newEnv(t)
	for _, s := range []string{"bad_name", "a;reboot", "$(id)", "a b", "-x", "a'b", "a..b", ".a", "a.", "x\ny", "x\x1b", strings.Repeat("a", 254)} {
		expectRC(t, e.run("", "ntp", "set", s), 2)
	}
	expectRC(t, e.run("", "ntp", "set"), 2)
	expectRC(t, e.run("", "ntp", "set", "a", "b", "c", "d"), 2)
	expectRC(t, e.run("", "ntp", "off", "now"), 2)
	expectRC(t, e.run("", "ntp", "frob"), 2)
	if len(e.cam.Commands()) != 0 {
		t.Errorf("reached the camera: %q", e.cam.Commands())
	}
}

func TestNtpOldImage(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.NoNtpHelper = true })
	r := e.run("", "ntp", "set", "pool.ntp.org")
	expectRC(t, r, 1)
	contains(t, r.err, "this camera does not have /usr/sbin/akam_ntp.sh.")
	contains(t, r.out, "it runs an older Akam image: ./akam update.")
}

func TestAudioMic(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "audio")
	contains(t, r.out, "  Microphone: on (off = never opened and no audio track in the stream; read at start-up).",
		"  Volume, speaker, talk and microphone switches are saved on the camera and kept after a reboot.")
	r = e.run("", "audio", "mic", "off")
	expectRC(t, r, 0)
	contains(t, r.out, "microphone setting is saved, but the camera reads it at start-up", "  Microphone: off")
	if e.cam.CtlValue("audio.mic_enable") != "0" {
		t.Error("not set")
	}
	r = e.run("", "audio", "mic", "on")
	expectRC(t, r, 0)
	for _, bad := range [][]string{{"audio", "mic"}, {"audio", "mic", "maybe"}, {"audio", "mic", "on", "off"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	// an old image without the key: the line is simply absent
	e.cam.SetCtl("audio.mic_enable", "-")
	r = e.run("", "audio")
	expectRC(t, r, 0)
	notContains(t, r.out, "Microphone:")
}

func TestNewCommandsLeaveNoSecretOrStateFile(t *testing.T) {
	e := newEnv(t)
	expectRC(t, e.run("", "video"), 0)
	expectRC(t, e.run("", "ntp"), 0)
	if _, err := os.Stat(filepath.Join(e.state, "state")); err == nil {
		b, _ := os.ReadFile(filepath.Join(e.state, "state"))
		if strings.Contains(string(b), "ntp") {
			t.Errorf("state: %s", b)
		}
	}
}
