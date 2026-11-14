//go:build !windows

package fsutil

import "os"

func restrictDir(dir string) error { return nil } // MkdirAll(0700) already did it

func restrictFile(path string) error { return os.Chmod(path, 0o600) }
