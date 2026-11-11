package platform

import (
	"fmt"
	"regexp"
	"strings"
)

// WindowsName is the marketing name of a Windows build: 22000 and later is Windows 11, the other
// 10.0 builds are Windows 10. Older kernels are shown by their number.
func WindowsName(major, minor, build uint32) string {
	switch {
	case major == 10 && build >= 22000:
		return fmt.Sprintf("Windows 11 (build %d)", build)
	case major == 10:
		return fmt.Sprintf("Windows 10 (build %d)", build)
	}
	return fmt.Sprintf("Windows %d.%d (build %d, older than the Windows 10 that Akam supports)", major, minor, build)
}

var plistVersionRe = regexp.MustCompile(`<key>ProductVersion</key>\s*<string>([0-9][0-9.]{0,15})</string>`)

// MacVersion reads ProductVersion from the text of SystemVersion.plist ("" when absent).
func MacVersion(plist string) string {
	if m := plistVersionRe.FindStringSubmatch(plist); m != nil {
		return m[1]
	}
	return ""
}

// PrettyName reads PRETTY_NAME from the text of os-release ("" when absent); only printable ASCII
// is kept, at most 64 characters.
func PrettyName(osRelease string) string {
	for _, l := range strings.Split(osRelease, "\n") {
		if v, ok := strings.CutPrefix(strings.TrimRight(l, "\r"), "PRETTY_NAME="); ok {
			v = strings.Trim(v, `"'`)
			var b strings.Builder
			for _, r := range v {
				if r >= 0x20 && r < 0x7f && b.Len() < 64 {
					b.WriteRune(r)
				}
			}
			return b.String()
		}
	}
	return ""
}
