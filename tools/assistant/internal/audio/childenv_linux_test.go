//go:build linux

package audio

import (
	"context"
	"io"
	"os/exec"
	"strings"
	"testing"
)

// The pactl probe is injectable: with ffmpeg and pactl present, a sound server that answers selects
// ffmpeg's pulse input, one that does not falls back to arecord. Nothing real is started.
func TestSoundServerProbeInjected(t *testing.T) {
	calls := 0
	e := &Exec{LookPath: finder("ffmpeg", "pactl", "arecord"), GOOS: "linux", SoundServerUp: func() bool { calls++; return true }}
	cmd, err := e.MicCommand(0)
	if err != nil || calls != 1 || cmd[0] != "ffmpeg" || !strings.Contains(strings.Join(cmd, " "), "-f pulse") {
		t.Fatalf("%v %v calls=%d", cmd, err, calls)
	}
	e.SoundServerUp = func() bool { return false }
	if cmd, _ = e.MicCommand(0); cmd[0] != "arecord" {
		t.Fatalf("%v", cmd)
	}
}

// A child started by Start does not see AKAM_*PASS* variables of the assistant (plain sh only).
func TestStartDropsSecretsFromChildEnv(t *testing.T) {
	if _, err := exec.LookPath("sh"); err != nil {
		t.Skip("skip: no sh")
	}
	t.Setenv("AKAM_RTSP_PASSWORD", "hunter2-secret")
	t.Setenv("AKAM_SD_PASS", "hunter3-secret")
	t.Setenv("AKAM_PLAIN", "1")
	p, err := Start(context.Background(), []string{"sh", "-c", `printf '[%s][%s][%s]' "${AKAM_RTSP_PASSWORD-}" "${AKAM_SD_PASS-}" "${AKAM_PLAIN-}"`})
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()
	out, _ := io.ReadAll(p)
	if string(out) != "[][][1]" {
		t.Errorf("child environment = %q", out)
	}
}
