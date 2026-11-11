package state

import (
	"bufio"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestLockHelper is not a test: it is the second process of TestLockBetweenProcesses.
func TestLockHelper(t *testing.T) {
	dir := os.Getenv("AKAM_LOCK_HELPER_DIR")
	if dir == "" {
		t.Skip("helper process only")
	}
	l, ok, err := Acquire(dir)
	switch {
	case err != nil:
		os.Stdout.WriteString("error " + err.Error() + "\n")
	case ok:
		os.Stdout.WriteString("held\n")
		io.Copy(io.Discard, os.Stdin) // keep the lock until the parent closes stdin
		l.Release()
	default:
		os.Stdout.WriteString("busy\n")
	}
}

func startHelper(t *testing.T, dir string) (*exec.Cmd, io.WriteCloser, string) {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestLockHelper$")
	cmd.Env = append(os.Environ(), "AKAM_LOCK_HELPER_DIR="+dir)
	in, _ := cmd.StdinPipe()
	out, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	line, _ := bufio.NewReader(out).ReadString('\n')
	return cmd, in, strings.TrimSpace(line)
}

func TestLockBetweenProcesses(t *testing.T) {
	if runtime.GOOS == "plan9" {
		t.Skip("no lock on this OS")
	}
	dir := t.TempDir()
	// another process holds the lock: this one is refused
	cmd, in, got := startHelper(t, dir)
	if got != "held" {
		t.Fatalf("helper: %q", got)
	}
	if l, ok, err := Acquire(dir); ok || err != nil {
		if l != nil {
			l.Release()
		}
		t.Fatalf("second process got the lock: ok=%v err=%v", ok, err)
	}
	in.Close()
	_ = cmd.Wait()
	// and the other way round: this process holds it, the helper is refused
	l, ok, err := Acquire(dir)
	if !ok || err != nil {
		t.Fatalf("not released by the exit of the first process: ok=%v err=%v", ok, err)
	}
	cmd2, in2, got2 := startHelper(t, dir)
	if got2 != "busy" {
		t.Errorf("helper should be refused, got %q", got2)
	}
	in2.Close()
	_ = cmd2.Wait()
	l.Release()
}

func TestReservedCameraNames(t *testing.T) {
	for _, n := range []string{"con", "prn", "aux", "nul", "com1", "com9", "lpt1", "lpt9", "CON"} {
		if NameValid(n) {
			t.Errorf("new name %q accepted", n)
		}
	}
	for _, n := range []string{"salon", "com0", "console", "lpt10"} {
		if !NameValid(n) {
			t.Errorf("%q refused", n)
		}
	}
	// existing cameras keep their name (no rename, no disappearance)
	base := t.TempDir()
	dir := filepath.Join(base, "cameras", "nul")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "state"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if !CameraExists(base, "nul") || CameraExists(base, "con") {
		t.Error("CameraExists")
	}
	c, err := InitCameras(base, "", false)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, k := range c.Known() {
		found = found || k == "nul"
	}
	if !found {
		t.Errorf("existing camera hidden: %v", c.Known())
	}
	if err := c.Remember("nul"); err != nil || c.CurrentRead() != "nul" {
		t.Errorf("remembered name: %q %v", c.CurrentRead(), err)
	}
}

func TestDefaultDirEnv(t *testing.T) {
	t.Setenv("AKAM_STATE_DIR", "/x/y")
	if DefaultDir() != "/x/y" {
		t.Error("AKAM_STATE_DIR must win")
	}
}

func TestNameExistsRefusesReservedOnWindows(t *testing.T) {
	if nameExistsFor("windows", "con") || nameExistsFor("windows", "com1") || !nameExistsFor("windows", "salon") {
		t.Error("windows: reserved names must be refused, others kept")
	}
	if !nameExistsFor("linux", "con") || nameExistsFor("linux", "../x") || nameExistsFor("windows", "a/b") {
		t.Error("linux keeps con; separators and dots are never a name")
	}
}
