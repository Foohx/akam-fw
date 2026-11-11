package cli_test

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const talkPass = "open sesame 42"

// talkEnv has a camera whose stream password is already set (through the real helper script).
func talkEnv(t *testing.T) *env {
	e := newEnv(t)
	e.needHelpers()
	expectRC(t, e.run("viewer\n"+talkPass+"\n"+talkPass+"\n", "rtsp-auth", "set"), 0)
	return e
}

func pcmBytes(samples []int16) []byte {
	var b bytes.Buffer
	_ = binary.Write(&b, binary.LittleEndian, samples)
	return b.Bytes()
}

func wavFile(t *testing.T, rate uint32, channels uint16, samples []int16) string {
	t.Helper()
	data := pcmBytes(samples)
	var b bytes.Buffer
	b.WriteString("RIFF")
	_ = binary.Write(&b, binary.LittleEndian, uint32(36+len(data)))
	b.WriteString("WAVEfmt ")
	for _, v := range []any{uint32(16), uint16(1), channels, rate, rate * uint32(channels) * 2, channels * 2, uint16(16)} {
		_ = binary.Write(&b, binary.LittleEndian, v)
	}
	b.WriteString("data")
	_ = binary.Write(&b, binary.LittleEndian, uint32(len(data)))
	b.Write(data)
	p := filepath.Join(t.TempDir(), "voice.wav")
	if err := os.WriteFile(p, b.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestTalkTestTone(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	r := e.run("", "talk", "test")
	expectRC(t, r, 0)
	contains(t, r.out, "Talk to the camera", "  Sending a 2 second test tone to the speaker ...", "  Done. Heard it? If not: ./akam audio (volume, beep)")
	pk := e.cam.RTSP.Packets()
	if len(pk) != 100 { // 2 s at 20 ms
		t.Fatalf("%d RTP packets, want 100", len(pk))
	}
	for i, p := range pk {
		if len(p) != 12+160 || p[0] != 0x80 {
			t.Fatalf("packet %d: len %d", i, len(p))
		}
		if i == 0 && p[1] != 0x80 {
			t.Error("no marker bit on the first packet")
		}
		if i > 0 && p[1] != 0 {
			t.Errorf("marker on packet %d", i)
		}
		if i > 0 && binary.BigEndian.Uint16(p[2:]) != binary.BigEndian.Uint16(pk[i-1][2:])+1 {
			t.Fatalf("sequence jump at %d", i)
		}
	}
	if !e.cam.RTSP.TornDown() || len(e.cam.RTSP.RTCP()) != 1 {
		t.Error("no RTCP BYE and TEARDOWN")
	}
	if !strings.Contains(e.cam.RTSP.DescribeRequire(), "www.onvif.org/ver20/backchannel") {
		t.Errorf("Require: %q", e.cam.RTSP.DescribeRequire())
	}
	reqs := strings.Join(e.cam.RTSP.Requests(), "|")
	if !strings.HasPrefix(reqs, "DESCRIBE rtsp://127.0.0.1:") || strings.Count(reqs, "DESCRIBE") != 2 { // first without, then with the Digest login
		t.Errorf("requests: %s", reqs)
	}
	// the password was never printed, logged or sent in a command
	notContains(t, r.out+r.err+e.logText(), talkPass)
	for _, c := range e.cam.Commands() {
		notContains(t, c, talkPass)
	}
}

func TestTalkPasswordPromptWrongPasswordAndFile(t *testing.T) {
	e := talkEnv(t)
	// typed (no echo on a terminal; here a scripted line)
	r := e.run(talkPass+"\n", "talk", "test")
	expectRC(t, r, 0)
	contains(t, r.err, "Stream password of user viewer (not shown):")
	notContains(t, r.out+r.err+e.logText(), talkPass)
	// wrong password: the camera refuses the login
	r = e.run("wrong password\n", "talk", "test")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: the camera refused the user name or password.")
	contains(t, r.out, "What to do: check ./akam rtsp-auth status; run ./akam rtsp-auth set to choose a new one.")
	notContains(t, r.out+r.err+e.logText(), "wrong password")
	// a file
	f := filepath.Join(t.TempDir(), "pw")
	if err := os.WriteFile(f, []byte(talkPass+"\nsecond line ignored\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	expectRC(t, e.run("", "--password-file", f, "talk", "test"), 0)
}

func TestTalkRefusedAndNoStreamPassword(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	e.cam.RTSP.Configure(true, false, "")
	r := e.run("", "talk", "test")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: the camera does not offer talk.")
	e.cam.RTSP.Configure(false, true, "")
	r = e.run("", "talk", "test")
	expectRC(t, r, 1)
	contains(t, r.err, "the camera does not offer talk.")
	// the stream password is off: no talk, and the question defaults to No
	e2 := newEnv(t)
	e2.needHelpers()
	r = e2.run("\n", "talk", "test")
	expectRC(t, r, 1)
	contains(t, r.out, "  Talk needs the video stream password: the camera never offers talk without one.")
	contains(t, r.err, "ERROR: no stream password, no talk.")
	if len(e2.cam.RTSP.Requests()) != 0 {
		t.Error("RTSP was contacted without a password")
	}
}

func TestSayWav(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	one := make([]int16, 8000) // 1 s at 8 kHz mono
	for i := range one {
		one[i] = int16(i % 2000)
	}
	r := e.run("", "say", wavFile(t, 8000, 1, one))
	expectRC(t, r, 0)
	contains(t, r.out, "  Playing ", "  Done.")
	if n := len(e.cam.RTSP.Packets()); n != 50 {
		t.Errorf("%d packets for 1 s, want 50", n)
	}
	// another rate and two channels are converted in the binary: 1 s at 16 kHz stereo is still 1 s
	st := make([]int16, 32000)
	r = e.run("", "say", wavFile(t, 16000, 2, st))
	expectRC(t, r, 0)
	if n := len(e.cam.RTSP.Packets()); n != 100 {
		t.Errorf("%d packets after the second file, want 100", n)
	}
	if e.capture != nil {
		t.Error("a capture tool was used for a WAV file")
	}
}

func TestSayNeedsFfmpegForOtherFormatsAndNeverRunsIt(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	mp3 := filepath.Join(t.TempDir(), "x.mp3")
	if err := os.WriteFile(mp3, []byte("ID3 not really"), 0o600); err != nil {
		t.Fatal(err)
	}
	r := e.run("", "say", mp3)
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: ffmpeg is needed to convert "+mp3+".")
	if len(e.cam.RTSP.Requests()) != 0 {
		t.Error("the camera was contacted before the conversion tool was found")
	}
	// with a (fake) converter it plays
	e.capture = &fakeCapture{convPCM: pcmBytes(make([]int16, 1600))}
	r = e.run("", "say", mp3)
	expectRC(t, r, 0)
	if n := len(e.cam.RTSP.Packets()); n != 10 {
		t.Errorf("%d packets, want 10", n)
	}
	if _, cl, cv := e.capture.stats(); cv != 1 || cl != 1 {
		t.Errorf("converter started %d times, closed %d times: it must be stopped on every exit", cv, cl)
	}
	// missing file / usage
	expectRC(t, e.run("", "say", filepath.Join(t.TempDir(), "none.wav")), 1)
	expectRC(t, e.run("", "say"), 2)
}

func TestTalkMicrophone(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	// no tool installed: refused before anything is asked or sent
	n := len(e.cam.Commands())
	r := e.run("", "talk", "5")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: no way to read the microphone here (ffmpeg or arecord not found).")
	if len(e.cam.Commands()) != n {
		t.Error("the camera was contacted although there is no microphone tool")
	}
	// a fake microphone delivering 1.5 s, stopped at the end of its audio; always closed
	e.capture = &fakeCapture{micPCM: pcmBytes(make([]int16, 12000))}
	r = e.run("", "talk", "5")
	expectRC(t, r, 0)
	contains(t, r.out, "  Speak now, for at most 5 seconds.", "  Talk ended.")
	if n := len(e.cam.RTSP.Packets()); n != 75 {
		t.Errorf("%d packets, want 75", n)
	}
	if mc, cl, _ := e.capture.stats(); mc != 1 || cl < 1 {
		t.Errorf("microphone started %d times, closed %d times", mc, cl)
	}
	r = e.run("", "talk")
	contains(t, r.out, "  Speak now. The camera plays your voice; its own microphone is muted while you speak.", "  Press Ctrl-C to stop.")
	for _, bad := range [][]string{{"talk", "0"}, {"talk", "007"}, {"talk", "3601"}, {"talk", "x"}, {"talk", "1", "2"}} {
		expectRC(t, e.run("", bad...), 2)
	}
}
