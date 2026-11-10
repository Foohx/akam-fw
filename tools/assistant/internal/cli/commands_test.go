package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// ---------------------------------------------------------------- led

func TestLedShowAndSet(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "Front LED and IR LEDs", "  Front LED: auto (now: idle).", "  IR LEDs at night: auto (they light in night mode).",
		"  Both settings are saved on the camera and kept after a reboot.")
	notContains(t, r.out, "LOG ") // the camera's own log lines never reach the user

	r = e.run("", "led", "status", "off", "ir", "off")
	expectRC(t, r, 0)
	contains(t, r.out, "  Done.", "  Front LED: off.", "  IR LEDs at night: off (never lit; the night image still switches on).")
	if got := e.cam.CtlSets(); len(got) != 2 || got[0] != "SET led.status off" || got[1] != "SET night.ir_led off" {
		t.Errorf("control lines: %v", got)
	}
}

func TestLedErrors(t *testing.T) {
	e := newEnv(t)
	for _, c := range []struct {
		args []string
		msg  string
	}{
		{[]string{"led", "status", "blue"}, "akam: led status needs on, off or auto (see ./akam help)"},
		{[]string{"led", "ir", "on"}, "akam: led ir needs auto or off (see ./akam help)"},
		{[]string{"led", "status", "on", "status", "off"}, "akam: led status given twice (see ./akam help)"},
		{[]string{"led", "show", "ir", "off"}, "akam: led: show cannot be combined with status or ir (see ./akam help)"},
		{[]string{"led", "party"}, "akam: led: unknown word 'party' (see ./akam help)"},
	} {
		r := e.run("", c.args...)
		expectRC(t, r, 2)
		contains(t, r.err, c.msg)
	}
	if len(e.cam.Commands()) != 0 {
		t.Error("a usage error reached the camera")
	}
	e.cam.SetCtl("led.status", "-") // an older image
	r := e.run("", "led")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: the camera did not answer the LED settings.")
	contains(t, r.out, "What to do: it may run an older Akam image without them (./akam update)")
	e.cam.SetCtl("led.status", "auto")
	r = e.run("", "led", "status", "on")
	e.cam.SetCtl("night.ir_led", "-")
	r = e.run("", "led", "ir", "off")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: the camera refused night.ir_led off: unknown param night.ir_led")
}

func TestWithoutAnAddressNothingIsSent(t *testing.T) {
	e := newEnv(t)
	r := e.runRaw("", "--no-color", "--state-dir", e.state, "led")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: the camera address is not known yet.")
	contains(t, r.out, "What to do: run: ./akam step find-camera (or pass --ip ADDRESS)")
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "-x", "led")
	expectRC(t, r, 1) // an option-looking address is never used as a host
	contains(t, r.err, "ERROR: '-x' is not a valid camera address.")
}

// ---------------------------------------------------------------- audio

func TestAudio(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "audio")
	expectRC(t, r, 0)
	contains(t, r.out, "  Speaker output: on (state: idle); the on/off choice is read at start-up.", "  Volume: 60 (0 to 100).",
		"  Talk (RTSP audio backchannel): on.", "  Playback counters: underruns 0, drops 0.")

	r = e.run("", "audio", "volume", "030")
	expectRC(t, r, 0)
	contains(t, r.out, "  Done.", "  Volume: 30 (0 to 100).")
	r = e.run("", "audio", "beep")
	contains(t, r.out, "A short beep plays on the speaker")
	r = e.run("", "audio", "off")
	contains(t, r.out, "The speaker is off now and stays off after a reboot (./akam audio on to enable it again).", "Speaker output: off")
	r = e.run("", "audio", "talk", "off")
	contains(t, r.out, "Talk (RTSP audio backchannel): off.")
	r = e.run("", "audio", "on")
	contains(t, r.out, "  Done. The speaker is enabled.")
	want := []string{"SET audio.out_volume 30", "SET audio.out_test 1", "SET audio.out_enable 0", "SET audio.talk_enable 0", "SET audio.out_enable 1"}
	got := e.cam.CtlSets()
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Errorf("control lines:\n got %v\nwant %v", got, want)
	}
	for _, bad := range [][]string{{"audio", "volume"}, {"audio", "volume", "101"}, {"audio", "volume", "1000"}, {"audio", "volume", "x"}, {"audio", "talk", "maybe"}, {"audio", "loud"}, {"audio", "beep", "now"}} {
		expectRC(t, e.run("", bad...), 2)
	}
}

// ---------------------------------------------------------------- picture

func TestPicture(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "picture")
	expectRC(t, r, 0)
	contains(t, r.out, "  Brightness 0, contrast 0, saturation 0, sharpness 0 (each -50 to 50, 0 = unchanged). Saved on the camera.",
		"  White balance: conf", "  Day/night: auto")
	r = e.run("", "picture", "contrast", "-20", "brightness", "5", "wb", "manual", "night", "day")
	expectRC(t, r, 0)
	contains(t, r.out, "  Done.", "Brightness 5, contrast -20")
	want := "SET isp.contrast -20|SET isp.brightness 5|SET img.wb manual|SET night.mode day"
	if got := strings.Join(e.cam.CtlSets(), "|"); got != want {
		t.Errorf("control lines (one SET each, in the order given):\n got %s\nwant %s", got, want)
	}
	r = e.run("", "picture", "reset")
	contains(t, r.out, "Done. Brightness, contrast, saturation and sharpness are back to 0 and the saved file is removed.")
	if e.cam.CtlValue("isp.contrast") != "0" {
		t.Error("reset did not reach the camera")
	}
	for _, bad := range [][]string{{"picture", "contrast", "51"}, {"picture", "contrast", "--5"}, {"picture", "contrast"}, {"picture", "wb", "x"},
		{"picture", "night", "x"}, {"picture", "contrast", "1", "contrast", "2"}, {"picture", "reset", "contrast", "1"}, {"picture", "show", "reset"}, {"picture", "blur"}} {
		expectRC(t, e.run("", bad...), 2)
	}
	// a camera that refuses a value: the error is shown and the next SET is not sent
	n := len(e.cam.CtlSets())
	e.cam.SetCtl("isp.sharpness", "-")
	r = e.run("", "picture", "sharpness", "3", "night", "auto")
	expectRC(t, r, 1)
	contains(t, r.err, "the camera refused isp.sharpness 3")
	if len(e.cam.CtlSets()) != n+1 {
		t.Errorf("sets after the refusal: %v", e.cam.CtlSets()[n:])
	}
}

// ---------------------------------------------------------------- debug (real helper script)

func TestDebug(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	r := e.run("", "debug")
	expectRC(t, r, 0)
	contains(t, r.out, "  debug off (the control port is read-only for the network)")

	// default No: Enter, n, q, end of input
	for _, in := range []string{"\n", "n\n", "q\n", ""} {
		r = e.run(in, "debug", "on", "30")
		expectRC(t, r, 0)
		contains(t, r.out, "  Left as it was (read-only).")
	}
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "debug.sh on") {
			t.Fatalf("debug on was sent without a yes: %s", c)
		}
	}
	r = e.run("y\n", "debug", "on", "30")
	expectRC(t, r, 0)
	contains(t, r.out, "debug on: the control port accepts writes from the network for 30 minutes", "WARNING: anyone on your network can now change the camera settings through port 8091 until it is switched off: ./akam debug off.")
	r = e.run("", "debug", "status")
	contains(t, r.out, "debug on, 1800 s left")
	r = e.run("", "debug", "off")
	contains(t, r.out, "debug off: the control port is read-only for the network", "  Port 8091 is read-only for the network again.")
	// no terminal: refuses, asks nothing
	t.Setenv("AKAM_FORCE_TTY", "")
	r = e.run("y\n", "debug", "on")
	expectRC(t, r, 1)
	contains(t, r.err, "allowing writes on the control port needs an interactive terminal and refuses to run without one.")
	for _, bad := range [][]string{{"debug", "on", "0"}, {"debug", "on", "1441"}, {"debug", "on", "x"}, {"debug", "on", "1", "2"}, {"debug", "off", "1"}, {"debug", "loud"}} {
		expectRC(t, e.run("", bad...), 2)
	}
}

// ---------------------------------------------------------------- onvif

func TestOnvif(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "onvif")
	expectRC(t, r, 0)
	contains(t, r.out, "  ONVIF: disabled (default)")
	// enable asks first, default No
	r = e.run("\n", "onvif", "enable", "8080")
	expectRC(t, r, 0)
	contains(t, r.out, "This opens port 8080 (TCP) and UDP 3702 on your network, and needs the stream password.", "  Left as it was (ONVIF off).")
	// without a stream password the camera refuses
	r = e.run("y\n", "onvif", "enable")
	expectRC(t, r, 1)
	contains(t, r.out, "akam_onvif: ONVIF needs a password")
	contains(t, r.err, "the camera did not run /usr/sbin/akam_onvif.sh enable.")
	e.needHelpers()
	expectRC(t, e.run("viewer\nsupersecret1\nsupersecret1\n", "rtsp-auth", "set"), 0)
	r = e.run("y\n", "onvif", "enable", "8080")
	expectRC(t, r, 0)
	contains(t, r.out, "  ONVIF enabled on port 8080", "  ONVIF is on. In your app, add the camera by its address (port 8080 unless you chose another), with your stream user and password.")
	r = e.run("", "onvif", "disable")
	contains(t, r.out, "  ONVIF is off: the ONVIF ports are closed again.")
	for _, bad := range [][]string{{"onvif", "enable", "0"}, {"onvif", "enable", "70000"}, {"onvif", "enable", "1", "2"}, {"onvif", "disable", "x"}, {"onvif", "frob"}} {
		expectRC(t, e.run("", bad...), 2)
	}
}

func TestOnvifOldImage(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.NoOnvifHelper = true })
	r := e.run("", "onvif", "status")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: this camera does not have /usr/sbin/akam_onvif.sh.")
}

// ---------------------------------------------------------------- hostname

func TestHostname(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	r := e.run("", "hostname", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Set the camera's network name", "  The camera's network name is set to 'salon'. It applies after the next power cycle (unplug, wait 5 seconds, plug back).")
	b, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "hostname"))
	if strings.TrimSpace(string(b)) != "salon" {
		t.Errorf("hostname file: %q", b)
	}
	r = e.run("", "hostname", "Bad_Name")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: 'Bad_Name' is not a valid network name.")
	// the default is the camera's name; asked when there is none
	r = e.run("grenier\n", "hostname")
	expectRC(t, r, 0)
	contains(t, r.out, "'grenier'")
	r = e.run("", "--camera", "salon", "hostname")
	contains(t, r.out, "'salon'")
}

func TestHostnameOldFirmwareIsOnlyANote(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.NoHostnameHelper = true })
	r := e.run("", "hostname", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "  Note: this firmware does not have /usr/sbin/akam_hostname.sh yet, so the camera keeps its default network name.",
		"  Update to a newer Akam image, then run: ./akam hostname salon")
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "set salon") {
			t.Error("set was sent to a camera without the helper")
		}
	}
}

// ---------------------------------------------------------------- rtsp-auth

func TestRtspAuthSetStatusDisable(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	const pw = "correct horse battery"
	r := e.run("", "rtsp-auth")
	expectRC(t, r, 0)
	contains(t, r.out, "  RTSP password: disabled. The stream is open to anyone on your network.")

	// user, password, again; the password is on stdin, never in a command line, the log or the output
	r = e.run("viewer\n"+pw+"\n"+pw+"\n", "rtsp-auth", "set")
	expectRC(t, r, 0)
	contains(t, r.out, "  Sending the login over SSH ...", "  The video stream now asks for a user name and password (user: viewer).",
		"This password is now also stored on the camera (a file only root can read) so that ONVIF can verify it.",
		"  Stream URL: rtsp://viewer:PASSWORD@127.0.0.1:"+itoa(e.cam.RTSPPort())+"/")
	notContains(t, r.out+r.err+e.logText(), pw)
	for _, c := range e.cam.Commands() {
		notContains(t, c, pw, "correct")
	}
	if got := e.cam.Commands(); got[len(got)-1] != "/usr/sbin/akam_rtsp_auth.sh set-creds" {
		t.Errorf("commands: %v", got)
	}
	cred, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials"))
	if string(cred) != "user=viewer\npass="+pw+"\n" {
		t.Errorf("credentials on the camera: %q", cred)
	}
	r = e.run("", "rtsp-auth", "status")
	contains(t, r.out, "  RTSP password: enabled (user viewer, ONVIF-ready). Players must log in (Digest).")

	// disable: default No
	r = e.run("\n", "rtsp-auth", "disable")
	contains(t, r.out, "  Left as it was.")
	r = e.run("y\n", "rtsp-auth", "disable")
	expectRC(t, r, 0)
	contains(t, r.out, "RTSP authentication disabled: the stream is open again.", "  The video stream is open again (no password; the copy stored for ONVIF is removed too).")
	r = e.run("", "rtsp-auth", "status")
	contains(t, r.out, "disabled")
	expectRC(t, e.run("", "rtsp-auth", "bogus"), 2)
}

func TestRtspAuthSetRefusals(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	for _, c := range []struct{ in, msg string }{
		{"viewer\nshortpw\nshortpw\n", "the password is too short."},
		{"viewer\nlongenough1\nlongenough2\n", "the two passwords differ."},
		{"bad user\nlongenough1\nlongenough1\n", "invalid user name."},
		{"viewer\nlong\tenough\nlong\tenough\n", "the password contains a control character"},
		{"viewer\n" + strings.Repeat("x", 129) + "\n" + strings.Repeat("x", 129) + "\n", "the password is too long."},
	} {
		r := e.run(c.in, "rtsp-auth", "set")
		expectRC(t, r, 1)
		contains(t, r.err, c.msg)
	}
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "set-creds") {
			t.Fatal("a refused password reached the camera")
		}
	}
}

// ---------------------------------------------------------------- register, find-camera, state

func backupFixture(t *testing.T) string {
	t.Helper()
	d := filepath.Join(t.TempDir(), "backup")
	if err := os.MkdirAll(d, 0o700); err != nil {
		t.Fatal(err)
	}
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		f := filepath.Join(d, "mtdblock"+itoa(i)+".bin")
		size := int64(4096)
		if i == 7 {
			size = 5242880
		}
		if err := os.WriteFile(f, make([]byte, size), 0o600); err != nil {
			t.Fatal(err)
		}
		h, err := hashOf(f)
		if err != nil {
			t.Fatal(err)
		}
		sums.WriteString(h + "  mtdblock" + itoa(i) + ".bin\n")
	}
	if err := os.WriteFile(filepath.Join(d, "SHA256SUMS"), []byte(sums.String()), 0o600); err != nil {
		t.Fatal(err)
	}
	return d
}

func TestRegister(t *testing.T) {
	e := newEnv(t)
	bk := backupFixture(t)
	r := e.run("", "--camera", "salon", "--backup-dir", bk, "register")
	expectRC(t, r, 0)
	contains(t, r.out, "Register a camera that already runs Akam", "  SHA256SUMS verified.", "  Camera network name: akam-fake. APP partition (mtd7) md5: "+testfake.Mtd7MD5,
		"registered at 127.0.0.1 as already installed.")
	st, _ := os.ReadFile(filepath.Join(e.state, "cameras", "salon", "state"))
	for _, want := range []string{"camera_ip=127.0.0.1", "camera_kind=akam", "camera_registered=1", "flash_md5=" + testfake.Mtd7MD5, "step_find_camera_done=", "step_flash_done=", "step_verify_done=", "step_backup_done=", "backup_dir=" + bk, "tool=go"} {
		if !strings.Contains(string(st), want) {
			t.Errorf("state lacks %s:\n%s", want, st)
		}
	}
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "of=") {
			t.Errorf("register wrote something: %s", c)
		}
	}
	// a damaged backup stops it before any login
	if err := os.WriteFile(filepath.Join(bk, "mtdblock0.bin"), []byte("changed"), 0o600); err != nil {
		t.Fatal(err)
	}
	n := len(e.cam.Commands())
	r = e.run("", "--camera", "other", "--backup-dir", bk, "register")
	expectRC(t, r, 1)
	contains(t, r.err, "SHA256SUMS does not match the files in "+bk)
	if len(e.cam.Commands()) != n {
		t.Error("the camera was contacted although the backup is bad")
	}
}

func TestRegisterRefusesATelnetCameraAndNeedsAnAddress(t *testing.T) {
	e := newEnv(t)
	r := e.runRaw("", "--no-color", "--state-dir", e.state, "register")
	expectRC(t, r, 1)
	contains(t, r.err, "ERROR: register needs the camera's address.")
	// the camera's SSH port does not answer: not Akam
	t.Setenv("AKAM_PORT_SSH", freeClosedPort(t))
	r = e.run("", "register")
	expectRC(t, r, 1)
	contains(t, r.out, "  Open ports: ")
	contains(t, r.err, "does not look like a camera running Akam")
}

func seedDone(e *env, steps ...string) {
	var b strings.Builder
	for _, s := range steps {
		b.WriteString("step_" + strings.ReplaceAll(s, "-", "_") + "_done=2026-01-01T00:00:00Z\n")
	}
	e.writeState(b.String())
}

func TestFindCamera(t *testing.T) {
	e := newEnv(t)
	// blocked until the steps before it are done (same rule as the bash wizard)
	r := e.run("", "find-camera")
	expectRC(t, r, 1)
	contains(t, r.err, "step 'find-camera' needs these steps first: prepare-backup-sd")
	seedDone(e, "doctor", "prepare-backup-sd")

	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "Step 4/9 - Find the camera on your network", "  Probing 127.0.0.1 on ports ", "  Open ports: ", "this is a camera running Akam.", "  Camera found: it already runs Akam (127.0.0.1).")
	if e.readState("camera_kind") != "akam" || e.readState("camera_ip") != "127.0.0.1" || e.readState("camera_telnet") != "0" {
		t.Errorf("state: %s", e.logText())
	}

	// stock firmware: the maintenance telnet port is open
	ln := listen(t)
	t.Setenv("AKAM_PORT_TELNET", itoa(ln))
	t.Setenv("AKAM_PORT_SSH", freeClosedPort(t))
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "  Camera found: stock firmware with the toolkit shell on port "+itoa(ln)+" (127.0.0.1). The backup can start.")
	if e.readState("camera_kind") != "stock" || e.readState("camera_telnet") != "1" {
		t.Error("stock not recorded")
	}

	// only SSH answers (Akam still starting)
	t.Setenv("AKAM_PORT_TELNET", freeClosedPort(t))
	t.Setenv("AKAM_PORT_SSH", itoa(e.cam.SSHPort()))
	t.Setenv("AKAM_PORT_RTSP", freeClosedPort(t))
	t.Setenv("AKAM_PORT_CTL", freeClosedPort(t))
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 1)
	contains(t, r.err, "only SSH answers.")
}

func TestFindCameraTypedAddressAndNothingFound(t *testing.T) {
	e := newEnv(t)
	seedDone(e, "doctor", "prepare-backup-sd")
	// typed at the prompt; nothing is sent before it
	r := e.runRaw("127.0.0.1\n", "--no-color", "--state-dir", e.state, "find-camera")
	expectRC(t, r, 0)
	contains(t, r.out, "Find the address in your router's client list (the maker is shown as 'AltoBeam').")
	contains(t, r.err, "Camera IP address:")
	// an invalid answer
	r = e.runRaw("-bad\n", "--no-color", "--state-dir", e.state, "find-camera")
	expectRC(t, r, 1)
	contains(t, r.err, "'-bad' is not a valid address.")
	// nothing open on a closed address
	for _, p := range []string{"AKAM_PORT_SSH", "AKAM_PORT_RTSP", "AKAM_PORT_CTL", "AKAM_PORT_TELNET"} {
		t.Setenv(p, freeClosedPort(t))
	}
	r = e.runRaw("", "--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "find-camera")
	expectRC(t, r, 1)
	contains(t, r.out, "  Open ports: none")
	contains(t, r.err, "127.0.0.1 does not answer on any expected port.")
}

func TestStatusCamerasRename(t *testing.T) {
	e := newEnv(t)
	seedDone(e, "doctor")
	r := e.run("", "status")
	contains(t, r.out, "1. [done   ] doctor", "2. [todo   ] get-kit", "4. [blocked] find-camera", "Waiting for: prepare-backup-sd")
	r = e.run("", "cameras")
	contains(t, r.out, "Cameras (state in "+e.state+")", "  * default              1/9 steps done  last: doctor", "  Switch with: ./akam --camera NAME   (a new name starts a new camera)")
	r = e.run("", "rename-camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "  Camera 'default' is now called 'salon'. Backup and image folders already made keep their names.")
	if _, err := os.Stat(filepath.Join(e.state, "cameras", "salon", "state")); err != nil {
		t.Error("state did not move")
	}
	if _, err := os.Stat(filepath.Join(e.state, "state.before-rename.bak")); err != nil {
		t.Error("no untouched copy of the old state was kept")
	}
	r = e.run("", "cameras")
	contains(t, r.out, "  * salon")
	expectRC(t, e.run("", "rename-camera", "Bad Name"), 1)
	expectRC(t, e.run("", "rename-camera"), 2)
	// a name that exists
	e.run("", "--camera", "grenier", "status")
	r = e.run("", "--camera", "salon", "rename-camera", "grenier")
	if r.rc != 0 { // grenier has no state file yet: the rename is allowed, like bash
		contains(t, r.err, "already exists")
	}
}

func TestStrayCommandsAreRefusedPolitely(t *testing.T) {
	e := newEnv(t)
	for _, c := range []string{"flash", "backup", "image", "get-kit", "update", "restore", "wipe-sd", "prepare-flash-sd", "verify"} {
		r := e.run("", c)
		expectRC(t, r, 1)
		contains(t, r.err, "'"+c+"' is not in the native binary yet")
	}
	r := e.run("", "frobnicate")
	expectRC(t, r, 2)
	contains(t, r.err, "akam: unknown command 'frobnicate' (see ./akam help)")
	if len(e.cam.Commands()) != 0 {
		t.Errorf("commands reached the camera: %v", e.cam.Commands())
	}
}
