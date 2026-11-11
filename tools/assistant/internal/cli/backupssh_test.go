//go:build akamtest

package cli_test

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// sshBackupWorld is an Akam camera (the fake) whose state says "camera_kind=akam".
func sshBackupWorld(t *testing.T, opts ...func(*testfake.Options)) (*env, string) {
	t.Helper()
	e := newEnv(t, opts...)
	root := t.TempDir()
	t.Setenv("AKAM_BACKUP_ROOT", root)
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=akam\n")
	return e, root
}

func sshBackupDirs(root string) []string {
	ms, _ := filepath.Glob(filepath.Join(root, "akam-backup-*"))
	return ms
}

func ddCommands(e *env) (n int) {
	for _, c := range e.cam.Commands() {
		if strings.HasPrefix(c, "dd ") {
			n++
		}
	}
	return n
}

var readOnlyDd = regexp.MustCompile(`^dd if=/dev/mtdblock[0-7] bs=64k$`)

func TestBackupSSHSuccess(t *testing.T) {
	e, root := sshBackupWorld(t)
	r := e.run("", "backup")
	expectRC(t, r, 0)
	contains(t, r.out, "This camera runs Akam: this saves what is on the camera now.",
		"  Partition table: LAYOUT_OK (8 partitions, APP 00500000).",
		"  READS_IDENTICAL:", "  Backup saved and verified")
	dirs := sshBackupDirs(root)
	if len(dirs) != 1 {
		t.Fatalf("backup folders: %v", dirs)
	}
	d := dirs[0]
	var want strings.Builder
	for n := 0; n < 8; n++ {
		f := fmt.Sprintf("mtdblock%d.bin", n)
		got, err := os.ReadFile(filepath.Join(d, f))
		if err != nil || !bytes.Equal(got, testfake.MtdData(n)) {
			t.Errorf("%s differs (err %v)", f, err)
		}
		s := sha256.Sum256(got)
		fmt.Fprintf(&want, "%s  %s\n", hex.EncodeToString(s[:]), f)
	}
	sums, _ := os.ReadFile(filepath.Join(d, "SHA256SUMS"))
	if string(sums) != want.String() {
		t.Errorf("SHA256SUMS:\n%s\nwant:\n%s", sums, want.String())
	}
	line := regexp.MustCompile(`(?m)^[0-9a-f]{64}  mtdblock[0-7]\.bin$`)
	if len(line.FindAllString(string(sums), -1)) != 8 {
		t.Errorf("SHA256SUMS format: %s", sums)
	}
	info, _ := os.ReadFile(filepath.Join(d, "camera_info.txt"))
	contains(t, string(info), "camera: 127.0.0.1\n", "\nkind=akam\n", "config_partition_differs_between_reads: 0\n", "--- /proc/mtd\nmtd0: 00032000 00001000 \"UBOOT\"", "mtd7: 00500000 00001000 \"APP\"")
	readme, _ := os.ReadFile(filepath.Join(d, "README.txt"))
	contains(t, string(readme), "Akam backup of the camera at 127.0.0.1, made ", "Check them with: sha256sum -c SHA256SUMS\n")
	es, _ := os.ReadDir(d)
	if len(es) != 11 {
		t.Errorf("%d entries in the folder (partial or extra file?)", len(es))
	}
	for _, en := range es {
		if strings.HasSuffix(en.Name(), ".part") {
			t.Errorf("temporary file left: %s", en.Name())
		}
		if runtime.GOOS != "windows" && modeOf(t, filepath.Join(d, en.Name())) != 0o600 {
			t.Errorf("%s is not 0600", en.Name())
		}
	}
	if runtime.GOOS != "windows" && modeOf(t, d) != 0o700 {
		t.Errorf("folder mode %v", modeOf(t, d))
	}
	// 16 reads, only the read-only form, nothing else that reads or writes the flash
	if n := ddCommands(e); n != 16 {
		t.Errorf("%d dd commands, want 16", n)
	}
	for _, c := range e.cam.Commands() {
		if strings.HasPrefix(c, "dd ") && !readOnlyDd.MatchString(c) {
			t.Errorf("not the read-only form: %q", c)
		}
		if strings.Contains(c, "of=") {
			t.Errorf("of= sent: %q", c)
		}
	}
	// the stock backup state is not touched: it is bound to the original firmware
	if e.readState("backup_dir") != "" || e.readState("backup_sha") != "" || e.readState("step_backup_done") != "" {
		t.Error("the SSH backup changed the stock backup state")
	}
}

func TestBackupSSHDifferingSecondRead(t *testing.T) {
	e, root := sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 3 && read == 2 {
				data[10] ^= 0xff
			}
			return data, false
		}
	})
	r := e.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the two reads differ on mtdblock3 (not CONFIG only): the read is not reliable.")
	if es, _ := os.ReadDir(root); len(es) != 0 {
		t.Errorf("files left in the backup root: %v", es)
	}
}

func TestBackupSSHConfigMayDiffer(t *testing.T) {
	// once: read again twice, then identical
	e, root := sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 6 && read == 2 {
				data[0] ^= 1
			}
			return data, false
		}
	})
	r := e.run("", "backup")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "only mtdblock6 (CONFIG, read/write while the camera runs) differs: reading it again (1 of 2).")
	d := sshBackupDirs(root)[0]
	info, _ := os.ReadFile(filepath.Join(d, "camera_info.txt"))
	contains(t, string(info), "config_partition_differs_between_reads: 0\n")
	if ddCommands(e) != 18 {
		t.Errorf("%d dd commands, want 18", ddCommands(e))
	}
	// always: two redos, then the first read is used and the folder says so
	e, root = sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 6 && read%2 == 0 {
				data[0] ^= 1
			}
			return data, false
		}
	})
	r = e.run("", "backup")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "(2 of 2)", "mtdblock6 (CONFIG) still differs between the two reads.")
	d = sshBackupDirs(root)[0]
	info, _ = os.ReadFile(filepath.Join(d, "camera_info.txt"))
	contains(t, string(info), "config_partition_differs_between_reads: 1\n")
	if ddCommands(e) != 20 {
		t.Errorf("%d dd commands, want 20", ddCommands(e))
	}
}

func TestBackupSSHDroppedSessionLeavesNothing(t *testing.T) {
	e, root := sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 5 && read == 1 {
				return data[:len(data)/2], true
			}
			return data, false
		}
	})
	r := e.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "mtdblock5")
	if es, _ := os.ReadDir(root); len(es) != 0 {
		t.Errorf("something was left in the backup root: %v", es)
	}
}

func TestBackupSSHSizeMismatch(t *testing.T) {
	e, root := sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 2 && read == 1 {
				return data[:100], false
			}
			return data, false
		}
	})
	r := e.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "mtdblock2 is 100 bytes, expected 4096")
	if es, _ := os.ReadDir(root); len(es) != 0 {
		t.Errorf("something was left: %v", es)
	}
}

func TestBackupSSHOversizeStreamRefused(t *testing.T) {
	e, root := sshBackupWorld(t, func(o *testfake.Options) {
		o.MtdRead = func(n, read int, data []byte) ([]byte, bool) {
			if n == 1 && read == 2 {
				return append(data, make([]byte, 5000)...), false
			}
			return data, false
		}
	})
	r := e.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "mtdblock1: the camera sent more than the 4096 bytes of /proc/mtd; refused")
	if es, _ := os.ReadDir(root); len(es) != 0 {
		t.Errorf("something was left: %v", es)
	}
}

func TestBackupSSHUnsupportedLayoutAndHostileText(t *testing.T) {
	hostile := strings.Replace(testfake.ProcMtdText, `"APP"`, "\"APP\"\x1b]0;pwned\x07\x1b[2J", 1)
	e, root := sshBackupWorld(t, func(o *testfake.Options) { o.ProcMtd = hostile })
	r := e.run("", "backup")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the partition table of this camera is not the supported 8-partition layout:")
	notContains(t, r.out+r.err, "\x1b]0;", "\x1b[2J", "\x07")
	if ddCommands(e) != 0 || len(sshBackupDirs(root)) != 0 {
		t.Error("a read was sent or a folder made for an unsupported layout")
	}
	// a bigger APP than the supported one is refused too (the size cap comes from /proc/mtd)
	big := strings.Replace(testfake.ProcMtdText, "mtd7: 00500000", "mtd7: 02000000", 1)
	e, _ = sshBackupWorld(t, func(o *testfake.Options) { o.ProcMtd = big })
	expectRC(t, e.run("", "backup"), 1)
	if ddCommands(e) != 0 {
		t.Error("a read was sent")
	}
}
