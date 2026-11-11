package cli

import (
	"bufio"
	"os"
	"runtime"
	"strings"
	"time"

	"golang.org/x/term"
)

func isTerminal(f *os.File) bool { return term.IsTerminal(int(f.Fd())) }

func secondsDuration(n int) time.Duration { return time.Duration(n) * time.Second }

// hostOS is host_os: arch | debian | ubuntu | unknown, from os-release (AKAM_OS_RELEASE overrides).
func hostOS() string {
	f := os.Getenv("AKAM_OS_RELEASE")
	if f == "" {
		f = "/etc/os-release"
	}
	fh, err := os.Open(f)
	if err != nil {
		return "unknown"
	}
	defer fh.Close()
	id, like := "", ""
	sc := bufio.NewScanner(fh)
	for sc.Scan() {
		l := sc.Text()
		switch {
		case strings.HasPrefix(l, "ID="):
			id = strings.ReplaceAll(strings.TrimPrefix(l, "ID="), `"`, "")
		case strings.HasPrefix(l, "ID_LIKE="):
			like = strings.ReplaceAll(strings.TrimPrefix(l, "ID_LIKE="), `"`, "")
		}
	}
	switch id {
	case "arch", "archarm", "manjaro", "endeavouros":
		return "arch"
	case "ubuntu":
		return "ubuntu"
	case "debian":
		return "debian"
	}
	for _, w := range strings.Fields(like) {
		switch w {
		case "arch", "ubuntu", "debian":
			return w
		}
	}
	return "unknown"
}

// pkgHint is host_pkg_install_hint.
func pkgHint(_ *App, pkgs ...string) string {
	p := strings.Join(pkgs, " ")
	switch runtime.GOOS {
	case "darwin":
		return "brew install " + p
	case "windows":
		return "winget install " + p + " (or download it and put it on the PATH)"
	}
	switch hostOS() {
	case "arch":
		return "sudo pacman -S --needed " + p
	case "debian", "ubuntu":
		return "sudo apt-get install " + p
	}
	return "install with your package manager: " + p
}
