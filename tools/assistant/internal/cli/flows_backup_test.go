package cli_test

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// backup menu: 1 back up, 2 restore, 3 check a folder, 4 choose where backups are kept, 5 back.
func akamState(e *env, extra string) {
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=akam\n" + extra)
}

func TestBackupLocationProposesAndChangesTheFolder(t *testing.T) {
	e := newEnv(t)
	akamState(e, "")
	day := time.Now().Format("20060102")
	suggested := filepath.Join(e.home, "Akam backups", "camera", day)
	chosen := filepath.Join(t.TempDir(), "mybackups")
	os.MkdirAll(chosen, 0o700)
	// home 4 > 4 (location) > 2 (another folder) > folder > 1 (use it) > 5 (back) > 9
	r := e.run("4\n4\n2\n"+chosen+"\n1\n5\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Where should the backup of the camera be kept?", "your Wi-Fi password in clear text",
		"Suggested:  "+suggested, "Free space: ", "The backups will be kept under "+filepath.Join(chosen, "Akam backups"),
		"Suggested:  "+filepath.Join(chosen, "Akam backups", "camera", day))
	if e.readState("backup_root") != chosen || e.readState("backup_root_ok") != "1" {
		t.Errorf("state: root=%q ok=%q", e.readState("backup_root"), e.readState("backup_root_ok"))
	}
	// nothing was created yet: a location is only a proposal
	if _, err := os.Stat(filepath.Join(chosen, "Akam backups")); err == nil {
		t.Error("the folder was created before any backup")
	}
	// 'default' goes back to the suggestion
	r = e.run("4\n4\n2\ndefault\n1\n5\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	if e.readState("backup_root") != "" {
		t.Errorf("root kept: %q", e.readState("backup_root"))
	}
}

func TestBackupLocationWarnsAboutCloudFoldersAndDefaultsToNo(t *testing.T) {
	e := newEnv(t)
	akamState(e, "")
	cloud := filepath.Join(t.TempDir(), "Dropbox", "stuff")
	os.MkdirAll(cloud, 0o700)
	r := e.run("4\n4\n2\n"+cloud+"\n\n3\n10\n", "--camera", "default") // another folder > Enter = No > cancel
	expectRC(t, r, 0)
	contains(t, r.out, "looks like part of")
	contains(t, r.err, "Use it anyway? [y/N]")
	if e.readState("backup_root") != "" {
		t.Error("a cloud folder was taken without a yes")
	}
	// a missing folder and a folder inside a git work tree are refused
	r = e.run("4\n4\n2\n/no/such/folder\n3\n10\n", "--camera", "default")
	contains(t, r.out, "is not an existing folder.")
}

// The SSH backup of an Akam camera goes to the folder the user chose, private, with SHA256SUMS; the
// recorded folder is what the update gate re-verifies.
func TestBackupSSHToTheChosenRoot(t *testing.T) {
	e := newEnv(t)
	root := t.TempDir()
	akamState(e, "backup_root="+root+"\nbackup_root_ok=1\n")
	r := e.run("4\n1\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Route: SSH (this camera runs Akam).", "it cannot recreate the original (stock) firmware.", "Backup saved and verified")
	d := filepath.Join(root, "Akam backups", "camera", time.Now().Format("20060102"))
	for _, f := range []string{"mtdblock0.bin", "mtdblock7.bin", "SHA256SUMS", "camera_info.txt"} {
		st, err := os.Stat(filepath.Join(d, f))
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		if runtime.GOOS != "windows" && st.Mode().Perm() != 0o600 {
			t.Errorf("%s mode %v", f, st.Mode().Perm())
		}
	}
	if runtime.GOOS != "windows" {
		for _, p := range []string{filepath.Join(root, "Akam backups"), filepath.Join(root, "Akam backups", "camera"), d} {
			if st, _ := os.Stat(p); st == nil || st.Mode().Perm() != 0o700 {
				t.Errorf("%s is not private", p)
			}
		}
	}
	if e.readState("ssh_backup_dir") != d {
		t.Errorf("ssh_backup_dir=%q", e.readState("ssh_backup_dir"))
	}
	// the stock-backup state is not touched by an SSH backup
	if e.readState("backup_dir") != "" || e.readState("step_backup_done") != "" {
		t.Error("the stock backup state was touched")
	}
}

// Route choice: a stock camera takes the card route (and says so); location first on the first backup.
func TestBackupRouteChoice(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nbackup_root_ok=1\n")
	r := e.run("4\n1\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Route: the maintenance telnet and the backup card (stock firmware).")
	notContains(t, r.out, "Route: SSH")
	// first backup asks for the location first; cancelling does nothing
	e = newEnv(t)
	akamState(e, "")
	r = e.run("4\n1\n3\n10\n", "--camera", "default") // location screen > Cancel
	expectRC(t, r, 0)
	contains(t, r.out, "Where should the backup of the camera be kept?", "Cancelled. Nothing was done.")
	if len(e.cam.Commands()) != 0 {
		t.Errorf("commands sent after cancelling: %v", e.cam.Commands())
	}
}

func TestBackupFolderCheck(t *testing.T) {
	e := newEnv(t)
	akamState(e, "")
	bk := backupFixture(t)
	r := e.run("4\n3\n"+bk+"\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, bk+": 8 partitions present, SHA256SUMS matches.")
	if err := os.WriteFile(filepath.Join(bk, "mtdblock2.bin"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = e.run("4\n3\n"+bk+"\n10\n", "--camera", "default")
	if strings.Contains(r.out, "SHA256SUMS matches.") {
		t.Errorf("a damaged backup passed:\n%s", r.out)
	}
}

func TestSettingsScreen(t *testing.T) {
	e := newEnv(t)
	akamState(e, "")
	t.Setenv("AKAM_SD_ELEVATE", "pkexec")
	want, _ := os.ReadFile(filepath.Join("..", "..", "..", "..", "THIRD_PARTY_NOTICES.md"))
	r := e.run("6\n2\n3\n10\n", "--camera", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Settings", "Backup folder:  ", "State folder:   "+e.state, "SD helper mode: pkexec", string(want))
}
