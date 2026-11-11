//go:build windows

package fsutil

import (
	"github.com/Foohx/akam-fw/tools/assistant/internal/backuploc"
	"golang.org/x/sys/windows"
)

// protect sets a protected DACL (no inheritance from the parent) that gives full control to the
// current user, SYSTEM and Administrators only. dir adds the inherit flags so that files created
// inside the directory get the same access.
func protect(path string, dir bool) error {
	path = backuploc.LongPath(path) // x/sys calls do not get the long-path handling of package os
	tok := windows.GetCurrentProcessToken()
	user, err := tok.GetTokenUser()
	if err != nil {
		return err
	}
	flags := ""
	if dir {
		flags = "OICI"
	}
	sddl := "D:PAI(A;" + flags + ";FA;;;SY)(A;" + flags + ";FA;;;BA)(A;" + flags + ";FA;;;" + user.User.Sid.String() + ")"
	sd, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return err
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return err
	}
	return windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION, nil, nil, dacl, nil)
}

func restrictDir(dir string) error { return protect(dir, true) }

func restrictFile(path string) error { return protect(path, false) }
