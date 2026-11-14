// Package platform holds the small per-OS facts the assistant needs: where its state lives, which
// signals a long write must ignore, and the console set-up. The OS-specific files only collect
// facts and perform actions; every decision is made in this file and is testable on any host.
package platform

import (
	"os"
	"path/filepath"
	"runtime"
	"sync/atomic"
)

// StateDirFor is the default state directory for a given OS (specs/installer.md), from injected facts:
//
//	linux:   $XDG_STATE_HOME/akam, else <home>/.local/state/akam (shared with the bash wizard)
//	darwin:  <home>/Library/Application Support/Akam
//	windows: %LocalAppData%\Akam, else <home>\AppData\Local\Akam (never %AppData%: it roams)
//
// AKAM_STATE_DIR is handled by the caller (it wins on every OS).
func StateDirFor(goos string, getenv func(string) string, home string) string {
	switch goos {
	case "darwin":
		return filepath.Join(home, "Library", "Application Support", "Akam")
	case "windows":
		if d := getenv("LOCALAPPDATA"); d != "" {
			return filepath.Join(d, "Akam")
		}
		return filepath.Join(home, "AppData", "Local", "Akam")
	}
	if x := getenv("XDG_STATE_HOME"); x != "" {
		return filepath.Join(x, "akam")
	}
	return filepath.Join(home, ".local", "state", "akam")
}

// DefaultStateDir is StateDirFor for the running OS.
func DefaultStateDir() string {
	home, _ := os.UserHomeDir()
	return StateDirFor(runtime.GOOS, os.Getenv, home)
}

// SignalsToIgnore is the set a flash or an SD write ignores, and the set a long interactive
// operation listens to: Ctrl-C and TERM everywhere, plus hangup where the OS has it (not Windows,
// where a closed console arrives as TERM).
func SignalsToIgnore() []os.Signal { return signalSet() }

// ExitCodeFor is the shell-style exit code of a terminating signal: 130 for Ctrl-C, 143 for TERM,
// 129 for hangup.
func ExitCodeFor(sig os.Signal) int {
	switch {
	case sig == sigTerm:
		return 143
	case sigHup != nil && sig == sigHup:
		return 129
	}
	return 130
}

var vtFailed atomic.Bool

// Init does the one-time console set-up. It never fails: when the console refuses virtual-terminal
// mode the plain output simply has no colours (VTEnabled is false).
func Init() { vtFailed.Store(!EnableVT()) }

// VTEnabled reports whether coloured (ANSI) output may be used on this console.
func VTEnabled() bool { return !vtFailed.Load() }
