package squashfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io/fs"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// These tests use mksquashfs and unsquashfs when they are installed (test-only exec; skipped otherwise).

func lookTools() error {
	for _, tool := range []string{"mksquashfs", "unsquashfs"} {
		if _, err := exec.LookPath(tool); err != nil {
			return fmt.Errorf("%s is not installed", tool)
		}
	}
	return nil
}

func needTools(t *testing.T) {
	t.Helper()
	if err := lookTools(); err != nil {
		t.Skip(err)
	}
}

func run(t *testing.T, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		t.Fatalf("%s %v: %v\n%s%s", name, args, err, out.String(), errb.String())
	}
	return out.String()
}

// pseudoRandom returns n deterministic incompressible bytes.
func pseudoRandom(seed int64, n int) []byte {
	b := make([]byte, n)
	rand.New(rand.NewSource(seed)).Read(b)
	return b
}

func textBytes(n int) []byte {
	var b bytes.Buffer
	for i := 0; b.Len() < n; i++ {
		fmt.Fprintf(&b, "line %d of a synthetic test file\n", i)
	}
	return b.Bytes()[:n]
}

// makeTree creates a synthetic source tree (no device nodes: those need pseudo definitions).
func makeTree(t *testing.T, root string) {
	t.Helper()
	w := func(rel string, data []byte, mode fs.FileMode) {
		p := filepath.Join(root, rel)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, data, mode); err != nil {
			t.Fatal(err)
		}
		if err := os.Chmod(p, mode); err != nil {
			t.Fatal(err)
		}
	}
	w("empty", nil, 0o644)
	w("one", []byte("x"), 0o600)
	w("bin/tool", textBytes(1000), 0o755)
	w("bin/suid", textBytes(5000), 0o4755)
	w("data/exact128k", textBytes(131072), 0o644)
	w("data/just_over", textBytes(131073), 0o644)
	w("data/three_blocks", pseudoRandom(1, 3*131072+777), 0o640)
	w("data/zeros", make([]byte, 400000), 0o644)
	w("data/holey", append(append(make([]byte, 200000), textBytes(3000)...), make([]byte, 140000)...), 0o644)
	w("data/random_small", pseudoRandom(2, 4000), 0o644)
	w("a b/space name.txt", []byte("name with spaces\n"), 0o644)
	w("long/"+strings.Repeat("n", 255), []byte("longest name\n"), 0o644)
	if err := os.MkdirAll(filepath.Join(root, "emptydir"), 0o750); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "sticky"), 0o1777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(filepath.Join(root, "sticky"), 0o1777); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 700; i++ { // a directory that needs several metadata blocks and a directory index
		w(fmt.Sprintf("many/file_with_a_rather_long_name_%04d.txt", i), []byte(fmt.Sprintf("file %d\n", i)), 0o644)
	}
	for _, l := range [][2]string{{"link_rel", "bin/tool"}, {"link_abs", "/etc/passwd"}, {"dangling", "nowhere"}, {"loop_a", "loop_b"}, {"loop_b", "loop_a"}, {"bin/long_link", strings.Repeat("d/", 1000)}} {
		if err := os.Symlink(l[1], filepath.Join(root, l[0])); err != nil {
			t.Skipf("no symlinks on this host: %v", err)
		}
	}
	if err := os.Link(filepath.Join(root, "bin/tool"), filepath.Join(root, "bin/tool_hardlink")); err != nil {
		t.Fatal(err)
	}
	if err := exec.Command("mkfifo", filepath.Join(root, "pipe")).Run(); err == nil {
		_ = os.Chmod(filepath.Join(root, "pipe"), 0o640)
	}
	// distinct mtimes
	i := 0
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err == nil && d.Type()&fs.ModeSymlink == 0 {
			i++
			ts := time.Unix(1_600_000_000+int64(i)*7919, 0)
			_ = os.Chtimes(p, ts, ts)
		}
		return nil
	})
}

var lsRe = regexp.MustCompile(`^(\S{10}) (\d+)/(\d+) +(\d+(?:, *\d+)?) (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)$`)

type lsLine struct {
	mode, uid, gid, size, when, rest string
}

func parseLS(t *testing.T, out string) map[string]lsLine {
	t.Helper()
	m := map[string]lsLine{}
	for _, ln := range strings.Split(strings.TrimRight(out, "\n"), "\n") {
		g := lsRe.FindStringSubmatch(ln)
		if g == nil {
			t.Fatalf("unparsable unsquashfs line %q", ln)
		}
		rest := g[6]
		link := ""
		if g[1][0] == 'l' {
			if i := strings.Index(rest, " -> "); i >= 0 {
				rest, link = rest[:i], rest[i+4:]
			}
		}
		p := strings.TrimPrefix(rest, "squashfs-root")
		if p == "" {
			p = "/"
		}
		m[p] = lsLine{g[1], g[2], g[3], strings.ReplaceAll(g[4], " ", ""), g[5], link}
	}
	return m
}

func modeString(e ListEntry) string {
	b := []byte("----------")
	b[0] = "dflbcps"[e.Type-1]
	if b[0] == 'f' {
		b[0] = '-'
	}
	perm := "rwxrwxrwx"
	for i := 0; i < 9; i++ {
		if e.Mode&(1<<(8-i)) != 0 {
			b[1+i] = perm[i]
		}
	}
	if e.Mode&0o4000 != 0 {
		b[3] = map[byte]byte{'x': 's', '-': 'S'}[b[3]]
	}
	if e.Mode&0o2000 != 0 {
		b[6] = map[byte]byte{'x': 's', '-': 'S'}[b[6]]
	}
	if e.Mode&0o1000 != 0 {
		b[9] = map[byte]byte{'x': 't', '-': 'T'}[b[9]]
	}
	return string(b)
}

// compareToUnsquashfs checks the canonical listing line by line against `unsquashfs -lln`.
func compareToUnsquashfs(t *testing.T, img string, f *FS) []ListEntry {
	t.Helper()
	out := run(t, "unsquashfs", "-lln", "-full-precision", "-UTC", img)
	want := parseLS(t, out)
	got, err := f.Listing()
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(want) {
		t.Fatalf("listing has %d entries, unsquashfs %d", len(got), len(want))
	}
	for _, e := range got {
		w, ok := want[e.Path]
		if !ok {
			t.Fatalf("%s is not in unsquashfs output", e.Path)
		}
		size := strconv.FormatInt(e.Size, 10)
		switch e.Type {
		case TypeDir:
			size = w.size // the size of a directory listing is layout dependent, unsquashfs prints it
		case TypeBlock, TypeChar:
			size = fmt.Sprintf("%d,%d", e.Major, e.Minor)
		case TypeFifo, TypeSocket:
			size = "0"
		}
		when := time.Unix(e.MTime, 0).UTC().Format("2006-01-02 15:04:05")
		have := fmt.Sprintf("%s %d/%d %s %s %s", modeString(e), e.UID, e.GID, size, when, e.Target)
		exp := fmt.Sprintf("%s %s/%s %s %s %s", w.mode, w.uid, w.gid, w.size, w.when, w.rest)
		if have != exp {
			t.Errorf("%s:\n  reader      %s\n  unsquashfs  %s", e.Path, have, exp)
		}
	}
	return got
}

// compareToExtraction extracts with unsquashfs and compares content, link targets and modes.
func compareToExtraction(t *testing.T, img string, f *FS) {
	t.Helper()
	dst := filepath.Join(t.TempDir(), "x")
	run(t, "unsquashfs", "-no-xattrs", "-f", "-d", dst, img)
	var n int
	err := f.Walk("/", func(fi FileInfo) error {
		n++
		host := filepath.Join(dst, filepath.FromSlash(fi.Path))
		st, err := os.Lstat(host)
		if err != nil {
			return err
		}
		switch fi.Type {
		case TypeFile:
			if !st.Mode().IsRegular() {
				t.Errorf("%s: extracted as %v", fi.Path, st.Mode())
				return nil
			}
			a, _ := os.ReadFile(host)
			b, err := f.ReadFile(fi.Path)
			if err != nil {
				return err
			}
			if !bytes.Equal(a, b) {
				t.Errorf("%s: content differs from the extraction (%d vs %d bytes)", fi.Path, len(a), len(b))
			}
			ha, hb := sha256.Sum256(a), sha256.Sum256(b)
			if ha != hb {
				t.Errorf("%s: hash %s vs %s", fi.Path, hex.EncodeToString(ha[:]), hex.EncodeToString(hb[:]))
			}
			if int64(len(a)) != fi.Size {
				t.Errorf("%s: size %d, extracted %d", fi.Path, fi.Size, len(a))
			}
		case TypeSymlink:
			l, err := os.Readlink(host)
			if err != nil || l != fi.Target {
				t.Errorf("%s: link %q (%v), reader %q", fi.Path, l, err, fi.Target)
			}
			if r, err := f.Readlink(fi.Path); err != nil || r != fi.Target {
				t.Errorf("Readlink(%s) = %q, %v", fi.Path, r, err)
			}
		case TypeDir:
			if !st.IsDir() {
				t.Errorf("%s: not a directory", fi.Path)
			}
		}
		if fi.Type != TypeSymlink && fi.Mode&0o777 != uint16(st.Mode().Perm()) {
			t.Errorf("%s: mode %o, extracted %o", fi.Path, fi.Mode&0o777, st.Mode().Perm())
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if n < 700 {
		t.Fatalf("walked only %d entries", n)
	}
}

func TestAgainstMksquashfsAndUnsquashfs(t *testing.T) {
	needTools(t)
	src := filepath.Join(t.TempDir(), "src")
	makeTree(t, src)
	cases := []struct {
		name string
		args []string
	}{
		{"xz-128k", []string{"-comp", "xz", "-b", "131072"}},
		{"xz-crc32-dict-128k", []string{"-comp", "xz", "-b", "131072", "-Xdict-size", "131072"}},
		{"xz-4k-blocks", []string{"-comp", "xz", "-b", "4096"}},
		{"xz-1m-blocks", []string{"-comp", "xz", "-b", "1M"}},
		{"gzip-128k", []string{"-comp", "gzip", "-b", "131072"}},
		{"gzip-8k-all-root", []string{"-comp", "gzip", "-b", "8192", "-all-root"}},
		{"stored-everything", []string{"-comp", "gzip", "-no-compression"}},
		{"stored-data-only", []string{"-comp", "xz", "-noD"}},
		{"no-fragments", []string{"-comp", "xz", "-no-fragments"}},
		{"always-fragments", []string{"-comp", "gzip", "-always-use-fragments"}},
		{"no-duplicates", []string{"-comp", "xz", "-no-duplicates"}},
		{"not-exportable", []string{"-comp", "xz", "-no-exports"}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			img := filepath.Join(t.TempDir(), "i.sqfs")
			args := append([]string{src, img, "-noappend", "-no-xattrs", "-quiet", "-processors", "1",
				"-p", "bin/tool m 755 1000 100", "-p", "data/zeros m 640 33 33", "-p", "pipe m 600 1000 33",
				"-p", "dev_null c 666 0 0 1 3", "-p", "dev_sda b 660 0 6 8 0", "-p", "dev_big c 600 5 5 259 70",
				"-p", "dir_made d 700 7 8"}, c.args...)
			run(t, "mksquashfs", args...)
			raw, err := os.ReadFile(img)
			if err != nil {
				t.Fatal(err)
			}
			f, err := Open(bytes.NewReader(raw), int64(len(raw)))
			if err != nil {
				t.Fatal(err)
			}
			if err := f.Verify(); err != nil {
				t.Fatalf("Verify: %v", err)
			}
			list := compareToUnsquashfs(t, img, f)
			var sawDev, sawFifo, sawLong bool
			for _, e := range list {
				sawDev = sawDev || e.Type == TypeChar
				sawFifo = sawFifo || e.Type == TypeFifo
				sawLong = sawLong || len(e.Path) > 255
			}
			if !sawDev || !sawLong {
				t.Errorf("fixture lacks a device or a long name: dev=%v long=%v fifo=%v", sawDev, sawLong, sawFifo)
			}
			// extraction needs mknod rights for the pseudo devices: build a second image without them
			img2 := filepath.Join(t.TempDir(), "j.sqfs")
			args2 := append([]string{src, img2, "-noappend", "-no-xattrs", "-quiet", "-processors", "1", "-all-root"}, c.args...)
			run(t, "mksquashfs", args2...)
			raw2, _ := os.ReadFile(img2)
			f2, err := Open(bytes.NewReader(raw2), int64(len(raw2)))
			if err != nil {
				t.Fatal(err)
			}
			compareToExtraction(t, img2, f2)
		})
	}
}

func TestAppFakeFixtures(t *testing.T) {
	needTools(t)
	for _, name := range []string{"app_fake", "app_fake_variants/a", "app_fake_variants/b", "app_fake_variants/c", "app_fake_variants/d"} {
		src := filepath.Join("..", "..", "..", "..", "tests", "fixtures", filepath.FromSlash(name))
		if _, err := os.Stat(src); err != nil {
			t.Skipf("fixture %s: %v", name, err)
		}
		for _, comp := range []string{"xz", "gzip"} {
			t.Run(name+"/"+comp, func(t *testing.T) {
				img := filepath.Join(t.TempDir(), "i.sqfs")
				run(t, "mksquashfs", src, img, "-noappend", "-no-xattrs", "-quiet", "-processors", "1", "-comp", comp, "-b", "131072", "-all-root", "-all-time", "1700000000")
				raw, _ := os.ReadFile(img)
				f, err := Open(bytes.NewReader(raw), int64(len(raw)))
				if err != nil {
					t.Fatal(err)
				}
				if err := f.Verify(); err != nil {
					t.Fatal(err)
				}
				compareToUnsquashfs(t, img, f)
				dst := filepath.Join(t.TempDir(), "x")
				run(t, "unsquashfs", "-no-xattrs", "-f", "-d", dst, img)
				if err := f.Walk("/", func(fi FileInfo) error {
					if fi.Type != TypeFile {
						return nil
					}
					a, _ := os.ReadFile(filepath.Join(dst, filepath.FromSlash(fi.Path)))
					b, err := f.ReadFile(fi.Path)
					if err != nil || !bytes.Equal(a, b) {
						t.Errorf("%s differs from the extraction (%v)", fi.Path, err)
					}
					// and from the source tree
					s, err := os.ReadFile(filepath.Join(src, filepath.FromSlash(fi.Path)))
					if err != nil || !bytes.Equal(s, b) {
						t.Errorf("%s differs from the source (%v)", fi.Path, err)
					}
					return nil
				}); err != nil {
					t.Fatal(err)
				}
			})
		}
	}
}
