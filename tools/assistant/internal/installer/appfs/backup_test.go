package appfs

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

const realProcMtd = `dev:    size   erasesize  name
mtd0: 00040000 00010000 "uboot"
mtd1: 00180000 00010000 "kernel"
mtd2: 00150000 00010000 "rootfs"
mtd3: 00490000 00010000 "appfs"
mtd4: 00040000 00010000 "config"
mtd5: 00020000 00010000 "factory"
`

// bcam is a camera that only answers reads; any other command fails the test (a backup writes nothing).
type bcam struct {
	t        *testing.T
	proc     string
	mtd      [6][]byte
	reads    [6]int
	flipRead map[int]int // partition -> read number (1 or 2) whose first byte is flipped
	shortOn  int         // partition whose read is cut (-1: none)
	cmds     []string
}

func newBCam(t *testing.T) *bcam {
	c := &bcam{t: t, proc: realProcMtd, shortOn: -1, flipRead: map[int]int{}}
	for n := range c.mtd {
		c.mtd[n] = bytes.Repeat([]byte{byte(0x20 + n)}, mtdLayout[n])
	}
	copy(c.mtd[3], squashMagic)
	return c
}

func (c *bcam) Close() {}
func (c *bcam) Run(_ context.Context, cmd string, _ io.Reader) (camssh.Result, error) {
	c.cmds = append(c.cmds, cmd)
	switch {
	case cmd == "cat /proc/mtd":
		return camssh.Result{Stdout: []byte(c.proc)}, nil
	case strings.HasPrefix(cmd, "dd if=/dev/mtd") && strings.HasSuffix(cmd, "bs=65536 2>/dev/null"):
		var n int
		_, _ = fmt.Sscanf(cmd, "dd if=/dev/mtd%d", &n)
		c.reads[n]++
		b := append([]byte(nil), c.mtd[n]...)
		if c.flipRead[n] == c.reads[n] {
			b[0] ^= 0xff
		}
		if c.shortOn == n {
			b = b[:len(b)-1]
		}
		return camssh.Result{Stdout: b}, nil
	}
	c.t.Errorf("the backup sent a command other than a read: %q", cmd)
	return camssh.Result{Status: 1}, nil
}

func (c *bcam) dial(int) (Conn, error) { return c, nil }

func TestBackupWritesSixVerifiedPartitions(t *testing.T) {
	c := newBCam(t)
	dir := filepath.Join(t.TempDir(), "backup")
	res, err := Backup(context.Background(), c.dial, dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	for n := 0; n < 6; n++ {
		b, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("mtd%d.bin", n)))
		if err != nil || !bytes.Equal(b, c.mtd[n]) {
			t.Fatalf("mtd%d: %v", n, err)
		}
		if c.reads[n] != 2 {
			t.Errorf("mtd%d was read %d times, want 2", n, c.reads[n])
		}
		if st, _ := os.Stat(filepath.Join(dir, fmt.Sprintf("mtd%d.bin", n))); st.Mode().Perm()&0o077 != 0 {
			t.Errorf("mtd%d.bin is readable by others: %v", n, st.Mode())
		}
	}
	if err := VerifyBackupDir(dir); err != nil || len(res.Sums) != 6 {
		t.Fatalf("%v %v", err, res)
	}
	// the folder is what update and appfs-flash take as --backup-dir: its mtd3 is the base of the image
	if got := res.Sums["mtd3.bin"]; got != sum(c.mtd[3]) {
		t.Fatal("mtd3 hash")
	}
	for _, cmd := range c.cmds {
		if !strings.HasPrefix(cmd, "dd if=/dev/mtd") && cmd != "cat /proc/mtd" {
			t.Errorf("not a read: %q", cmd)
		}
	}
}

func TestBackupRefusals(t *testing.T) {
	cases := map[string]struct {
		mod  func(c *bcam)
		want string
	}{
		"1080p layout": {func(c *bcam) {
			c.proc = "dev: size erasesize name\nmtd0: 00032000 00001000 \"UBOOT\"\nmtd1: 00001000 00001000 \"X\"\n"
		}, "not the layout of the 2K"},
		"a partition of another size": {func(c *bcam) {
			c.proc = strings.Replace(realProcMtd, "00490000", "00500000", 1)
		}, "refused"},
		"two reads differ": {func(c *bcam) { c.flipRead[4] = 2 }, "two reads of mtd4 differ"},
		"a short read":     {func(c *bcam) { c.shortOn = 2 }, "mtd2 came back with"},
		"mtd3 is not a squashfs": {func(c *bcam) {
			c.mtd[3][0] = 'x'
		}, "not a squashfs"},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			c := newBCam(t)
			tc.mod(c)
			dir := filepath.Join(t.TempDir(), "backup")
			if _, err := Backup(context.Background(), c.dial, dir, nil); err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("want %q, got %v", tc.want, err)
			}
			if ents, _ := os.ReadDir(dir); len(ents) != 0 {
				t.Errorf("a failed backup left %d file(s)", len(ents))
			}
		})
	}
	t.Run("the camera cannot be reached", func(t *testing.T) {
		_, err := Backup(context.Background(), func(int) (Conn, error) { return nil, errors.New("no route") }, filepath.Join(t.TempDir(), "b"), nil)
		if err == nil || !strings.Contains(err.Error(), "Nothing was done") {
			t.Fatal(err)
		}
	})
	t.Run("an existing non-empty folder is never touched", func(t *testing.T) {
		c := newBCam(t)
		dir := t.TempDir()
		keep := filepath.Join(dir, "keep.txt")
		_ = os.WriteFile(keep, []byte("x"), 0o600)
		if _, err := Backup(context.Background(), c.dial, dir, nil); err == nil {
			t.Fatal("accepted")
		}
		if len(c.cmds) != 0 {
			t.Fatal("the camera was contacted before the folder was refused")
		}
		if b, _ := os.ReadFile(keep); string(b) != "x" {
			t.Fatal("the file was changed")
		}
	})
}

// A backup that fails while writing leaves nothing that could pass for a backup.
func TestBackupFailureWhileWritingLeavesNothing(t *testing.T) {
	c := newBCam(t)
	dir := filepath.Join(t.TempDir(), "backup")
	failRenames(t, "SHA256SUMS")
	if _, err := Backup(context.Background(), c.dial, dir, nil); err == nil {
		t.Fatal("reported success")
	}
	if ents, _ := os.ReadDir(dir); len(ents) != 0 {
		t.Fatalf("%d file(s) left: a half backup could be mistaken for a full one", len(ents))
	}
}

// A damaged folder is refused by the verifier the update flow relies on.
func TestVerifyBackupDirDetectsDamage(t *testing.T) {
	c := newBCam(t)
	dir := filepath.Join(t.TempDir(), "backup")
	if _, err := Backup(context.Background(), c.dial, dir, nil); err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "mtd1.bin")
	b, _ := os.ReadFile(p)
	b[10] ^= 1
	_ = os.WriteFile(p, b, 0o600)
	if err := VerifyBackupDir(dir); err == nil || !strings.Contains(err.Error(), "mtd1.bin") {
		t.Fatal(err)
	}
}
