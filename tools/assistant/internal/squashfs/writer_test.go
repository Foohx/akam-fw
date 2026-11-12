package squashfs

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testTime = 1700000000

func ownersTree() *Node {
	f := fileN("f", 0o644, textBytes(500))
	f.UID, f.GID = 1000, 100
	d := dirN("d", 0o750, fileN("g", 0o600, []byte("g")))
	d.UID, d.GID, d.MTime = 33, 33, 1234567
	l := NewSymlink("l", "f")
	l.UID, l.MTime = 7, 99
	root := dirN("", 0o755, f, d, l)
	root.UID, root.GID, root.MTime = 5, 6, 1111
	return root
}

func roundTripTrees() []treeCase {
	cases := diffTrees()
	cases = append(cases,
		treeCase{"zero-small", zeroSmallTree()},
		treeCase{"long-names-256", longNamesTree(256)},
		treeCase{"owners", ownersTree()},
	)
	return cases
}

func reverseChildren(n *Node) *Node {
	c := *n
	c.Children = nil
	for i := len(n.Children) - 1; i >= 0; i-- {
		c.Children = append(c.Children, reverseChildren(n.Children[i]))
	}
	return &c
}

// Gate steps 2 to 4 without external tools: every tree is written, read back by the reader (Verify), its
// canonical listing equals the one computed from the input, the tables are laid out as the kernel expects,
// every xz block is a CRC32 stream with a dictionary the kernel accepts, the output is deterministic
// (children order does not matter) and padded to 4096.
func TestWriterRoundTrip(t *testing.T) {
	for _, c := range roundTripTrees() {
		t.Run(c.name, func(t *testing.T) {
			o := Options{Time: testTime, AllTime: c.name != "owners"}
			img, st, err := Build(c.root, o)
			if err != nil {
				t.Fatal(err)
			}
			if len(img)%4096 != 0 || int64(len(img)) != st.Size {
				t.Errorf("size %d (stats %d) is not a multiple of 4096", len(img), st.Size)
			}
			f := openImage(t, img)
			if err := f.Verify(); err != nil {
				t.Fatalf("Verify: %v", err)
			}
			got, err := f.Listing()
			if err != nil {
				t.Fatal(err)
			}
			diffListings(t, "listing", got, treeListing(c.root, o))
			in := f.Info()
			if in.Compression != CompXZ || in.BlockSize != 131072 || in.Flags != 0x2c0 || !in.HasExportTable || in.HasXattrTable ||
				in.MkfsTime != testTime || in.XzDictSize != 0 || in.Inodes != st.Inodes || in.Fragments != st.Fragments {
				t.Errorf("superblock %+v", in)
			}
			blocks := collectXZ(t, img, f)
			for _, b := range blocks {
				checkXZParameters(t, b, 131072)
			}
			again, _, err := Build(reverseChildren(c.root), o)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(img, again) {
				t.Errorf("not deterministic (second build, children reversed): %s", firstDiff(img, again))
			}
			// the tree read back from the image writes the same bytes again
			tr, err := f.Tree()
			if err != nil {
				t.Fatal(err)
			}
			third, _, err := Build(tr, Options{Time: testTime, AllTime: o.AllTime})
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(img, third) {
				t.Errorf("Tree() then Build differs: %s", firstDiff(img, third))
			}
		})
	}
}

// Gate step 4: every xz block of the images decodes with the system xz, and the concatenation of the
// decoded blocks is what the reader decoded.
func TestWriterBlocksDecodeWithSystemXZ(t *testing.T) {
	for _, c := range []string{"tiny", "edge-sizes", "many-fragments", "dir-700-long-names", "random-2"} {
		for _, tc := range diffTrees() {
			if tc.name != c {
				continue
			}
			t.Run(c, func(t *testing.T) {
				img, _, err := Build(tc.root, Options{Time: testTime, AllTime: true})
				if err != nil {
					t.Fatal(err)
				}
				systemXZ(t, collectXZ(t, img, openImage(t, img)))
			})
		}
	}
}

func TestWriterOptions(t *testing.T) {
	root := dirN("", 0o755, fileN("a", 0o644, textBytes(20000)), fileN("b", 0o644, pseudoRandom(1, 10000)))
	for _, bs := range []int{4096, 8192, 65536, 131072, 1 << 20} {
		img, _, err := Build(root, Options{BlockSize: bs, Time: 1})
		if err != nil {
			t.Fatalf("block size %d: %v", bs, err)
		}
		f := openImage(t, img)
		if err := f.Verify(); err != nil {
			t.Fatalf("block size %d: %v", bs, err)
		}
		if f.Info().BlockSize != uint32(bs) {
			t.Errorf("block size %d, got %d", bs, f.Info().BlockSize)
		}
		for _, b := range collectXZ(t, img, f) {
			checkXZParameters(t, b, int64(max(bs, 8192)))
		}
	}
	for _, bs := range []int{1, 4095, 6000, 2 << 20} {
		if _, _, err := Build(root, Options{BlockSize: bs}); err == nil {
			t.Errorf("block size %d accepted", bs)
		}
	}
	img, _, _ := Build(root, Options{Time: 1})
	raw, _, _ := Build(root, Options{Time: 1, NoPad: true})
	if len(raw)%4096 == 0 && len(raw) == len(img) {
		t.Log("image happens to be a multiple of 4096")
	}
	if !bytes.Equal(raw, img[:len(raw)]) || int(parseRawSB(raw).bytesUsed) != len(raw) {
		t.Errorf("NoPad image is not the unpadded prefix")
	}
	if _, _, err := Build(root, Options{Time: 1, MaxSize: 4096}); !errors.Is(err, ErrLimit) {
		t.Errorf("MaxSize: %v", err)
	}
	var w bytes.Buffer
	if _, err := Write(&w, root, Options{Time: 1}); err != nil || !bytes.Equal(w.Bytes(), img) {
		t.Errorf("Write differs from Build (%v)", err)
	}
	// per-inode times without AllTime
	n := dirN("", 0o755, fileN("a", 0o644, nil))
	n.MTime, n.Children[0].MTime = 10, 20
	im, _, _ := Build(n, Options{Time: 99})
	l, _ := openImage(t, im).Listing()
	if l[0].MTime != 10 || l[1].MTime != 20 || parseRawSB(im).mkfsTime != 99 {
		t.Errorf("times %v %v mkfs %d", l[0].MTime, l[1].MTime, parseRawSB(im).mkfsTime)
	}
}

func TestWriterRefusesBadTrees(t *testing.T) {
	shared := fileN("x", 0o644, []byte("x"))
	longName := strings.Repeat("n", 257)
	cases := []struct {
		name string
		root *Node
		want error
	}{
		{"hard link", dirN("", 0o755, dirN("a", 0o755, shared), dirN("b", 0o755, &Node{Name: "y", Type: TypeFile, Data: nil}, shared)), ErrHardLink},
		{"same dir twice", func() *Node {
			d := dirN("d", 0o755)
			return dirN("", 0o755, d, &Node{Name: "e", Type: TypeDir, Children: []*Node{d}})
		}(), ErrHardLink},
		{"root is a file", fileN("", 0o644, nil), ErrInvalidTree},
		{"nil root", nil, ErrInvalidTree},
		{"empty name", dirN("", 0o755, fileN("", 0o644, nil)), ErrInvalidTree},
		{"dot", dirN("", 0o755, fileN(".", 0o644, nil)), ErrInvalidTree},
		{"dotdot", dirN("", 0o755, fileN("..", 0o644, nil)), ErrInvalidTree},
		{"slash", dirN("", 0o755, fileN("a/b", 0o644, nil)), ErrInvalidTree},
		{"nul", dirN("", 0o755, fileN("a\x00b", 0o644, nil)), ErrInvalidTree},
		{"name of 257", dirN("", 0o755, fileN(longName, 0o644, nil)), ErrInvalidTree},
		{"duplicate", dirN("", 0o755, fileN("a", 0o644, nil), NewSymlink("a", "b")), ErrInvalidTree},
		{"nil child", dirN("", 0o755, nil), ErrInvalidTree},
		{"mode with type bits", dirN("", 0o755, fileN("a", 0o100644, nil)), ErrInvalidTree},
		{"empty symlink", dirN("", 0o755, NewSymlink("a", "")), ErrInvalidTree},
		{"symlink 4097", dirN("", 0o755, NewSymlink("a", strings.Repeat("t", 4097))), ErrInvalidTree},
		{"device major", dirN("", 0o755, NewDevice("a", TypeChar, 0o600, 4096, 0)), ErrInvalidTree},
		{"device minor", dirN("", 0o755, NewDevice("a", TypeBlock, 0o600, 1, 1<<20)), ErrInvalidTree},
		{"bad type", dirN("", 0o755, &Node{Name: "a", Type: 9}), ErrInvalidTree},
		{"children on a file", dirN("", 0o755, &Node{Name: "a", Type: TypeFile, Children: []*Node{fileN("b", 0o644, nil)}}), ErrInvalidTree},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, _, err := Build(c.root, Options{}); !errors.Is(err, c.want) {
				t.Errorf("got %v, want %v", err, c.want)
			}
		})
	}
	// 255 is the longest name mksquashfs can read from a host; the format itself allows 256
	if _, _, err := Build(dirN("", 0o755, fileN(strings.Repeat("n", 256), 0o644, nil)), Options{}); err != nil {
		t.Errorf("name of 256: %v", err)
	}
}

// A tree with more distinct owners than the id table can hold is refused, not truncated.
func TestWriterTooManyIDs(t *testing.T) {
	root := dirN("", 0o755)
	for i := 0; i < 65537; i++ {
		f := fileN(fmt.Sprintf("f%d", i), 0o644, nil)
		f.UID = uint32(i)
		root.Add(f)
	}
	if _, _, err := Build(root, Options{}); !errors.Is(err, ErrLimit) {
		t.Errorf("got %v", err)
	}
}

// FS.Tree refuses what the writer cannot represent: the hard links of the images mksquashfs builds from a
// tree that has some.
func TestTreeRefusesHardLinks(t *testing.T) {
	needTools(t)
	src := filepath.Join(t.TempDir(), "src")
	if err := os.MkdirAll(filepath.Join(src, "d"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(src, "a"), []byte("same inode"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Link(filepath.Join(src, "a"), filepath.Join(src, "d", "b")); err != nil {
		t.Skipf("no hard links: %v", err)
	}
	img := filepath.Join(t.TempDir(), "i.sqfs")
	run(t, "mksquashfs", src, img, "-noappend", "-no-xattrs", "-quiet", "-no-progress", "-processors", "1", "-comp", "xz", "-all-root")
	raw, _ := os.ReadFile(img)
	f := openImage(t, raw)
	if _, err := f.Tree(); !errors.Is(err, ErrHardLink) {
		t.Errorf("Tree of an image with a hard link: %v", err)
	}
}

// Rewriting a real mksquashfs image keeps the tree, and the second image equals the first as a tree.
func TestRewriteMksquashfsImage(t *testing.T) {
	needTools(t)
	src := filepath.Join(t.TempDir(), "src")
	makeTree(t, src)
	if err := os.Remove(filepath.Join(src, "bin", "tool_hardlink")); err != nil {
		t.Fatal(err)
	}
	img := filepath.Join(t.TempDir(), "i.sqfs")
	run(t, "mksquashfs", src, img, "-noappend", "-no-xattrs", "-quiet", "-no-progress", "-processors", "1", "-comp", "xz", "-b", "131072", "-all-root",
		"-p", "dev_null c 666 0 0 1 3", "-p", "dev_sda b 660 0 6 8 0")
	raw, _ := os.ReadFile(img)
	f := openImage(t, raw)
	want, err := f.Listing()
	if err != nil {
		t.Fatal(err)
	}
	tr, err := f.Tree()
	if err != nil {
		t.Fatal(err)
	}
	out, _, err := Build(tr, Options{Time: f.Info().MkfsTime})
	if err != nil {
		t.Fatal(err)
	}
	g := openImage(t, out)
	if err := g.Verify(); err != nil {
		t.Fatal(err)
	}
	got, err := g.Listing()
	if err != nil {
		t.Fatal(err)
	}
	diffListings(t, "rewritten listing", got, want)
	// and unsquashfs reads it like the original
	imgOut := writeTemp(t, "o.sqfs", out)
	a := unsquashfsText(t, "-lln", "-full-precision", "-UTC", img)
	b := unsquashfsText(t, "-lln", "-full-precision", "-UTC", imgOut)
	if a != b {
		t.Errorf("unsquashfs -lln differs between the original and the rewritten image")
	}
}

func treeFromDir(t testing.TB, dir string, mtime uint32) *Node {
	t.Helper()
	var rec func(p, name string) *Node
	rec = func(p, name string) *Node {
		st, err := os.Lstat(p)
		if err != nil {
			t.Fatal(err)
		}
		mode := uint16(st.Mode().Perm())
		if st.Mode()&fs.ModeSetuid != 0 {
			mode |= 0o4000
		}
		if st.Mode()&fs.ModeSetgid != 0 {
			mode |= 0o2000
		}
		if st.Mode()&fs.ModeSticky != 0 {
			mode |= 0o1000
		}
		n := &Node{Name: name, Mode: mode, MTime: mtime}
		switch {
		case st.IsDir():
			n.Type = TypeDir
			ents, err := os.ReadDir(p)
			if err != nil {
				t.Fatal(err)
			}
			for _, e := range ents {
				n.Children = append(n.Children, rec(filepath.Join(p, e.Name()), e.Name()))
			}
		case st.Mode()&fs.ModeSymlink != 0:
			n.Type = TypeSymlink
			if n.Target, err = os.Readlink(p); err != nil {
				t.Fatal(err)
			}
		case st.Mode().IsRegular():
			n.Type = TypeFile
			if n.Data, err = os.ReadFile(p); err != nil {
				t.Fatal(err)
			}
		default:
			t.Fatalf("%s: unsupported file type", p)
		}
		return n
	}
	return rec(dir, "")
}

// The fake APP fixtures: the writer's image equals mksquashfs's in superblock lines, listing and
// extraction, and stays under the partition size.
func TestWriterAppFakeFixtures(t *testing.T) {
	needTools(t)
	for _, name := range []string{"app_fake", "app_fake_variants/a", "app_fake_variants/b", "app_fake_variants/c", "app_fake_variants/d"} {
		src := filepath.Join("..", "..", "..", "..", "tests", "fixtures", filepath.FromSlash(name))
		if _, err := os.Stat(src); err != nil {
			t.Skipf("fixture %s: %v", name, err)
		}
		if name == "app_fake" {
			src = appFakeCopy(t, src)
		}
		t.Run(name, func(t *testing.T) {
			root := treeFromDir(t, src, testTime)
			img, _, err := Build(root, Options{Time: testTime, AllTime: true})
			if err != nil {
				t.Fatal(err)
			}
			if len(img) > 5242880 {
				t.Errorf("image of %d bytes exceeds the APP partition", len(img))
			}
			ref := filepath.Join(t.TempDir(), "ref.sqfs")
			run(t, "mksquashfs", src, ref, "-noappend", "-no-xattrs", "-quiet", "-no-progress", "-processors", "1", "-comp", "xz", "-b", "131072",
				"-all-root", "-all-time", fmt.Sprint(testTime), "-mkfs-time", fmt.Sprint(testTime))
			mine := writeTemp(t, "mine.sqfs", img)
			compareImagesWithUnsquashfs(t, mine, ref)
			refRaw, _ := os.ReadFile(ref)
			t.Logf("%s: writer %d bytes, mksquashfs %d bytes", name, len(img), len(refRaw))
			dst := filepath.Join(t.TempDir(), "x")
			run(t, "unsquashfs", "-no-xattrs", "-f", "-d", dst, mine)
			f := openImage(t, img)
			err = f.Walk("/", func(fi FileInfo) error {
				if fi.Type != TypeFile {
					return nil
				}
				a, _ := os.ReadFile(filepath.Join(dst, filepath.FromSlash(fi.Path)))
				s, _ := os.ReadFile(filepath.Join(src, filepath.FromSlash(fi.Path)))
				if !bytes.Equal(a, s) {
					t.Errorf("%s: extraction differs from the source", fi.Path)
				}
				return nil
			})
			if err != nil {
				t.Fatal(err)
			}
		})
	}
}

// compareImagesWithUnsquashfs checks that two images of the same tree agree on every `unsquashfs -s` line
// (but the filesystem size) and on `unsquashfs -lln` (directory sizes included, which only depend on the
// layout of the listings), and that the reader gives the same canonical listing.
func compareImagesWithUnsquashfs(t *testing.T, mine, ref string) {
	t.Helper()
	a, b := sbLines(unsquashfsText(t, "-s", mine)), sbLines(unsquashfsText(t, "-s", ref))
	if strings.Join(a, "\n") != strings.Join(b, "\n") {
		t.Errorf("unsquashfs -s differs:\nwriter:\n%s\nmksquashfs:\n%s", strings.Join(a, "\n"), strings.Join(b, "\n"))
	}
	la := unsquashfsText(t, "-lln", "-full-precision", "-UTC", mine)
	lb := unsquashfsText(t, "-lln", "-full-precision", "-UTC", ref)
	if la != lb {
		t.Errorf("unsquashfs -lln differs between the writer and mksquashfs")
	}
	ra, _ := os.ReadFile(mine)
	rb, _ := os.ReadFile(ref)
	l1, err1 := openImage(t, ra).Listing()
	l2, err2 := openImage(t, rb).Listing()
	if err1 != nil || err2 != nil {
		t.Fatalf("listings: %v %v", err1, err2)
	}
	diffListings(t, "writer vs mksquashfs listing", l1, l2)
}
