package cli_test

import (
	"bytes"
	"context"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
)

// ---- synthetic backups (never a real one) ----

var sizes1080 = []int{204800, 4096, 4096, 65536, 1572864, 1032192, 262144, 5242880}

func fill(n, seed int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte((i*7 + seed*31 + i/251) % 251)
	}
	return b
}

func shaHex(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }
func md5Hex(b []byte) string { h := md5.Sum(b); return hex.EncodeToString(h[:]) }

type synth struct {
	names []string
	data  map[string][]byte
}

// make1080 builds the eight partitions of the 1080p; seed makes two backups differ.
func make1080(seed int) synth {
	s := synth{data: map[string][]byte{}}
	for i, sz := range sizes1080 {
		n := fmt.Sprintf("mtdblock%d.bin", i)
		s.names = append(s.names, n)
		s.data[n] = fill(sz, seed+i)
	}
	return s
}

func make2K(seed int) synth {
	s := synth{data: map[string][]byte{}}
	for i, sz := range appfs.Layout() {
		n := fmt.Sprintf("mtd%d.bin", i)
		s.names = append(s.names, n)
		s.data[n] = fill(sz, seed+i)
	}
	return s
}

func (s synth) write(t *testing.T, dir string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, n := range s.names {
		if err := os.WriteFile(filepath.Join(dir, n), s.data[n], 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func (s synth) sha256sums() string {
	var sb strings.Builder
	for _, n := range s.names {
		fmt.Fprintf(&sb, "%s  %s\n", shaHex(s.data[n]), n)
	}
	return sb.String()
}

func (s synth) md5sums() string {
	var sb strings.Builder
	for _, n := range s.names {
		fmt.Fprintf(&sb, "%s  %s\n", md5Hex(s.data[n]), n)
	}
	return sb.String()
}

func put(t *testing.T, p, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

// srcTreeHash hashes names, modes, sizes and contents of a whole tree: the source of an import must not change.
func srcTreeHash(t *testing.T, root string) string {
	t.Helper()
	h := sha256.New()
	var paths []string
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err == nil {
			paths = append(paths, p)
		}
		return nil
	})
	sort.Strings(paths)
	for _, p := range paths {
		st, err := os.Lstat(p)
		if err != nil {
			t.Fatal(err)
		}
		rel, _ := filepath.Rel(root, p)
		fmt.Fprintf(h, "%s|%v|%d|%d\n", rel, st.Mode(), st.Size(), st.ModTime().UnixNano())
		if st.Mode().IsRegular() {
			b, _ := os.ReadFile(p)
			h.Write(b)
		}
	}
	return hex.EncodeToString(h.Sum(nil))
}

func fileExists(p string) bool { _, err := os.Lstat(p); return err == nil }

func privateMode(t *testing.T, p string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		return
	}
	st, err := os.Stat(p)
	if err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm()&0o077 != 0 {
		t.Errorf("%s is not private: %v", p, st.Mode().Perm())
	}
}

// root of the backups of a test world: the home folder (no Documents there).
func (e *env) akamBackups() string { return filepath.Join(e.home, "Akam backups") }

// ---- backup-location ----

func TestBackupLocationShowSetDefault(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "backup-location")
	expectRC(t, r, 0)
	contains(t, r.out, "Backup folder:", "(suggested", e.akamBackups())

	dir := t.TempDir()
	r = e.run("", "backup-location", dir)
	expectRC(t, r, 0)
	contains(t, r.out, "The backups will be kept under "+filepath.Join(dir, "Akam backups"), "chosen by you")
	if got := e.readState("backup_root"); got != dir {
		t.Errorf("backup_root = %q, want %q", got, dir)
	}
	r = e.run("", "backup-location", "show")
	expectRC(t, r, 0)
	contains(t, r.out, dir, "Next backup of 'default'")

	// a rehearsal changes nothing
	other := t.TempDir()
	r = e.run("", "--dry-run", "backup-location", other)
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD keep the backups under "+filepath.Join(other, "Akam backups"))
	if got := e.readState("backup_root"); got != dir {
		t.Errorf("a dry run changed backup_root to %q", got)
	}

	r = e.run("", "backup-location", "default")
	expectRC(t, r, 0)
	contains(t, r.out, "Back to the suggested folder", "suggested")
	if got := e.readState("backup_root"); got != "" {
		t.Errorf("backup_root after default = %q", got)
	}
	r = e.run("", "backup-location", "a", "b")
	expectRC(t, r, 2)
}

func TestBackupLocationRefusals(t *testing.T) {
	e := newEnv(t)
	root := t.TempDir()

	r := e.run("", "backup-location", filepath.Join(root, "missing"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not an existing folder")

	file := filepath.Join(root, "file")
	put(t, file, "x")
	r = e.run("", "backup-location", file)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not an existing folder")

	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(filepath.Join(repo, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	sub := filepath.Join(repo, "sub")
	if err := os.MkdirAll(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, d := range []string{repo, sub} {
		r = e.run("", "backup-location", d)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "inside a git repository")
	}

	cloud := filepath.Join(root, "Dropbox")
	if err := os.MkdirAll(cloud, 0o755); err != nil {
		t.Fatal(err)
	}
	r = e.run("", "backup-location", cloud)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "looks like part of Dropbox")
	if got := e.readState("backup_root"); got != "" {
		t.Errorf("a refused cloud folder was recorded: %q", got)
	}
	r = e.run("", "backup-location", cloud, "--yes")
	expectRC(t, r, 0)
	contains(t, r.out, "looks like part of Dropbox")
	if got := e.readState("backup_root"); got != cloud {
		t.Errorf("--yes: backup_root = %q", got)
	}
	// Windows-style text on a Unix host is just not a folder there; on Windows it is checked as a path
	if runtime.GOOS != "windows" {
		r = e.run("", "backup-location", `C:\Users\me\Backups`)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "is not an existing folder")
	}
	r = e.run("", "backup-location", "bad\npath")
	expectRC(t, r, 1)
}

// ---- backup-2k location ----

// fake2KBackup is Hooks.Backup2K: six synthetic partitions and their SHA256SUMS, as appfs.Backup leaves them.
func fake2KBackup(s synth, fail bool) func(context.Context, func(int) (appfs.Conn, error), string, func(string)) (*appfs.BackupResult, error) {
	return func(_ context.Context, _ func(int) (appfs.Conn, error), dir string, _ func(string)) (*appfs.BackupResult, error) {
		if fail {
			return nil, fmt.Errorf("reading mtd3 failed")
		}
		if ents, _ := os.ReadDir(dir); len(ents) > 0 {
			return nil, fmt.Errorf("%s is not empty", dir)
		}
		for _, n := range s.names {
			if err := os.WriteFile(filepath.Join(dir, n), s.data[n], 0o600); err != nil {
				return nil, err
			}
		}
		if err := os.WriteFile(filepath.Join(dir, "SHA256SUMS"), []byte(s.sha256sums()), 0o600); err != nil {
			return nil, err
		}
		return &appfs.BackupResult{Dir: dir}, nil
	}
}

func (e *env) runWith(h cli.Hooks, stdin string, args ...string) result {
	var out, errb bytes.Buffer
	rc := cli.Run(append(e.base(), args...), cli.Streams{In: strings.NewReader(stdin), Out: &out, Err: &errb}, "test", h)
	return result{out.String(), errb.String(), rc}
}

func TestBackup2KUsesTheBackupLocation(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	s := make2K(1)
	h := e.hooks()
	h.Backup2K = fake2KBackup(s, false)
	today := time.Now().Format("20060102")

	// default: <home>/Akam backups/<camera>/<date>, private, with the three files
	r := e.runWith(h, "", "--model", rotModel, "backup-2k")
	expectRC(t, r, 0)
	dir := filepath.Join(e.akamBackups(), "camera", today)
	contains(t, r.out, "Backup verified: "+dir)
	for _, n := range append([]string{"SHA256SUMS", "README.txt", "camera_info.txt"}, s.names...) {
		if !fileExists(filepath.Join(dir, n)) {
			t.Errorf("%s is missing from the backup folder", n)
		}
	}
	for _, p := range []string{e.akamBackups(), filepath.Dir(dir), dir, filepath.Join(dir, "README.txt"), filepath.Join(dir, "mtd0.bin")} {
		privateMode(t, p)
	}
	info, _ := os.ReadFile(filepath.Join(dir, "camera_info.txt"))
	contains(t, string(info), "model="+rotModel, "kind=cardless-appfs", "source=ssh", "mtd3.bin 4784128")
	readme, _ := os.ReadFile(filepath.Join(dir, "README.txt"))
	contains(t, string(readme), "mtd0..5.bin are the 6 flash partitions", "sha256sum -c SHA256SUMS")
	if got := e.readState("rot2k_backup"); got != dir {
		t.Errorf("rot2k_backup = %q, want %q", got, dir)
	}
	if err := appfs.VerifyBackupDir(dir); err != nil {
		t.Errorf("update could not use it: %v", err)
	}
	if fileExists(filepath.Join(e.state, "backups")) {
		t.Error("the backup went to the state folder")
	}

	// the same day again: the -N suffix, the first one untouched
	before := srcTreeHash(t, dir)
	r = e.runWith(h, "", "--model", rotModel, "backup-2k")
	expectRC(t, r, 0)
	if !fileExists(dir+"-2") || srcTreeHash(t, dir) != before {
		t.Error("second backup of the day: no -2 folder, or the first changed")
	}

	// the folder chosen with backup-location
	chosen := t.TempDir()
	expectRC(t, e.run("", "backup-location", chosen), 0)
	r = e.runWith(h, "", "--model", rotModel, "backup-2k")
	expectRC(t, r, 0)
	if !fileExists(filepath.Join(chosen, "Akam backups", "camera", today, "mtd5.bin")) {
		t.Errorf("not under the chosen folder:\n%s", r.out)
	}
	if got := e.readState("rot2k_backup"); got != filepath.Join(chosen, "Akam backups", "camera", today) {
		t.Errorf("rot2k_backup = %q", got)
	}

	// --out still wins
	out := filepath.Join(t.TempDir(), "mine")
	r = e.runWith(h, "", "--model", rotModel, "backup-2k", "--out", out)
	expectRC(t, r, 0)
	if !fileExists(filepath.Join(out, "mtd0.bin")) || !fileExists(filepath.Join(out, "camera_info.txt")) {
		t.Errorf("--out folder incomplete:\n%s", r.out)
	}
	if got := e.readState("rot2k_backup"); got != out {
		t.Errorf("rot2k_backup = %q, want %q", got, out)
	}

	// a named camera has its own folder
	// (the chosen folder is a setting of each camera)
	expectRC(t, e.run("", "--camera", "salon", "backup-location", chosen), 0)
	r = e.runWith(h, "", "--camera", "salon", "--model", rotModel, "backup-2k")
	expectRC(t, r, 0)
	if !fileExists(filepath.Join(chosen, "Akam backups", "salon", today, "mtd0.bin")) {
		t.Errorf("no folder for the camera 'salon':\n%s", r.out)
	}
}

func TestBackup2KRefusals(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	s := make2K(2)

	// a failed read leaves no folder and no record
	h := e.hooks()
	h.Backup2K = fake2KBackup(s, true)
	r := e.runWith(h, "", "--model", rotModel, "backup-2k")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "reading mtd3 failed")
	day := filepath.Join(e.akamBackups(), "camera", time.Now().Format("20060102"))
	if fileExists(day) {
		t.Error("a failed backup left its folder")
	}
	if e.readState("rot2k_backup") != "" {
		t.Error("a failed backup was recorded")
	}

	// a folder inside a git tree is refused
	repo := t.TempDir()
	if err := os.MkdirAll(filepath.Join(repo, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	h.Backup2K = fake2KBackup(s, false)
	r = e.runWith(h, "", "--model", rotModel, "backup-2k", "--out", filepath.Join(repo, "b"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "inside the repository")

	// a non-empty --out is refused, nothing changed in it
	full := t.TempDir()
	put(t, filepath.Join(full, "keep.txt"), "x")
	r = e.runWith(h, "", "--model", rotModel, "backup-2k", "--out", full)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not empty")

	// the rehearsal says where, and writes nothing
	e2 := newEnv(t)
	e2.writeState("camera_model=" + rotModel + "\n")
	r = e2.runWith(h, "", "--dry-run", "--model", rotModel, "backup-2k")
	expectRC(t, r, 0)
	contains(t, r.out, "Akam backups", "README.txt and camera_info.txt")
	if fileExists(e2.akamBackups()) {
		t.Error("a dry run created the backup folder")
	}
}

// ---- import-backup ----

func (e *env) imp(args ...string) result {
	return e.run("", append([]string{"import-backup"}, args...)...)
}

func importedDir(e *env, camera, date string) string {
	return filepath.Join(e.akamBackups(), camera, date)
}

func TestImportBackup1080p(t *testing.T) {
	e := newEnv(t)
	s := make1080(3)
	src := filepath.Join(t.TempDir(), "akam-backup-20260314")
	s.write(t, src)
	put(t, filepath.Join(src, "SHA256SUMS"), s.sha256sums())
	put(t, filepath.Join(src, "camera_info.txt"), "camera: 192.0.2.7\nkernel: Linux x\n")
	put(t, filepath.Join(src, "hack_log.txt"), "how it was done\n")
	put(t, filepath.Join(src, "dump1", "mtdblock0.bin"), "extra")
	before := srcTreeHash(t, src)

	// rehearsal: says exactly what it would do and does nothing
	r := e.imp("--from", src, "--dry-run")
	expectRC(t, r, 0)
	dest := importedDir(e, "camera", "20260314")
	contains(t, r.out, "Import an existing backup", "1080p (8 partitions", "SHA256SUMS matches all 8 partitions",
		"Date: 20260314 (from the folder name)", "Destination:  "+dest,
		"WOULD copy mtdblock7.bin (5242880 bytes)", "WOULD copy hack_log.txt", "WOULD copy dump1/mtdblock0.bin",
		"WOULD create the sub folder dump1", "WOULD write README.txt", "WOULD mark camera_info.txt as imported", "WOULD re-read the copy")
	notContains(t, r.out, "WOULD write SHA256SUMS")
	if fileExists(e.akamBackups()) {
		t.Fatal("a dry run created folders")
	}

	r = e.imp("--from", src, "--camera", "salon", "--model", "1080p", "--record")
	expectRC(t, r, 0)
	dest = importedDir(e, "salon", "20260314")
	contains(t, r.out, "Imported and verified: "+dest, "Recorded as the backup of 'salon'")
	for _, n := range append([]string{"SHA256SUMS", "README.txt", "camera_info.txt", "hack_log.txt", "dump1/mtdblock0.bin"}, s.names...) {
		if !fileExists(filepath.Join(dest, filepath.FromSlash(n))) {
			t.Errorf("%s is missing from the copy", n)
		}
	}
	for _, p := range []string{filepath.Join(e.akamBackups(), "salon"), dest, filepath.Join(dest, "mtdblock3.bin"), filepath.Join(dest, "dump1")} {
		privateMode(t, p)
	}
	info, _ := os.ReadFile(filepath.Join(dest, "camera_info.txt"))
	contains(t, string(info), "camera: 192.0.2.7", "imported=1", "imported_from="+src)
	sums, _ := os.ReadFile(filepath.Join(dest, "SHA256SUMS"))
	if string(sums) != s.sha256sums() {
		t.Error("the original SHA256SUMS was not kept as it was")
	}
	if srcTreeHash(t, src) != before {
		t.Error("the source was modified")
	}
	// recorded in the state of the camera 'salon'
	if !strings.Contains(readAllState(e, "salon"), "backup_dir="+dest) {
		t.Errorf("backup_dir not recorded: %s", readAllState(e, "salon"))
	}

	// the same backup again: detected, nothing copied
	r = e.imp("--from", src, "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Already there: "+dest, "Nothing was copied")
	if fileExists(dest + "-2") {
		t.Error("a duplicate was copied twice")
	}
	// importing the copy itself is a duplicate too
	r = e.imp("--from", dest, "--camera", "salon")
	expectRC(t, r, 0)
	contains(t, r.out, "Already there")
	if srcTreeHash(t, src) != before {
		t.Error("the source was modified by the second import")
	}
}

func readAllState(e *env, cam string) string {
	var sb strings.Builder
	_ = filepath.WalkDir(e.state, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() && filepath.Base(p) == "state" && strings.Contains(p, cam) {
			b, _ := os.ReadFile(p)
			sb.Write(b)
		}
		return nil
	})
	return sb.String()
}

func TestImportBackupNoSumsAndDates(t *testing.T) {
	e := newEnv(t)
	s := make1080(4)
	src := filepath.Join(t.TempDir(), "dump")
	s.write(t, src)
	mt := time.Date(2025, 2, 3, 10, 0, 0, 0, time.UTC)
	for _, n := range s.names {
		if err := os.Chtimes(filepath.Join(src, n), mt, mt); err != nil {
			t.Fatal(err)
		}
	}
	r := e.imp("--from", src)
	expectRC(t, r, 0)
	dest := importedDir(e, "camera", "20250203")
	contains(t, r.out, "(from the date of the files)", "no SHA256SUMS or MD5SUMS in the source", "Imported and verified: "+dest)
	sums, _ := os.ReadFile(filepath.Join(dest, "SHA256SUMS"))
	if string(sums) != s.sha256sums() {
		t.Errorf("computed SHA256SUMS differs:\n%s", sums)
	}
	info, _ := os.ReadFile(filepath.Join(dest, "camera_info.txt"))
	contains(t, string(info), "imported=1", "source=import", "model=lsc-indoor-1080p-ak3918")

	// --date wins; a different backup on a taken date gets -2
	s2 := make1080(5)
	src2 := filepath.Join(t.TempDir(), "other")
	s2.write(t, src2)
	r = e.imp("--from", src2, "--date", "20250203")
	expectRC(t, r, 0)
	if !fileExists(dest+"-2") || !fileExists(filepath.Join(dest+"-2", "mtdblock0.bin")) {
		t.Errorf("no -2 folder:\n%s", r.out)
	}
	if !strings.Contains(r.out, "from --date") {
		t.Error("the date source is not said")
	}
	expectRC(t, e.imp("--from", src2, "--date", "2025-02-03"), 2)
	expectRC(t, e.imp("--from", src2, "--date", "20251340"), 2)
}

func Test2KImportWithSumFiles(t *testing.T) {
	e := newEnv(t)
	s := make2K(6)

	// MD5SUMS only (a hand-made dump): verified by MD5, SHA256SUMS written, MD5SUMS and extras kept
	src := filepath.Join(t.TempDir(), "2k-stock-20260101")
	s.write(t, src)
	put(t, filepath.Join(src, "MD5SUMS"), s.md5sums()+strings.Repeat("0", 32)+"  notkept.bin\n")
	put(t, filepath.Join(src, "hack_log.txt"), "log\n")
	put(t, filepath.Join(src, "dump_extra.txt"), "d\n")
	before := srcTreeHash(t, src)
	r := e.imp("--from", src, "--camera", "porch")
	expectRC(t, r, 0)
	dest := importedDir(e, "porch", "20260101")
	contains(t, r.out, "2K (6 partitions", "MD5SUMS matches all 6 partitions", "Imported and verified")
	for _, n := range []string{"MD5SUMS", "hack_log.txt", "dump_extra.txt", "SHA256SUMS", "README.txt", "camera_info.txt"} {
		if !fileExists(filepath.Join(dest, n)) {
			t.Errorf("%s missing", n)
		}
	}
	if err := appfs.VerifyBackupDir(dest); err != nil {
		t.Errorf("the copy is not usable by update: %v", err)
	}
	info, _ := os.ReadFile(filepath.Join(dest, "camera_info.txt"))
	contains(t, string(info), "kind=cardless-appfs", "imported=1", "mtd5.bin 131072", "model="+rotModel)
	if srcTreeHash(t, src) != before {
		t.Error("the source was modified")
	}

	// both sums, --record on the 2K: rot2k_backup
	src2 := filepath.Join(t.TempDir(), "b2")
	s2 := make2K(7)
	s2.write(t, src2)
	put(t, filepath.Join(src2, "SHA256SUMS"), s2.sha256sums())
	put(t, filepath.Join(src2, "MD5SUMS"), s2.md5sums())
	r = e.imp("--from", src2, "--camera", "porch", "--model", "2k", "--date", "20260202", "--record")
	expectRC(t, r, 0)
	contains(t, r.out, "SHA256SUMS matches all 6", "MD5SUMS matches all 6", "(rot2k_backup)")
	if !strings.Contains(readAllState(e, "porch"), "rot2k_backup="+importedDir(e, "porch", "20260202")) {
		t.Errorf("rot2k_backup not recorded: %s", readAllState(e, "porch"))
	}
	// the camera is now a 2K for later commands (options, update) without --model
	if !strings.Contains(readAllState(e, "porch"), "camera_model="+rotModel) {
		t.Errorf("camera_model not recorded on a 2K import: %s", readAllState(e, "porch"))
	}
}

func TestImportBackupRefusals(t *testing.T) {
	e := newEnv(t)
	root := t.TempDir()
	mk := func(name string, s synth) string {
		d := filepath.Join(root, name)
		s.write(t, d)
		return d
	}
	nothingCopied := func(label string, r result, want string) {
		t.Helper()
		expectRC(t, r, 1)
		contains(t, r.out+r.err, want)
		if fileExists(e.akamBackups()) {
			t.Errorf("%s: a refused import created folders", label)
		}
	}
	s := make1080(8)
	k := make2K(8)

	// partial
	p1 := mk("partial1080", s)
	os.Remove(filepath.Join(p1, "mtdblock5.bin"))
	nothingCopied("partial", e.imp("--from", p1), "partial: mtdblock5.bin missing")
	p2 := mk("partial2k", k)
	os.Remove(filepath.Join(p2, "mtd2.bin"))
	os.Remove(filepath.Join(p2, "mtd4.bin"))
	nothingCopied("partial2k", e.imp("--from", p2), "partial: mtd2.bin, mtd4.bin missing")

	// wrong size
	p3 := mk("short", k)
	put(t, filepath.Join(p3, "mtd1.bin"), "tiny")
	nothingCopied("size", e.imp("--from", p3), "mtd1.bin is 4 bytes")

	// mismatched sums
	p4 := mk("badsha", s)
	put(t, filepath.Join(p4, "SHA256SUMS"), strings.Replace(s.sha256sums(), shaHex(s.data["mtdblock2.bin"]), strings.Repeat("a", 64), 1))
	nothingCopied("badsha", e.imp("--from", p4), "SHA256SUMS does not match mtdblock2.bin")
	p5 := mk("badmd5", k)
	put(t, filepath.Join(p5, "MD5SUMS"), strings.Replace(k.md5sums(), md5Hex(k.data["mtd0.bin"]), strings.Repeat("b", 32), 1))
	nothingCopied("badmd5", e.imp("--from", p5), "MD5SUMS does not match mtd0.bin")
	// a flipped byte under correct sums
	p6 := mk("flipped", k)
	put(t, filepath.Join(p6, "SHA256SUMS"), k.sha256sums())
	b := append([]byte(nil), k.data["mtd3.bin"]...)
	b[100] ^= 0xff
	if err := os.WriteFile(filepath.Join(p6, "mtd3.bin"), b, 0o644); err != nil {
		t.Fatal(err)
	}
	nothingCopied("flipped", e.imp("--from", p6), "SHA256SUMS does not match mtd3.bin")
	// sums that do not cover the whole backup, or are garbage
	p7 := mk("incomplete", s)
	put(t, filepath.Join(p7, "SHA256SUMS"), strings.Join(strings.SplitAfter(s.sha256sums(), "\n")[:3], ""))
	nothingCopied("incomplete", e.imp("--from", p7), "does not list mtdblock3.bin")
	p8 := mk("garbage", s)
	put(t, filepath.Join(p8, "SHA256SUMS"), "not a checksum file\n")
	nothingCopied("garbage", e.imp("--from", p8), "is not usable")
	p9 := mk("traversal", s)
	put(t, filepath.Join(p9, "SHA256SUMS"), shaHex([]byte("x"))+"  ../escape\n")
	nothingCopied("traversal", e.imp("--from", p9), "is not usable")

	// mixed, wrong model flag, other layout, no partitions, not a folder, missing --from
	p10 := mk("mixed", s)
	k.write(t, p10)
	nothingCopied("mixed", e.imp("--from", p10), "both mtdblock*.bin (1080p) and mtd*.bin (2K)")
	p11 := mk("is1080", s)
	nothingCopied("model", e.imp("--from", p11, "--model", "2k"), "--model 2k, but the folder holds the 1080p files")
	p12 := mk("is2k", k)
	nothingCopied("model2", e.imp("--from", p12, "--model", "1080p"), "--model 1080p, but the folder holds the 2K files")
	p13 := mk("seven", s)
	put(t, filepath.Join(p13, "mtdblock8.bin"), "x")
	nothingCopied("layout", e.imp("--from", p13), "different layout")
	p14 := filepath.Join(root, "empty")
	if err := os.MkdirAll(p14, 0o755); err != nil {
		t.Fatal(err)
	}
	nothingCopied("empty", e.imp("--from", p14), "holds no mtdblock0..7.bin")
	nothingCopied("file", e.imp("--from", filepath.Join(p14, "nothing")), "not a folder")
	expectRC(t, e.imp(), 2)
	expectRC(t, e.imp("--from", p11, "--model", "3k"), 2)
	if runtime.GOOS != "windows" {
		p15 := mk("link", s)
		os.Remove(filepath.Join(p15, "mtdblock1.bin"))
		if os.Symlink(filepath.Join(p11, "mtdblock1.bin"), filepath.Join(p15, "mtdblock1.bin")) == nil {
			nothingCopied("symlink", e.imp("--from", p15), "is not a regular file")
		}
	}
	// a destination inside a git tree (a chosen root) is refused
	repo := filepath.Join(root, "repo")
	if err := os.MkdirAll(filepath.Join(repo, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	expectRC(t, e.run("", "backup-location", repo), 1)
	if e.readState("backup_root") != "" {
		t.Error("a git folder became the backup root")
	}
}

func TestImportBackupUsesTheChosenRootAndNeverTouchesTheSource(t *testing.T) {
	e := newEnv(t)
	chosen := t.TempDir()
	expectRC(t, e.run("", "backup-location", chosen), 0)
	s := make2K(9)
	src := filepath.Join(t.TempDir(), "x")
	s.write(t, src)
	put(t, filepath.Join(src, "SHA256SUMS"), s.sha256sums())
	before := srcTreeHash(t, src)
	r := e.imp("--from", src, "--date", "20260505")
	expectRC(t, r, 0)
	if !fileExists(filepath.Join(chosen, "Akam backups", "camera", "20260505", "mtd0.bin")) {
		t.Errorf("not under the chosen folder:\n%s", r.out)
	}
	if srcTreeHash(t, src) != before {
		t.Error("the source changed")
	}
	// importing into a root that lies inside the source is refused (no copy into itself)
	inner := filepath.Join(src, "root")
	if err := os.MkdirAll(inner, 0o755); err != nil {
		t.Fatal(err)
	}
	expectRC(t, e.run("", "backup-location", inner), 0)
	before = srcTreeHash(t, src)
	r = e.imp("--from", src, "--date", "20260606")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "inside the folder you import from")
	if srcTreeHash(t, src) != before {
		t.Error("the source changed")
	}
}

// Windows-style paths: the date and the name of a folder are read from either separator.
func TestImportWindowsStyleNames(t *testing.T) {
	for in, want := range map[string]string{
		`C:\Users\me\Documents\akam-backup-salon-20260314`: "20260314",
		`D:\backups\2026-03-14\`:                           "20260314",
		`\\nas\share\cam\20250102T101500Z`:                 "20250102",
		`/home/me/backup_20240229`:                         "20240229",
	} {
		got := cli.DateFromName(in)
		if got != want {
			t.Errorf("DateFromName(%q) = %q, want %q", in, got, want)
		}
	}
	for _, in := range []string{`C:\Users\20261301\dump`, `E:\1234567890123\x`, `C:\data\dump1`, `C:\2026031`} {
		if got := cli.DateFromName(in); got != "" {
			t.Errorf("DateFromName(%q) = %q, want none", in, got)
		}
	}
}
