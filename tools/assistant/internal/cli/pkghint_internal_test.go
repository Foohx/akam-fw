package cli

import (
	"strings"
	"testing"
)

func TestPkgHintTable(t *testing.T) {
	cases := []struct{ goos, distro, tool, want string }{
		{"linux", "arch", "ffmpeg", "sudo pacman -S --needed ffmpeg"},
		{"linux", "arch", "arecord", "sudo pacman -S --needed alsa-utils"},
		{"linux", "arch", "mksquashfs", "sudo pacman -S --needed squashfs-tools"},
		{"linux", "arch", "unsquashfs", "sudo pacman -S --needed squashfs-tools"},
		{"linux", "debian", "ffmpeg", "sudo apt-get install ffmpeg"},
		{"linux", "ubuntu", "arecord", "sudo apt-get install alsa-utils"},
		{"linux", "debian", "mksquashfs", "sudo apt-get install squashfs-tools"},
		{"linux", "ubuntu", "unsquashfs", "sudo apt-get install squashfs-tools"},
		{"linux", "unknown", "arecord", "install with your package manager: alsa-utils"},
		{"darwin", "unknown", "ffmpeg", "brew install ffmpeg"},
		{"darwin", "unknown", "mksquashfs", "brew install squashfs"},
		{"darwin", "unknown", "unsquashfs", "brew install squashfs"},
		{"windows", "unknown", "ffmpeg", "winget install Gyan.FFmpeg"},
	}
	for _, c := range cases {
		if got := pkgHintFor(c.goos, c.distro, c.tool); got != c.want {
			t.Errorf("%s/%s/%s: %q, want %q", c.goos, c.distro, c.tool, got, c.want)
		}
	}
	for _, tc := range [][2]string{{"windows", "mksquashfs"}, {"windows", "unsquashfs"}, {"windows", "arecord"}, {"darwin", "arecord"}} {
		got := pkgHintFor(tc[0], "unknown", tc[1])
		if strings.Contains(got, "winget install") || strings.Contains(got, "brew install") {
			t.Errorf("%v: fake install command: %q", tc, got)
		}
	}
	if got := pkgHintFor("windows", "unknown", "mksquashfs"); !strings.Contains(got, "--builder native") || !strings.Contains(got, "WSL") {
		t.Error(got)
	}
	if got := pkgHintFor("linux", "arch", "mksquashfs", "unsquashfs"); got != "sudo pacman -S --needed squashfs-tools" {
		t.Error(got)
	}
}
