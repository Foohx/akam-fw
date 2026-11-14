package cli

import (
	"runtime"
	"strings"
)

// pkgNames maps a tool to its package per package manager. A tool missing from a manager's map
// has no sensible package there (see pkgNone).
var pkgNames = map[string]map[string]string{
	"pacman": {"ffmpeg": "ffmpeg", "arecord": "alsa-utils", "mksquashfs": "squashfs-tools", "unsquashfs": "squashfs-tools"},
	"apt":    {"ffmpeg": "ffmpeg", "arecord": "alsa-utils", "mksquashfs": "squashfs-tools", "unsquashfs": "squashfs-tools"},
	"brew":   {"ffmpeg": "ffmpeg", "mksquashfs": "squashfs", "unsquashfs": "squashfs"},
	"winget": {"ffmpeg": "Gyan.FFmpeg"},
}

// pkgNone is the advice for a tool that has no package on an OS (never a fake install command).
func pkgNone(goos, tool string) string {
	switch tool {
	case "mksquashfs", "unsquashfs":
		if goos == "windows" {
			return tool + " has no native Windows package: use `--builder native` (experimental, needs none) or run the assistant inside WSL"
		}
		return tool + " is not packaged here: use `--builder native` (experimental, needs none) or build squashfs-tools from source"
	case "arecord":
		return "arecord is part of ALSA and exists on Linux only: use ffmpeg for the microphone"
	}
	return "no known package for " + tool + " on this system: install it manually and put it on the PATH"
}

// pkgHintFor is the install advice for the given tools on goos / the Linux distribution id.
func pkgHintFor(goos, distro string, tools ...string) string {
	mgr, cmd := "", ""
	switch goos {
	case "darwin":
		mgr, cmd = "brew", "brew install "
	case "windows":
		mgr, cmd = "winget", "winget install "
	default:
		switch distro {
		case "arch":
			mgr, cmd = "pacman", "sudo pacman -S --needed "
		case "debian", "ubuntu":
			mgr, cmd = "apt", "sudo apt-get install "
		}
	}
	if mgr == "" {
		var names []string
		for _, t := range tools {
			n := pkgNames["apt"][t]
			if n == "" {
				n = t
			}
			names = append(names, n)
		}
		return "install with your package manager: " + strings.Join(uniq(names), " ")
	}
	var pk, none []string
	for _, t := range tools {
		if p, ok := pkgNames[mgr][t]; ok {
			pk = append(pk, p)
		} else {
			none = append(none, pkgNone(goos, t))
		}
	}
	var parts []string
	if len(pk) > 0 {
		parts = append(parts, cmd+strings.Join(uniq(pk), " "))
	}
	return strings.Join(append(parts, none...), "; ")
}

func uniq(in []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, s := range in {
		if s != "" && !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	return out
}

// pkgHint is host_pkg_install_hint.
func pkgHint(_ *App, pkgs ...string) string { return pkgHintFor(runtime.GOOS, hostOS(), pkgs...) }
