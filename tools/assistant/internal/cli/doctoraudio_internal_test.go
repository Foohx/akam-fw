package cli

import (
	"os"
	"runtime"
	"strings"
	"testing"
)

// The Audio line says what is built in and, without a microphone source, why. No host access:
// test builds have no built-in backend and the fake LookPath finds nothing.
func TestDoctorAudioLine(t *testing.T) {
	t.Setenv("AKAM_TALK_CAPTURE_CMD", "")
	a := &App{}
	a.Hooks.LookPath = func(string) (string, error) { return "", os.ErrNotExist }
	got := a.doctorAudio()
	if !strings.Contains(got, "WAV, MP3, FLAC and Ogg Vorbis built in") || !strings.Contains(got, "microphone: none found") {
		t.Error(got)
	}
	t.Setenv("AKAM_TALK_CAPTURE_CMD", "cat /dev/null")
	if got := a.doctorAudio(); !strings.Contains(got, "microphone: AKAM_TALK_CAPTURE_CMD") {
		t.Error(got)
	}
	t.Setenv("AKAM_TALK_CAPTURE_CMD", "")
	a.Hooks.LookPath = func(n string) (string, error) {
		if n == "ffmpeg" {
			return "/x/ffmpeg", nil
		}
		return "", os.ErrNotExist
	}
	if got := a.doctorAudio(); runtime.GOOS != "linux" && !strings.Contains(got, "microphone: ffmpeg") {
		t.Error(got)
	}
}
