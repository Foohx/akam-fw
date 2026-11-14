//go:build unix

package cli

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

// say refuses what is not a regular file (a FIFO would block, /dev/zero would never end) and a
// directory, without opening a pipe for reading, and reads at most maxSayFile+1 bytes of a file.
func TestReadSayFileRefusesNonRegular(t *testing.T) {
	dir := t.TempDir()
	fifo := filepath.Join(dir, "pipe.wav")
	if err := syscall.Mkfifo(fifo, 0o600); err != nil {
		t.Skip("no mkfifo here")
	}
	done := make(chan error, 1)
	go func() { _, err := readSayFile(fifo); done <- err }()
	select {
	case err := <-done:
		if !errors.Is(err, errSayNotRegular) {
			t.Errorf("fifo: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("readSayFile blocked on a FIFO")
	}
	if _, err := readSayFile(dir); !errors.Is(err, errSayNotRegular) {
		t.Errorf("directory: %v", err)
	}
	if _, err := readSayFile("/dev/null"); !errors.Is(err, errSayNotRegular) {
		t.Errorf("/dev/null: %v", err)
	}
	ok := filepath.Join(dir, "a.wav")
	if err := os.WriteFile(ok, []byte("RIFFxxxx"), 0o600); err != nil {
		t.Fatal(err)
	}
	if b, err := readSayFile(ok); err != nil || string(b) != "RIFFxxxx" {
		t.Errorf("regular file: %q %v", b, err)
	}
	if _, err := readSayFile(filepath.Join(dir, "missing")); err == nil || errors.Is(err, errSayNotRegular) {
		t.Errorf("missing file: %v", err)
	}
}
