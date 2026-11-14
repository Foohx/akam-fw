package squashfs

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// An image whose inodes carry extended attributes is refused by Tree (ErrUnsupported) and read by TreeWith
// with DropXattrs, which reports what it dropped; the rewritten image has the same listing and no xattrs.
func TestTreeDropXattrs(t *testing.T) {
	needTools(t)
	if _, err := exec.LookPath("setfattr"); err != nil {
		t.Skip("setfattr is not installed")
	}
	src := filepath.Join(t.TempDir(), "src")
	if err := os.MkdirAll(filepath.Join(src, "d"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(src, "d", "f"), []byte("content"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(src, "plain"), []byte("no attributes"), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{src, filepath.Join(src, "d", "f")} {
		if out, err := exec.Command("setfattr", "-n", "user.akam", "-v", "x", p).CombinedOutput(); err != nil {
			t.Skipf("no user xattrs here: %v %s", err, out)
		}
	}
	img := filepath.Join(t.TempDir(), "i.sqfs")
	run(t, "mksquashfs", src, img, "-noappend", "-quiet", "-no-progress", "-processors", "1", "-comp", "xz", "-all-root")
	raw, _ := os.ReadFile(img)
	f := openImage(t, raw)
	if _, err := f.Tree(); !errors.Is(err, ErrUnsupported) {
		t.Fatalf("Tree of an image with xattrs: %v", err)
	}
	tr, rep, err := f.TreeWith(TreeOptions{})
	if tr != nil || !errors.Is(err, ErrUnsupported) || rep.DroppedXattrs != 0 {
		t.Fatalf("TreeWith without DropXattrs: %v %+v", err, rep)
	}
	tr, rep, err = f.TreeWith(TreeOptions{DropXattrs: true})
	if err != nil {
		t.Fatal(err)
	}
	if rep.DroppedXattrs != 2 || len(rep.DroppedXattrPaths) != 2 || rep.DroppedXattrPaths[0] != "/" || rep.DroppedXattrPaths[1] != "/d/f" {
		t.Errorf("report: %+v", rep)
	}
	out, _, err := Build(tr, Options{Time: f.Info().MkfsTime})
	if err != nil {
		t.Fatal(err)
	}
	g := openImage(t, out)
	if g.Info().HasXattrTable {
		t.Error("the rewritten image has an xattr table")
	}
	want, err := f.Listing()
	if err != nil {
		t.Fatal(err)
	}
	got, err := g.Listing()
	if err != nil {
		t.Fatal(err)
	}
	diffListings(t, "rewritten listing", got, want)
}

// MaxSize is enforced while the image is produced: a tree of incompressible data far above the cap fails
// with ErrLimit, one byte below the image fails, and the cap equal to the image goes through.
func TestWriteMaxSize(t *testing.T) {
	root := dirN("", 0o755)
	files := 40
	if raceEnabled {
		files = 10 // still 3 MB of incompressible data, far above MaxSize: see race_on_test.go
	}
	for i := 0; i < files; i++ {
		root.Add(fileN(fmt.Sprintf("f%02d", i), 0o644, pseudoRandom(int64(i), 300000)))
	}
	if _, _, err := Build(root, Options{MaxSize: 100000}); !errors.Is(err, ErrLimit) {
		t.Fatalf("MaxSize far below the data: %v", err)
	}
	img, st, err := Build(root, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := Build(root, Options{MaxSize: st.Size - 1}); !errors.Is(err, ErrLimit) {
		t.Errorf("MaxSize one below the image: %v", err)
	}
	again, _, err := Build(root, Options{MaxSize: st.Size})
	if err != nil || !bytes.Equal(again, img) {
		t.Errorf("MaxSize equal to the image: %v", err)
	}
}

// Untrusted nodes: bad names, NUL in a link target, bad modes, duplicates and a hard link are refused.
func TestWriteRefusesBadNodes(t *testing.T) {
	bad := map[string]*Node{
		"slash":   dirN("", 0o755, fileN("a/b", 0o644, nil)),
		"nul":     dirN("", 0o755, fileN("a\x00b", 0o644, nil)),
		"dot":     dirN("", 0o755, fileN(".", 0o644, nil)),
		"dotdot":  dirN("", 0o755, fileN("..", 0o644, nil)),
		"empty":   dirN("", 0o755, fileN("", 0o644, nil)),
		"long":    dirN("", 0o755, fileN(strings.Repeat("a", 257), 0o644, nil)),
		"dup":     dirN("", 0o755, fileN("a", 0o644, nil), fileN("a", 0o644, nil)),
		"mode":    dirN("", 0o755, fileN("a", 0o10644, nil)),
		"nullink": dirN("", 0o755, NewSymlink("l", "a\x00b")),
	}
	for name, root := range bad {
		if _, _, err := Build(root, Options{}); !errors.Is(err, ErrInvalidTree) {
			t.Errorf("%s: %v", name, err)
		}
	}
	shared := fileN("a", 0o644, []byte("x"))
	if _, _, err := Build(dirN("", 0o755, shared, dirN("d", 0o755, shared)), Options{}); !errors.Is(err, ErrHardLink) {
		t.Errorf("hard link: %v", err)
	}
	if _, _, err := Build(dirN("", 0o755, fileN(strings.Repeat("a", 256), 0o644, nil)), Options{}); err != nil {
		t.Errorf("name of 256 bytes: %v", err)
	}
}
