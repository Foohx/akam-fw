package cli_test

import (
	"path/filepath"
	"testing"
)

// The home menu's "Back up" of a 2K: shows the folder, changes it, checks a backup folder (it is not refused as
// a 1080p-only flow).
func TestBackupMenuOf2K(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=akam\ncamera_model=" + rotModel + "\n")
	chosen := t.TempDir()
	good := filepath.Join(t.TempDir(), "good")
	s := make2K(11)
	s.write(t, good)
	put(t, filepath.Join(good, "SHA256SUMS"), s.sha256sums())
	// home 4 > 2 (choose where) > 2 (another folder) > folder > 1 (use it) ; home 4 > 3 (check a folder) > folder ; quit
	r := e.run("4\n2\n2\n"+chosen+"\n1\n4\n3\n"+good+"\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Back up - default (2K)", "Backup folder: ", "The backups will be kept under "+filepath.Join(chosen, "Akam backups"),
		"6 partitions present, sizes and SHA256SUMS match")
	notContains(t, r.out, "not yet supported")
	if e.readState("backup_root") != chosen {
		t.Errorf("backup_root = %q", e.readState("backup_root"))
	}
}
