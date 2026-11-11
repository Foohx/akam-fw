//go:build windows

package platform

import "golang.org/x/sys/windows"

// OSVersion is the Windows version, from RtlGetVersion (not the shimmed GetVersionEx).
func OSVersion() string {
	v := windows.RtlGetVersion()
	return WindowsName(v.MajorVersion, v.MinorVersion, v.BuildNumber)
}
