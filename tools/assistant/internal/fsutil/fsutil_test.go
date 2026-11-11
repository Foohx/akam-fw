package fsutil

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

func TestReservedName(t *testing.T) {
	for _, n := range []string{"con", "CON", "Prn", "aux", "nul", "com1", "COM9", "lpt1", "lpt9", "nul.txt", "con.backup"} {
		if !ReservedName(n) {
			t.Errorf("%q should be reserved", n)
		}
	}
	for _, n := range []string{"", "salon", "com0", "com10", "lpt0", "console", "con-1", "my-nul", "a.con"} {
		if ReservedName(n) {
			t.Errorf("%q should not be reserved", n)
		}
	}
}

func TestRenameRetry(t *testing.T) {
	calls := 0
	flaky := func(a, b string) error {
		calls++
		if calls < 3 {
			return errors.New("sharing violation")
		}
		return nil
	}
	if err := renameRetry("a", "b", true, 10, time.Millisecond, flaky); err != nil || calls != 3 {
		t.Fatalf("err=%v calls=%d", err, calls)
	}
	calls = 0
	if err := renameRetry("a", "b", false, 10, time.Millisecond, flaky); err == nil || calls != 1 {
		t.Fatalf("without retry: err=%v calls=%d", err, calls)
	}
	calls = -100
	if err := renameRetry("a", "b", true, 4, time.Millisecond, flaky); err == nil || calls != -96 {
		t.Fatalf("exhausted: err=%v calls=%d", err, calls)
	}
}

func TestAtomicWriteAndPrivateMkdir(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "a", "b")
	if err := PrivateMkdir(dir); err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "f")
	if err := AtomicWrite(p, []byte("one"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := AtomicWrite(p, []byte("two"), 0o600); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(p)
	if string(b) != "two" {
		t.Fatalf("got %q", b)
	}
	ents, _ := os.ReadDir(dir)
	if len(ents) != 1 {
		t.Fatalf("temporary file left behind: %v", ents)
	}
	if runtime.GOOS != "windows" {
		st, _ := os.Stat(dir)
		if st.Mode().Perm() != 0o700 {
			t.Fatalf("dir mode %v", st.Mode().Perm())
		}
		st, _ = os.Stat(p)
		if st.Mode().Perm() != 0o600 {
			t.Fatalf("file mode %v", st.Mode().Perm())
		}
	}
}

func TestRenameNoRetryWhenSourceMissing(t *testing.T) {
	n := 0
	err := renameRetry("a", "b", true, 20, 0, func(a, b string) error { n++; return fs.ErrNotExist })
	if err == nil || n != 1 {
		t.Errorf("err=%v calls=%d, want 1 call", err, n)
	}
}
