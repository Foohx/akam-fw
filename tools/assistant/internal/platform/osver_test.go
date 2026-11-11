package platform

import (
	"strings"
	"testing"
)

func TestOSVersionParsers(t *testing.T) {
	if got := WindowsName(10, 0, 22631); got != "Windows 11 (build 22631)" {
		t.Error(got)
	}
	if got := WindowsName(10, 0, 19045); got != "Windows 10 (build 19045)" {
		t.Error(got)
	}
	if got := WindowsName(6, 1, 7601); !strings.Contains(got, "older than") {
		t.Error(got)
	}
	plist := "<dict>\n<key>ProductName</key>\n<string>macOS</string>\n<key>ProductVersion</key>\n\t<string>14.5</string>\n</dict>"
	if got := MacVersion(plist); got != "14.5" {
		t.Error(got)
	}
	if MacVersion("nothing") != "" {
		t.Error("no version expected")
	}
	if got := PrettyName("NAME=x\nPRETTY_NAME=\"Arch Linux\"\n"); got != "Arch Linux" {
		t.Error(got)
	}
	if got := PrettyName("PRETTY_NAME=\"A\x1b[31mB\"\n"); strings.ContainsRune(got, 0x1b) {
		t.Error(got)
	}
}
