//go:build akamtest

package cli_test

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

func modeOf(t *testing.T, p string) os.FileMode {
	t.Helper()
	st, err := os.Stat(p)
	if err != nil {
		t.Fatal(err)
	}
	return st.Mode().Perm()
}

// The CONFIG partition holds the Wi-Fi and ONVIF passwords: the folder is 0700, every file 0600.
func TestBackupFolderAndFilesArePrivate(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("modes are checked on Unix; Windows uses an ACL")
	}
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState(stockSteps)
	r := w.run("y\ny\n", "backup")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "THIS BACKUP CONTAINS SECRETS")
	dirs := w.backupDirs(t)
	if len(dirs) != 1 {
		t.Fatalf("backup folders: %v", dirs)
	}
	if m := modeOf(t, dirs[0]); m != 0o700 {
		t.Errorf("folder mode %v", m)
	}
	es, _ := os.ReadDir(dirs[0])
	if len(es) != 11 {
		t.Errorf("%d files", len(es))
	}
	for _, e := range es {
		if m := modeOf(t, filepath.Join(dirs[0], e.Name())); m != 0o600 {
			t.Errorf("%s mode %v", e.Name(), m)
		}
	}
}

// An accepted --backup-dir (made by hand, 0755/0644) is closed, as the bash wizard does; the
// second copy is 0700/0600 as well, and an existing destination folder is left as it is.
func TestBackupImportAndSecondCopyAreRestricted(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("modes are checked on Unix")
	}
	w := newStockWorld(t, testfake.StockOptions{})
	dir := t.TempDir()
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		c := testfake.StockMtdContent(i)
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("mtdblock%d.bin", i)), c, 0o644)
		fmt.Fprintf(&sums, "%s  mtdblock%d.bin\n", shaOf(c), i)
	}
	os.WriteFile(filepath.Join(dir, "SHA256SUMS"), []byte(sums.String()), 0o644)
	if err := os.Chmod(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	w.writeState("")
	copyTo := filepath.Join(t.TempDir(), "second", "copy")
	r := w.run("y\ny\n", "backup", "--backup-dir", dir, "--backup-copy", copyTo)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "THIS BACKUP CONTAINS SECRETS", "  Backup folder accepted: ")
	for _, d := range []string{dir, copyTo} {
		if m := modeOf(t, d); m != 0o700 {
			t.Errorf("%s mode %v", d, m)
		}
		for _, n := range []string{"SHA256SUMS", "mtdblock0.bin", "mtdblock7.bin"} {
			if m := modeOf(t, filepath.Join(d, n)); m != 0o600 {
				t.Errorf("%s/%s mode %v", d, n, m)
			}
		}
	}
}

// The camera counts the RTSP password in bytes (8..128): 4 accented letters are 8 bytes and
// pass, 65 of them are 130 bytes and are refused although they are only 65 runes.
func TestRtspPasswordLengthIsInBytes(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	long := strings.Repeat("é", 65)
	r := e.run("viewer\n"+long+"\n"+long+"\n", "rtsp-auth", "set")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the password is too long.")
	short := "ééé" // 3 runes, 6 bytes
	r = e.run("viewer\n"+short+"\n"+short+"\n", "rtsp-auth", "set")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the password is too short.")
	ok := "éééé" // 4 runes, 8 bytes: allowed
	r = e.run("viewer\n"+ok+"\n"+ok+"\n", "rtsp-auth", "set")
	expectRC(t, r, 0)
}
