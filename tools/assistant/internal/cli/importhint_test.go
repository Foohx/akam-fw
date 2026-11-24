package cli_test

import (
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The hint after an import names only the command of the detected model, and quotes the folder (the default
// "Akam backups" has a space).
func TestImportHintIsOneCommandWithAQuotedPath(t *testing.T) {
	for _, c := range []struct {
		name, cmd, other string
		s                synth
	}{
		{"1080p", "backup", "update", make1080(11)},
		{"2K", "update", "backup", make2K(12)},
	} {
		e := newEnv(t)
		src := filepath.Join(t.TempDir(), "stock-20260202")
		c.s.write(t, src)
		if c.name == "1080p" {
			put(t, filepath.Join(src, "SHA256SUMS"), c.s.sha256sums())
		} else {
			put(t, filepath.Join(src, "MD5SUMS"), c.s.md5sums())
		}
		r := e.imp("--from", src, "--camera", "porch")
		expectRC(t, r, 0)
		dest := importedDir(e, "porch", "20260202")
		q := "'"
		if runtime.GOOS == "windows" {
			q = `"`
		}
		contains(t, r.out+r.err, "akam "+c.cmd+" --backup-dir "+q+dest+q)
		notContains(t, r.out+r.err, "akam "+c.other+" --backup-dir", "(1080p)", "(2K)")
		if !strings.Contains(dest, " ") {
			t.Fatal("the test folder must contain a space")
		}
	}
}
