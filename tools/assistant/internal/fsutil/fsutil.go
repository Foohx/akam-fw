// Package fsutil has the file helpers that differ by OS: atomic replace (Windows refuses to rename
// over a file that an antivirus or indexer holds open for a moment), private directories and files
// (0700/0600 on Unix, a protected ACL on Windows) and the Windows reserved names.
package fsutil

import (
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"
)

var warnMu sync.Mutex
var warnings []string

func warn(s string) {
	warnMu.Lock()
	defer warnMu.Unlock()
	for _, w := range warnings {
		if w == s {
			return
		}
	}
	warnings = append(warnings, s)
}

// Warnings lists the privacy settings that could not be applied (a FAT/exFAT or network path cannot
// hold an ACL). They are not errors: doctor and the log show them.
func Warnings() []string {
	warnMu.Lock()
	defer warnMu.Unlock()
	return append([]string(nil), warnings...)
}

// PrivateMkdir creates dir (and parents) readable by the current user only. A failure to restrict
// the access is recorded in Warnings, never returned.
func PrivateMkdir(dir string) error {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	if err := restrictDir(dir); err != nil {
		warn(fmt.Sprintf("could not restrict access to %s: %v", dir, err))
	}
	return nil
}

// PrivateFile restricts an existing file to the current user (0600 on Unix, a protected ACL on
// Windows). A failure is recorded in Warnings, never returned.
func PrivateFile(path string) {
	if err := restrictFile(path); err != nil {
		warn(fmt.Sprintf("could not restrict access to %s: %v", path, err))
	}
}

// reserved is the set of names Windows refuses for a file or folder, with or without an extension.
var reserved = func() map[string]bool {
	m := map[string]bool{"con": true, "prn": true, "aux": true, "nul": true}
	for i := '1'; i <= '9'; i++ {
		m["com"+string(i)] = true
		m["lpt"+string(i)] = true
	}
	return m
}()

// ReservedName reports whether name is a Windows device name (con prn aux nul com1-9 lpt1-9),
// case-insensitive, with or without an extension ("nul.txt").
func ReservedName(name string) bool {
	base := name
	if i := strings.IndexByte(base, '.'); i >= 0 {
		base = base[:i]
	}
	return reserved[strings.ToLower(strings.TrimRight(base, " "))]
}

// AtomicWrite writes data to a temporary file beside path, flushes it, and renames it over path.
func AtomicWrite(path string, data []byte, perm os.FileMode) error {
	tmp := fmt.Sprintf("%s.tmp.%d", path, os.Getpid())
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, perm)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Chmod(tmp, perm); err != nil && runtime.GOOS != "windows" {
		os.Remove(tmp)
		return err
	}
	if err := Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// Rename is os.Rename; on Windows it retries for about a second, because a sharing violation from
// a scanner that holds the destination open is transient.
func Rename(oldpath, newpath string) error {
	return renameRetry(oldpath, newpath, runtime.GOOS == "windows", 20, 50*time.Millisecond, os.Rename)
}

// renameRetry is Rename with the OS facts and the rename function injected (for tests).
func renameRetry(oldpath, newpath string, retry bool, tries int, wait time.Duration, rename func(a, b string) error) error {
	err := rename(oldpath, newpath)
	for i := 1; err != nil && retry && i < tries; i++ {
		time.Sleep(wait)
		err = rename(oldpath, newpath)
	}
	return err
}
