package cli

import (
	"strings"
	"testing"
)

// The safety rule of the camera: nothing the binary sends may reboot, power off, halt, kill the
// watchdog or write the flash. Every command line is built in remote.go; this test checks all of
// them, and checks that the guard itself catches what it must.
func TestCameraCommandsAreAllowListed(t *testing.T) {
	for _, c := range allRemoteCommands() {
		if why := forbiddenRemote(c); why != "" {
			t.Errorf("%s", why)
		}
		if strings.Contains(c, "\n") {
			t.Errorf("multi-line command: %q", c)
		}
	}
	for _, bad := range []string{
		"reboot", "/sbin/reboot -f", "poweroff", "halt", "shutdown -h now", "init 6", "telinit 0",
		"killall watchdog", "kill 1", "echo 1 > /dev/watchdog", "flash_tool -w x", "flashcp x /dev/mtd7",
		"dd if=x of=/dev/mtd7", "flash_eraseall /dev/mtd7", "nandwrite -p /dev/mtd7 x", "mtd_debug write /dev/mtd7 0 1 x", "echo b > /proc/sysrq-trigger", "sh -c 'reboot'", "true; reboot",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
	// reads of the partition are fine (register)
	if forbiddenRemote(remoteRegister()) != "" {
		t.Error("register's read-only dd was refused")
	}
}

func TestPictureNumber(t *testing.T) {
	for in, want := range map[string]string{"0": "0", "-0": "0", "007": "7", "-50": "-50", "50": "50", "-5": "-5"} {
		if got, ok := pictureNum(in); !ok || got != want {
			t.Errorf("%q -> %q %v, want %q", in, got, ok, want)
		}
	}
	for _, in := range []string{"", "-", "--5", "5-", "51", "-51", "1000", "x", "1.5", "+5", " 5"} {
		if _, ok := pictureNum(in); ok {
			t.Errorf("%q accepted", in)
		}
	}
}

func TestNamesAndHosts(t *testing.T) {
	for _, ok := range []string{"salon", "a", "cam-1", "192.168.1.50", "host.local"} {
		if !validHost(ok) {
			t.Errorf("host %q refused", ok)
		}
	}
	for _, bad := range []string{"", "-x", "a b", "a;b", "$(x)", "a/b"} {
		if validHost(bad) {
			t.Errorf("host %q accepted", bad)
		}
	}
	for _, ok := range []string{"salon", "akam-fake", "a1"} {
		if !hostnameValid(ok) {
			t.Errorf("hostname %q refused", ok)
		}
	}
	for _, bad := range []string{"", "-a", "a-", "Salon", "a_b", strings.Repeat("a", 33)} {
		if hostnameValid(bad) {
			t.Errorf("hostname %q accepted", bad)
		}
	}
	if !rtspUserValid("viewer") || rtspUserValid("bad user") || rtspUserValid("a:b") || rtspUserValid("") {
		t.Error("rtsp user rules")
	}
}

func TestParseArgs(t *testing.T) {
	o, err := ParseArgs([]string{"--dry-run", "picture", "contrast", "-20", "--ip", "1.2.3.4", "--no-color"})
	if err != nil || !o.DryRun || o.IP != "1.2.3.4" || strings.Join(o.Args, " ") != "picture contrast -20" {
		t.Fatalf("%+v %v", o, err)
	}
	if _, err := ParseArgs([]string{"-20"}); err == nil {
		t.Error("a negative number before the command is an unknown option")
	}
	if _, err := ParseArgs([]string{"--ip"}); err == nil || !strings.Contains(err.Error(), "--ip needs a value") {
		t.Errorf("%v", err)
	}
	if _, err := ParseArgs([]string{"--camera", ""}); err == nil {
		t.Error("an empty camera name")
	}
	if _, err := ParseArgs([]string{"--comp", "zip", "status"}); err == nil {
		t.Error("--comp zip")
	}
	if o, _ := ParseArgs([]string{"-h"}); len(o.Args) != 1 || o.Args[0] != "help" {
		t.Error("-h")
	}
}

func TestHelpMentionsEveryCommandOfThisPhase(t *testing.T) {
	for _, w := range []string{"find-camera", "hostname", "rtsp-auth", "led ", "audio ", "picture ", "debug ", "onvif ", "talk ", "say FILE", "status", "cameras", "rename-camera", "register", "version", "self-test", "forget-host-key", "--trust-new-host-key", "--plain", "--state-dir"} {
		if !strings.Contains(helpText, w) {
			t.Errorf("help lacks %q", w)
		}
	}
}
