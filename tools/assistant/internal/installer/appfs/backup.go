package appfs

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// The full backup of the card-less 2K: its six flash partitions read over SSH (`dd if=/dev/mtdN`, nothing is written
// anywhere on the camera), each READ TWICE and compared, sizes checked against the layout, written to a new folder on
// the PC with a SHA256SUMS that is verified from the files on disk. The folder is what `update` and `appfs-flash` take
// as --backup-dir (mtd0..mtd5.bin and SHA256SUMS).

// mtdLayout is the flash map of the 2K (/proc/mtd): partition sizes in bytes.
var mtdLayout = [6]int{0x40000, 0x180000, 0x150000, 0x490000, 0x40000, 0x20000}

var mtdLineRe = regexp.MustCompile(`(?m)^mtd([0-9]+): ([0-9a-f]{8}) [0-9a-f]{8} "`)

// BackupResult of Backup.
type BackupResult struct {
	Dir  string
	Sums map[string]string // file name -> sha256
}

// Backup dumps the six partitions into dir, which must not exist or be empty. It refuses a camera whose /proc/mtd is
// not the 2K layout, a short read, two reads that differ, and a folder that does not read back as written. A failed
// backup removes what it wrote (a half backup must not be mistaken for a full one).
func Backup(ctx context.Context, dial func(port int) (Conn, error), dir string, say func(string)) (res *BackupResult, err error) {
	if say == nil {
		say = func(string) {}
	}
	if st, e := os.Stat(dir); e == nil {
		ents, _ := os.ReadDir(dir)
		if !st.IsDir() || len(ents) > 0 {
			return nil, fmt.Errorf("%s exists and is not an empty folder: a backup never overwrites anything. Choose a new folder", dir)
		}
	}
	conn, err := dial(0)
	if err != nil {
		return nil, fmt.Errorf("cannot connect to the camera: %w. Nothing was done", err)
	}
	defer conn.Close()
	r, err := conn.Run(ctx, "cat /proc/mtd", nil)
	if err != nil || r.Status != 0 {
		return nil, errors.New("cannot read /proc/mtd on the camera: this is not the layout of the 2K. Nothing was done")
	}
	if err := checkLayout(r.Stdout); err != nil {
		return nil, err
	}
	var dumps [6][]byte
	for n := 0; n < 6; n++ {
		var first []byte
		for pass := 1; pass <= 2; pass++ {
			say(fmt.Sprintf("Reading mtd%d (%d KiB), read %d of 2", n, mtdLayout[n]/1024, pass))
			rr, e := conn.Run(ctx, fmt.Sprintf("dd if=/dev/mtd%d bs=65536 2>/dev/null", n), nil)
			if e != nil || rr.Status != 0 {
				return nil, fmt.Errorf("reading mtd%d failed: nothing was written on the camera, no backup was made", n)
			}
			if len(rr.Stdout) != mtdLayout[n] {
				return nil, fmt.Errorf("mtd%d came back with %d bytes, expected %d: the link dropped data. No backup was made; retry", n, len(rr.Stdout), mtdLayout[n])
			}
			if pass == 1 {
				first = rr.Stdout
				continue
			}
			if !bytes.Equal(first, rr.Stdout) {
				return nil, fmt.Errorf("two reads of mtd%d differ (a flaky link, or the partition changed while reading): no backup was made; retry", n)
			}
		}
		dumps[n] = first
	}
	if string(dumps[3][:4]) != squashMagic {
		return nil, errors.New("mtd3 is not a squashfs appfs: this is not the camera this backup is for. No backup was made")
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	defer func() {
		if err != nil {
			for n := 0; n < 6; n++ {
				_ = os.Remove(filepath.Join(dir, fmt.Sprintf("mtd%d.bin", n)))
			}
			_ = os.Remove(filepath.Join(dir, "SHA256SUMS"))
		}
	}()
	sums := map[string]string{}
	var sb strings.Builder
	for n := 0; n < 6; n++ {
		name := fmt.Sprintf("mtd%d.bin", n)
		if err = writeFileAtomic(filepath.Join(dir, name), dumps[n]); err != nil {
			return nil, fmt.Errorf("cannot write %s: %w", name, err)
		}
		sums[name] = sum(dumps[n])
		fmt.Fprintf(&sb, "%s  %s\n", sums[name], name)
	}
	if err = writeFileAtomic(filepath.Join(dir, "SHA256SUMS"), []byte(sb.String())); err != nil {
		return nil, fmt.Errorf("cannot write SHA256SUMS: %w", err)
	}
	if err = VerifyBackupDir(dir); err != nil {
		return nil, fmt.Errorf("the backup does not read back as written: %w", err)
	}
	say("Backup verified from the files on disk (six partitions, two identical reads each).")
	return &BackupResult{Dir: dir, Sums: sums}, nil
}

// VerifyBackupDir checks a backup folder: the six files, their sizes, the SHA256SUMS lines and every hash.
func VerifyBackupDir(dir string) error {
	b, err := os.ReadFile(filepath.Join(dir, "SHA256SUMS"))
	if err != nil {
		return errors.New("no SHA256SUMS")
	}
	listed, err := parseSums(b)
	if err != nil {
		return err
	}
	for n := 0; n < 6; n++ {
		name := "mtd" + strconv.Itoa(n) + ".bin"
		f, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			return fmt.Errorf("%s is missing", name)
		}
		if len(f) != mtdLayout[n] {
			return fmt.Errorf("%s is %d bytes, expected %d", name, len(f), mtdLayout[n])
		}
		if listed[name] != sum(f) {
			return fmt.Errorf("%s does not match SHA256SUMS", name)
		}
	}
	return nil
}

func checkLayout(proc []byte) error {
	got := map[int]int{}
	for _, m := range mtdLineRe.FindAllSubmatch(proc, -1) {
		n, _ := strconv.Atoi(string(m[1]))
		sz, _ := strconv.ParseInt(string(m[2]), 16, 64)
		got[n] = int(sz)
	}
	if len(got) != 6 {
		keys := make([]int, 0, len(got))
		for k := range got {
			keys = append(keys, k)
		}
		sort.Ints(keys)
		return fmt.Errorf("/proc/mtd lists %d partitions (%v), the 2K has 6: this is not the layout of the 2K. Nothing was done", len(got), keys)
	}
	for n := 0; n < 6; n++ {
		if got[n] != mtdLayout[n] {
			return fmt.Errorf("mtd%d is %#x bytes on this camera, the 2K has %#x: refused, nothing was done", n, got[n], mtdLayout[n])
		}
	}
	return nil
}
