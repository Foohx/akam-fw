package audio

import (
	"context"
	"errors"
	"os/exec"
	"strings"
	"testing"
)

func finder(have ...string) func(string) (string, error) {
	return func(n string) (string, error) {
		for _, h := range have {
			if h == n {
				return "/fake/" + n, nil
			}
		}
		return "", exec.ErrNotFound
	}
}

// No test here starts ffmpeg, arecord or pactl: the commands are only built, never run.
func TestMicCommandChoice(t *testing.T) {
	// Linux without a sound server answer: arecord is preferred over ffmpeg alsa when ffmpeg+pactl are absent
	e := &Exec{LookPath: finder("arecord", "ffmpeg"), GOOS: "linux"}
	cmd, err := e.MicCommand(7)
	if err != nil || cmd[0] != "arecord" || strings.Join(cmd, " ") != "arecord -q -t raw -f S16_LE -r 8000 -c 1 -d 7" {
		t.Fatalf("%v %v", cmd, err)
	}
	cmd, _ = e.MicCommand(0)
	if strings.Contains(strings.Join(cmd, " "), "-d") {
		t.Errorf("a duration without a limit: %v", cmd)
	}
	// only ffmpeg: alsa
	e = &Exec{LookPath: finder("ffmpeg"), GOOS: "linux"}
	cmd, err = e.MicCommand(3)
	j := strings.Join(cmd, " ")
	if err != nil || !strings.HasPrefix(j, "ffmpeg -hide_banner -loglevel error -f alsa -i default -t 3 -af speechnorm=e=12.5:r=0.0001:l=1:p=0.6 -ar 8000 -ac 1 -f s16le -") {
		t.Fatalf("%v %v", cmd, err)
	}
	// macOS and Windows
	cmd, _ = (&Exec{LookPath: finder("ffmpeg"), GOOS: "darwin"}).MicCommand(0)
	if !strings.Contains(strings.Join(cmd, " "), "-f avfoundation -i :default") {
		t.Errorf("darwin: %v", cmd)
	}
	cmd, _ = (&Exec{LookPath: finder("ffmpeg"), GOOS: "windows"}).MicCommand(0)
	if !strings.Contains(strings.Join(cmd, " "), "-f dshow") {
		t.Errorf("windows: %v", cmd)
	}
	// nothing installed
	if _, err := (&Exec{LookPath: finder(), GOOS: "linux"}).MicCommand(0); !errors.Is(err, ErrNoTool) {
		t.Errorf("no tool: %v", err)
	}
	// the override of the wizard
	cmd, _ = (&Exec{Shell: "cat /dev/zero", LookPath: finder()}).MicCommand(5)
	if len(cmd) != 3 || cmd[0] != "sh" || cmd[2] != "cat /dev/zero" {
		t.Errorf("override: %v", cmd)
	}
	if c, err := (&Exec{LookPath: finder("ffmpeg")}).ConvertCommand("a b.mp3"); err != nil || c[len(c)-1] != "-" || c[5] != "a b.mp3" {
		t.Errorf("convert: %v %v", c, err)
	}
	if _, err := (&Exec{LookPath: finder()}).ConvertCommand("x"); !errors.Is(err, ErrNoTool) {
		t.Error("convert without ffmpeg")
	}
}

func TestStartRefusesAMissingProgram(t *testing.T) {
	if _, err := Start(context.Background(), []string{"/nonexistent/akam-capture"}); err == nil {
		t.Error("started a program that does not exist")
	}
}
