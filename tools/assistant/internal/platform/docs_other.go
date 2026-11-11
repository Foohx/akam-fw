//go:build !unix && !windows

package platform

// DocumentsKnownFolder: unknown on this OS.
func DocumentsKnownFolder() string { return "" }

// FreeSpace: unknown on this OS.
func FreeSpace(dir string) int64 { return -1 }
