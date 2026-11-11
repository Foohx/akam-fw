package cli_test

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The binary and the bash wizard share the state directory: each reads what the other wrote and
// ignores keys it does not know. These tests run the real ./akam (read and write of files only,
// no network, no camera) next to the Go program.

func bashAkam(t *testing.T, args ...string) (string, int) {
	t.Helper()
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("skip: bash is not installed")
	}
	script := filepath.Join("..", "..", "..", "..", "akam")
	if _, err := os.Stat(script); err != nil {
		t.Skip("skip: the bash wizard is not in this checkout")
	}
	cmd := exec.Command(bash, append([]string{script}, args...)...)
	cmd.Env = append(os.Environ(), "AKAM_STATE_DIR=", "NO_COLOR=1", "AKAM_FORCE_TTY=")
	cmd.Stdin = nil
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	rc := 0
	if err := cmd.Run(); err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			rc = ee.ExitCode()
		} else {
			t.Fatal(err)
		}
	}
	return out.String(), rc
}

func TestStateRoundTripWithBash(t *testing.T) {
	e := newEnv(t)
	// bash writes first: its doctor step marks itself done in the state file
	out, rc := bashAkam(t, "--no-color", "--state-dir", e.state, "doctor")
	if rc != 0 {
		t.Skipf("skip: the bash doctor does not pass on this host (%d): %s", rc, firstLine(out))
	}
	r := e.run("", "status")
	expectRC(t, r, 0)
	contains(t, r.out, "1. [done   ] doctor")
	// Go writes: it records a registered camera and adds its own keys
	bk := backupFixture(t)
	expectRC(t, e.run("", "--backup-dir", bk, "register"), 0)
	st, _ := os.ReadFile(filepath.Join(e.state, "state"))
	if !strings.Contains(string(st), "tool=go") || !strings.Contains(string(st), "step_doctor_done=") {
		t.Fatalf("state after Go:\n%s", st)
	}
	if _, err := os.Stat(filepath.Join(e.state, "state.bak-before-binary")); err != nil {
		t.Error("the state of the bash wizard was not copied before the binary used it")
	}
	// bash reads what Go wrote, and does not choke on the keys it does not know
	out, rc = bashAkam(t, "--no-color", "--state-dir", e.state, "status")
	if rc != 0 {
		t.Fatalf("bash status failed (%d):\n%s", rc, out)
	}
	for _, want := range []string{"1. [done   ] doctor", "4. [done   ] find-camera", "5. [done   ] backup", "8. [done   ] flash", "9. [done   ] verify"} {
		if !strings.Contains(out, want) {
			t.Errorf("bash does not see %q:\n%s", want, out)
		}
	}
	out, _ = bashAkam(t, "--no-color", "--state-dir", e.state, "cameras")
	contains(t, out, "default")
	// and Go reads bash's table the same way
	g := e.run("", "status")
	b, _ := bashAkam(t, "--no-color", "--state-dir", e.state, "status")
	if strings.ReplaceAll(g.out, "\r", "") != strings.ReplaceAll(b, "\r", "") {
		t.Errorf("status differs after the round trip\n--- go ---\n%s\n--- bash ---\n%s", g.out, b)
	}
}

func TestCameraLayoutIsTheSame(t *testing.T) {
	e := newEnv(t)
	expectRC(t, e.run("", "--camera", "salon", "--backup-dir", backupFixture(t), "register"), 0)
	// the named camera lives in cameras/salon, as in cameras.sh, and current_camera names it
	if _, err := os.Stat(filepath.Join(e.state, "cameras", "salon", "state")); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(e.state, "current_camera"))
	if string(b) != "salon\n" {
		t.Errorf("current_camera: %q", b)
	}
	out, rc := bashAkam(t, "--no-color", "--state-dir", e.state, "cameras")
	if rc != 0 {
		t.Fatalf("bash cameras: %s", out)
	}
	contains(t, out, "* salon", "4/9 steps done  127.0.0.1")
	// file modes: private
	for _, p := range []string{filepath.Join(e.state, "cameras", "salon", "state"), filepath.Join(e.state, "cameras", "salon", "akam.log")} {
		st, err := os.Stat(p)
		if err != nil || st.Mode().Perm() != 0o600 {
			t.Errorf("%s: %v %v", p, err, st)
		}
	}
	d, _ := os.Stat(e.state)
	if d.Mode().Perm() != 0o700 {
		t.Errorf("state directory mode %v", d.Mode().Perm())
	}
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}
