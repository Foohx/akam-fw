//go:build !windows

package platform

import (
	"os"
	"runtime"
)

// OSVersion is the version of the operating system: "macOS 14.5", the PRETTY_NAME of os-release
// on Linux, "" when unknown. AKAM_OS_RELEASE overrides the os-release path (tests).
func OSVersion() string {
	switch runtime.GOOS {
	case "darwin":
		b, err := os.ReadFile("/System/Library/CoreServices/SystemVersion.plist")
		if err != nil {
			return ""
		}
		if v := MacVersion(string(b)); v != "" {
			return "macOS " + v
		}
	case "linux":
		f := os.Getenv("AKAM_OS_RELEASE")
		if f == "" {
			f = "/etc/os-release"
		}
		if b, err := os.ReadFile(f); err == nil {
			return PrettyName(string(b))
		}
	}
	return ""
}
