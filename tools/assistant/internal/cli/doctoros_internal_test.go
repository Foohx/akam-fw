package cli

import (
	"strings"
	"testing"
)

func TestDoctorTextPerOS(t *testing.T) {
	if got := doctorHost("windows", "amd64", "unknown", "Windows 11 (build 22631)"); got != "Host: Windows 11 (build 22631) x86_64" {
		t.Error(got)
	}
	if got := doctorHost("darwin", "arm64", "unknown", "macOS 14.5"); got != "Host: macOS 14.5 aarch64" {
		t.Error(got)
	}
	if got := doctorHost("linux", "amd64", "arch", ""); got != "Host: arch x86_64" {
		t.Error(got)
	}
	for _, goos := range []string{"linux", "darwin", "windows"} {
		for _, o := range doctorOptional(goos) {
			if o.tool == "xz" {
				t.Errorf("%s: xz is built in and must not be listed", goos)
			}
		}
	}
	has := func(goos, tool string) bool {
		for _, o := range doctorOptional(goos) {
			if o.tool == tool {
				return true
			}
		}
		return false
	}
	if has("windows", "arecord") || !has("linux", "arecord") || !has("windows", "ffmpeg") {
		t.Error("optional tools per OS")
	}
	none := func(string) bool { return false }
	if s := doctorElevation("windows", none); !strings.Contains(s, "UAC") {
		t.Error(s)
	}
	if s := doctorElevation("darwin", none); !strings.Contains(s, "macOS password") {
		t.Error(s)
	}
	if s := doctorElevation("linux", func(n string) bool { return n == "sudo" }); !strings.Contains(s, "sudo") {
		t.Error(s)
	}
}
