package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// ---------------------------------------------------------------- host keys

func TestFirstContactAsksAndDefaultsToNo(t *testing.T) {
	e := newEnv(t)
	args := []string{"--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "--identity", e.key, "led"}
	pin := filepath.Join(e.state, "known_hosts")
	fp := e.cam.HostFingerprint()

	for _, in := range []string{"", "\n", "n\n", "q\n", "no\n", "yes please\n"} {
		r := e.runRaw(in, args...)
		expectRC(t, r, 1)
		contains(t, r.out, "  First SSH connection to this camera. Its host key fingerprint: "+fp)
		contains(t, r.err, "Trust this camera's host key? [y/N]")
		if _, err := os.Stat(pin); err == nil {
			t.Fatalf("input %q pinned the key", in)
		}
	}
	if len(e.cam.Commands()) != 0 {
		t.Error("a command was sent over an untrusted connection")
	}
	// no terminal: refuses, and tells how to accept it
	t.Setenv("AKAM_FORCE_TTY", "")
	r := e.runRaw("y\n", args...)
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "refusing to ask", "--trust-new-host-key "+fp)
	t.Setenv("AKAM_FORCE_TTY", "1")

	r = e.runRaw("y\n", args...)
	expectRC(t, r, 0)
	b, _ := os.ReadFile(pin)
	if !strings.HasPrefix(string(b), "127.0.0.1 ssh-ed25519 ") {
		t.Errorf("pin: %q", b)
	}
	// from now on silent, even without a terminal
	t.Setenv("AKAM_FORCE_TTY", "")
	expectRC(t, e.runRaw("", args...), 0)
}

func TestChangedHostKeyIsRefusedAndForgetIsExplicit(t *testing.T) {
	e := newEnv(t)
	args := []string{"--no-color", "--state-dir", e.state, "--ip", "127.0.0.1", "--identity", e.key, "--trust-new-host-key", e.cam.HostFingerprint(), "led"}
	expectRC(t, e.runRaw("", args...), 0)
	e.cam.RotateHostKey()
	n := len(e.cam.Commands())
	args[8] = e.cam.HostFingerprint() // the flag must not override a pin
	r := e.runRaw("y\n", args...)
	expectRC(t, r, 1)
	contains(t, r.err, "REMOTE HOST IDENTIFICATION HAS CHANGED for 127.0.0.1")
	contains(t, r.out, "forget-host-key 127.0.0.1")
	if len(e.cam.Commands()) != n {
		t.Error("a command was sent to a camera whose key changed")
	}
	// forget: default No, then yes
	fa := []string{"--no-color", "--state-dir", e.state, "forget-host-key", "127.0.0.1"}
	r = e.runRaw("\n", fa...)
	expectRC(t, r, 0)
	contains(t, r.out, "  Left as it was.")
	expectRC(t, e.runRaw("n\n", args...), 1)
	r = e.runRaw("y\n", fa...)
	expectRC(t, r, 0)
	contains(t, r.out, "  Done. The next connection to 127.0.0.1 shows its fingerprint and asks again.")
	if !strings.Contains(e.logText(), "FORGET host key of 127.0.0.1") {
		t.Error("forgetting is not logged")
	}
	expectRC(t, e.runRaw("", args...), 0) // trusted by flag again
	t.Setenv("AKAM_FORCE_TTY", "")
	r = e.runRaw("y\n", fa...)
	expectRC(t, r, 1)
	contains(t, r.err, "needs an interactive terminal")
	expectRC(t, e.runRaw("", "--state-dir", e.state, "forget-host-key"), 2)
}

func TestUserKnownHostsIsNeverWritten(t *testing.T) {
	e := newEnv(t)
	user := filepath.Join(e.home, ".ssh", "known_hosts")
	if err := os.MkdirAll(filepath.Dir(user), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(user, []byte("# mine\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	expectRC(t, e.run("", "led"), 0)
	b, _ := os.ReadFile(user)
	if string(b) != "# mine\n" {
		t.Errorf("the user's known_hosts changed: %q", b)
	}
}

// ---------------------------------------------------------------- default No

func TestEveryQuestionDefaultsToNo(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	// a camera with a stream password, so that every guarded action is reachable
	expectRC(t, e.run("viewer\nlongenough1\nlongenough1\n", "rtsp-auth", "set"), 0)
	cases := []struct {
		name string
		args []string
		bad  string // a command that must never reach the camera
	}{
		{"debug on", []string{"debug", "on"}, "akam_debug.sh on"},
		{"onvif enable", []string{"onvif", "enable"}, "akam_onvif.sh enable"},
		{"rtsp-auth disable", []string{"rtsp-auth", "disable"}, "akam_rtsp_auth.sh disable"},
	}
	for _, c := range cases {
		for _, in := range []string{"", "\n", "n\n", "N\n", "q\n", "\x1b\n", "maybe\n", "no\n"} {
			r := e.run(in, c.args...)
			expectRC(t, r, 0)
			contains(t, r.out, "Left as it was")
		}
		for _, cmd := range e.cam.Commands() {
			if strings.Contains(cmd, c.bad) {
				t.Errorf("%s: %q was sent without a yes", c.name, cmd)
			}
		}
		// no terminal at all: refused (exit 1), never asked, never sent
		t.Setenv("AKAM_FORCE_TTY", "")
		r := e.run("y\n", c.args...)
		expectRC(t, r, 1)
		contains(t, r.err, "needs an interactive terminal and refuses to run without one.")
		t.Setenv("AKAM_FORCE_TTY", "1")
		for _, cmd := range e.cam.Commands() {
			if strings.Contains(cmd, c.bad) {
				t.Errorf("%s: %q was sent without a terminal", c.name, cmd)
			}
		}
	}
	// only an explicit yes goes through
	expectRC(t, e.run("y\n", "rtsp-auth", "disable"), 0)
	found := false
	for _, cmd := range e.cam.Commands() {
		if strings.Contains(cmd, "akam_rtsp_auth.sh disable") {
			found = true
		}
	}
	if !found {
		t.Error("yes did not run the command")
	}
}

// ---------------------------------------------------------------- secrets and logs

func TestNoSecretInStateLogOrCommands(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	expectRC(t, e.run("", "talk", "test"), 0)
	expectRC(t, e.run("", "audio", "volume", "10"), 0)
	secrets := []string{talkPass}
	var all strings.Builder
	filepath.Walk(e.state, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() {
			b, _ := os.ReadFile(p)
			all.Write(b)
		}
		return nil
	})
	for _, c := range e.cam.Commands() {
		all.WriteString(c + "\n")
	}
	for _, s := range secrets {
		if strings.Contains(all.String(), s) {
			t.Errorf("the password %q is in the state, a log or a command", s)
		}
	}
	// the state file refuses a secret outright
	st, _ := os.ReadFile(filepath.Join(e.state, "state"))
	if strings.Contains(string(st), "pass") {
		t.Errorf("state: %s", st)
	}
	// the log redacts assignments typed in free text
	if strings.Contains(e.logText(), "password=hunter2") {
		t.Error("log leaks a password assignment")
	}
}
