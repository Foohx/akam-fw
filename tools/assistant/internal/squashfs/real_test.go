package squashfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

// realLine is the comparable part of one line of `unsquashfs -lln -full-precision -UTC`.
type realLine struct {
	mode, uid, gid, size, when string
}

var realLsRe = regexp.MustCompile(`^(\S{10}) (\d+)/(\d+) +(\d+(?:, *\d+)?) (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)$`)

// TestRealImageAgainstUnsquashfs is the opt-in check of the reader on a real image (Q1 gate step 1: the
// reader matches `unsquashfs -lls` and the extraction hashes). It runs only when AKAM_SQFS_REAL names an
// image and unsquashfs is installed:
//
//	AKAM_SQFS_REAL=/path/to/image go test -count=1 -v -run TestRealImageAgainstUnsquashfs ./internal/squashfs
//
// (from tools/assistant). The image is read in place, never copied. The test writes only into its own
// temporary directory (removed afterwards) and never prints file contents: the output holds counts and, on
// a mismatch, entry paths and metadata.
func TestRealImageAgainstUnsquashfs(t *testing.T) {
	img := os.Getenv("AKAM_SQFS_REAL")
	if img == "" {
		t.Skip("AKAM_SQFS_REAL is not set")
	}
	if _, err := exec.LookPath("unsquashfs"); err != nil {
		t.Skip("unsquashfs is not installed")
	}
	st, err := os.Stat(img)
	if err != nil {
		t.Fatalf("cannot stat the image: %v", err)
	}
	fh, err := os.Open(img)
	if err != nil {
		t.Fatalf("cannot open the image: %v", err)
	}
	defer fh.Close()
	// A dump may carry padding after bytes_used: the size limit is raised to the file size.
	f, err := OpenLimits(fh, st.Size(), Limits{MaxImageSize: st.Size()})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if err := f.Verify(); err != nil {
		t.Fatalf("Verify: %v", err)
	}
	got, err := f.Listing()
	if err != nil {
		t.Fatalf("Listing: %v", err)
	}
	if err := f.Walk("/", func(fi FileInfo) error {
		if fi.Type == TypeBlock || fi.Type == TypeChar {
			if back := fi.Rdev&0xff | fi.Major<<8 | (fi.Minor&^0xff)<<12; back != fi.Rdev {
				t.Errorf("%s: device number 0x%x does not round-trip (%d,%d)", fi.Path, fi.Rdev, fi.Major, fi.Minor)
			}
		}
		return nil
	}); err != nil {
		t.Fatalf("Walk: %v", err)
	}

	// 1. the listing against unsquashfs -lln
	var out, errb bytes.Buffer
	cmd := exec.Command("unsquashfs", "-lln", "-full-precision", "-UTC", img)
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		t.Fatalf("unsquashfs -lln: %v\n%s", err, errb.String())
	}
	want := map[string]realLine{}
	for _, ln := range strings.Split(strings.TrimRight(out.String(), "\n"), "\n") {
		g := realLsRe.FindStringSubmatch(ln)
		if g == nil {
			t.Fatalf("unparsable unsquashfs line of %d bytes", len(ln))
		}
		if _, dup := want[g[6]]; dup {
			t.Fatalf("unsquashfs printed %q twice", g[6])
		}
		want[g[6]] = realLine{g[1], g[2], g[3], strings.ReplaceAll(g[4], " ", ""), g[5]}
	}
	if len(want) != len(got) {
		t.Errorf("the reader lists %d entries, unsquashfs %d", len(got), len(want))
	}
	compared := 0
	for _, e := range got {
		key := "squashfs-root" + strings.TrimSuffix(e.Path, "/")
		if e.Type == TypeSymlink {
			key += " -> " + e.Target
		}
		w, ok := want[key]
		if !ok {
			t.Errorf("%s (type %v) is not in the unsquashfs output", e.Path, e.Type)
			continue
		}
		size := fmt.Sprint(e.Size)
		switch e.Type {
		case TypeDir:
			size = w.size // the stored size of a directory is layout dependent: unsquashfs prints it
		case TypeBlock, TypeChar:
			size = fmt.Sprintf("%d,%d", e.Major, e.Minor)
		case TypeFifo, TypeSocket:
			size = "0"
		}
		when := time.Unix(e.MTime, 0).UTC().Format("2006-01-02 15:04:05")
		have := realLine{modeString(e), fmt.Sprint(e.UID), fmt.Sprint(e.GID), size, when}
		if have != w {
			t.Errorf("%s:\n  reader     %+v\n  unsquashfs %+v", e.Path, have, w)
		}
		compared++
		delete(want, key)
	}
	for k := range want {
		t.Errorf("unsquashfs lists %q, the reader does not", k)
	}

	// 2. the extraction hashes, in a temporary directory removed afterwards
	tmp := t.TempDir()
	dst := filepath.Join(tmp, "x")
	defer func() { // directories may have been extracted without write permission: make the tree removable
		_ = filepath.WalkDir(tmp, func(p string, d os.DirEntry, err error) error {
			if d != nil && d.IsDir() {
				_ = os.Chmod(p, 0o700)
			}
			return nil
		})
	}()
	xc := exec.Command("unsquashfs", "-no-xattrs", "-d", dst, img)
	xc.Stdout, xc.Stderr = io.Discard, io.Discard
	if err := xc.Run(); err != nil {
		// device nodes and ownership need privileges: only the regular files and links are checked
		t.Logf("unsquashfs -d exited with %v (expected without root when the image holds device nodes)", err)
	}
	hashed := 0
	for _, e := range got { // sorted by path: a directory comes before its content
		host := filepath.Join(dst, filepath.FromSlash(e.Path))
		switch e.Type {
		case TypeDir:
			_ = os.Chmod(host, 0o700)
		case TypeFile:
			_ = os.Chmod(host, 0o600)
			xf, err := os.Open(host)
			if err != nil {
				t.Errorf("%s: not extracted: %v", e.Path, err)
				continue
			}
			h := sha256.New()
			n, err := io.Copy(h, xf)
			xf.Close()
			if err != nil {
				t.Errorf("%s: reading the extraction: %v", e.Path, err)
				continue
			}
			if n != e.Size {
				t.Errorf("%s: extracted %d bytes, the reader says %d", e.Path, n, e.Size)
			}
			if sum := hex.EncodeToString(h.Sum(nil)); sum != e.SHA256 {
				t.Errorf("%s: extraction sha256 %s, reader %s", e.Path, sum, e.SHA256)
			}
			hashed++
		case TypeSymlink:
			if l, err := os.Readlink(host); err != nil || l != e.Target {
				t.Errorf("%s: extracted link target differs (%v)", e.Path, err)
			}
		}
	}
	t.Logf("real image: %d bytes, %s, %d entries compared with unsquashfs -lln, %d regular files hashed against the extraction",
		st.Size(), f.Info().CompressionName(), compared, hashed)
}

// TestPaddedImageAgainstUnsquashfs exercises the real-image test on a synthetic image with trailing 0xFF padding, as in a
// partition dump, without needing a private file (opt-in only through the same tools being installed).
func TestPaddedImageAgainstUnsquashfs(t *testing.T) {
	needTools(t)
	src := filepath.Join(t.TempDir(), "src")
	makeTree(t, src)
	img := filepath.Join(t.TempDir(), "i.sqfs")
	run(t, "mksquashfs", src, img, "-noappend", "-no-xattrs", "-quiet", "-processors", "1", "-comp", "xz", "-b", "131072", "-all-root")
	raw, err := os.ReadFile(img)
	if err != nil {
		t.Fatal(err)
	}
	padded := append(append([]byte{}, raw...), bytes.Repeat([]byte{0xff}, 4096*3+17)...)
	f, err := Open(bytes.NewReader(padded), int64(len(padded)))
	if err != nil {
		t.Fatalf("padding after bytes_used refused: %v", err)
	}
	if err := f.Verify(); err != nil {
		t.Fatal(err)
	}
	// unsquashfs accepts the padded image too
	pimg := filepath.Join(t.TempDir(), "p.sqfs")
	if err := os.WriteFile(pimg, padded, 0o600); err != nil {
		t.Fatal(err)
	}
	compareToUnsquashfs(t, pimg, f)
}
