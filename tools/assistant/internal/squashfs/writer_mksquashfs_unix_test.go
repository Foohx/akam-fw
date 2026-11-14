//go:build unix

package squashfs

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"testing"
)

// materialize creates the tree on the host (under dir, which must exist and be empty) for mksquashfs and
// returns the -p pseudo definitions of the device nodes (those need privileges to be created for real).
// Ownership is not applied: the comparisons run mksquashfs with -all-root and ids 0 in the tree.
func materialize(t *testing.T, dir string, root *Node) []string {
	t.Helper()
	var pseudo []string
	var rec func(host, rel string, n *Node)
	rec = func(host, rel string, n *Node) {
		switch n.Type {
		case TypeDir:
			if rel != "" {
				if err := os.Mkdir(host, 0o700); err != nil {
					t.Fatal(err)
				}
			}
			for _, c := range n.Children {
				rec(filepath.Join(host, c.Name), rel+"/"+c.Name, c)
			}
			if err := os.Chmod(host, os.FileMode(n.Mode&0o777)); err != nil {
				t.Fatal(err)
			}
			// setuid/setgid/sticky are applied after the permission bits
			if n.Mode&0o7000 != 0 {
				var m os.FileMode = os.FileMode(n.Mode & 0o777)
				if n.Mode&0o4000 != 0 {
					m |= os.ModeSetuid
				}
				if n.Mode&0o2000 != 0 {
					m |= os.ModeSetgid
				}
				if n.Mode&0o1000 != 0 {
					m |= os.ModeSticky
				}
				if err := os.Chmod(host, m); err != nil {
					t.Fatal(err)
				}
			}
		case TypeFile:
			if err := os.WriteFile(host, n.Data, 0o600); err != nil {
				t.Fatal(err)
			}
			m := os.FileMode(n.Mode & 0o777)
			if n.Mode&0o4000 != 0 {
				m |= os.ModeSetuid
			}
			if n.Mode&0o2000 != 0 {
				m |= os.ModeSetgid
			}
			if n.Mode&0o1000 != 0 {
				m |= os.ModeSticky
			}
			if err := os.Chmod(host, m); err != nil {
				t.Fatal(err)
			}
		case TypeSymlink:
			if err := os.Symlink(n.Target, host); err != nil {
				t.Skipf("no symlinks: %v", err)
			}
		case TypeFifo:
			if err := syscall.Mkfifo(host, uint32(n.Mode&0o777)); err != nil {
				t.Skipf("no fifos: %v", err)
			}
			_ = os.Chmod(host, os.FileMode(n.Mode&0o777))
		case TypeSocket:
			if err := syscall.Mknod(host, syscall.S_IFSOCK|uint32(n.Mode&0o777), 0); err != nil {
				t.Skipf("no sockets: %v", err)
			}
			_ = os.Chmod(host, os.FileMode(n.Mode&0o777))
		case TypeBlock, TypeChar:
			c := "c"
			if n.Type == TypeBlock {
				c = "b"
			}
			pseudo = append(pseudo, fmt.Sprintf("%s %s %o 0 0 %d %d", strings.TrimPrefix(rel, "/"), c, n.Mode&0o777, n.Major, n.Minor))
		}
	}
	rec(dir, "", root)
	return pseudo
}

// mksquashfsImage builds the image of root with the installed mksquashfs and the options the assistant
// uses (xz, 128 KiB blocks, all root, fixed time, no xattrs); extra are more mksquashfs arguments.
func mksquashfsImage(t *testing.T, root *Node, tm uint32, extra ...string) []byte {
	t.Helper()
	dir := t.TempDir()
	src := filepath.Join(dir, "src")
	if err := os.Mkdir(src, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { // directories without write permission would stop the removal of the temporary tree
		_ = filepath.WalkDir(src, func(p string, d os.DirEntry, err error) error {
			if d != nil && d.IsDir() {
				_ = os.Chmod(p, 0o700)
			}
			return nil
		})
	})
	pseudo := materialize(t, src, root)
	img := filepath.Join(dir, "ref.sqfs")
	args := []string{src, img, "-noappend", "-no-xattrs", "-quiet", "-no-progress", "-processors", "1", "-comp", "xz", "-b", "131072",
		"-all-root", "-all-time", fmt.Sprint(tm), "-mkfs-time", fmt.Sprint(tm)}
	sort.Strings(pseudo)
	for _, p := range pseudo {
		args = append(args, "-p", p)
	}
	args = append(args, extra...)
	run(t, "mksquashfs", args...)
	raw, err := os.ReadFile(img)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

// TestStoredImageEqualsMksquashfs is the strongest layout check: with every block stored, the writer's
// image must be byte for byte the one of mksquashfs (-noI -noD -noF): same data order, fragment packing,
// duplicate removal, inode numbering and order, directory headers and index, tables, padding.
func TestStoredImageEqualsMksquashfs(t *testing.T) {
	needTools(t)
	const tm = 1700000000
	for _, c := range diffTrees() {
		t.Run(c.name, func(t *testing.T) {
			ref := mksquashfsImage(t, c.root, tm, "-noI", "-noD", "-noF")
			got, _, err := Build(c.root, Options{Time: tm, AllTime: true, noCompress: true})
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, ref) {
				if keep := os.Getenv("AKAM_SQFS_KEEP"); keep != "" {
					_ = os.WriteFile(filepath.Join(keep, c.name+".ref"), ref, 0o600)
					_ = os.WriteFile(filepath.Join(keep, c.name+".got"), got, 0o600)
				}
				t.Fatalf("stored image differs from mksquashfs: %s", firstDiff(got, ref))
			}
		})
	}
}

// A tree of the size of a real APP (bigger than the partition before compression): the image stays within
// 5242880 bytes and is not meaningfully bigger than mksquashfs's.
func TestWriterAppSizedTree(t *testing.T) {
	needTools(t)
	root := dirN("", 0o755)
	// ~3 MB of incompressible data (binaries, images), ~9 MB of compressible data, small files
	for i := 0; i < 6; i++ {
		root.Add(fileN(fmt.Sprintf("blob%d.bin", i), 0o755, mixedBytes(int64(i), 500000+i*17)))
	}
	lib := dirN("lib", 0o755)
	for i := 0; i < 15; i++ {
		lib.Add(fileN(fmt.Sprintf("lib%02d.so", i), 0o644, textBytes(600000+i*1000)))
	}
	web := dirN("www", 0o755)
	for i := 0; i < 300; i++ {
		web.Add(fileN(fmt.Sprintf("page%03d.html", i), 0o644, textBytes(500+i*40)))
	}
	root.Add(lib, web, fileN("firmware.dat", 0o644, pseudoRandom(77, 1500000)))
	img, _, err := Build(root, Options{Time: testTime, AllTime: true})
	if err != nil {
		t.Fatal(err)
	}
	ref := mksquashfsImage(t, root, testTime)
	t.Logf("writer %d bytes, mksquashfs %d bytes (ratio %.3f)", len(img), len(ref), float64(len(img))/float64(len(ref)))
	if len(img) > 5242880 {
		t.Errorf("image of %d bytes exceeds 5242880", len(img))
	}
	if len(img)%4096 != 0 {
		t.Errorf("image of %d bytes is not a multiple of 4096", len(img))
	}
	if float64(len(img)) > 1.10*float64(len(ref)) {
		t.Errorf("image is more than 10%% bigger than mksquashfs's: %d vs %d", len(img), len(ref))
	}
	mine := writeTemp(t, "mine.sqfs", img)
	refp := writeTemp(t, "ref.sqfs", ref)
	compareImagesWithUnsquashfs(t, mine, refp)
}

// The writer against mksquashfs with real compression, on every tree: same `unsquashfs -s` lines, same
// `unsquashfs -lln` text, same canonical listing, and the same placement decisions (inode numbers, fragment
// index and offset, sparse blocks, block counts) that do not depend on the compressor.
func TestWriterMatchesMksquashfsCompressed(t *testing.T) {
	needTools(t)
	const tm = 1700000000
	for _, c := range diffTrees() {
		t.Run(c.name, func(t *testing.T) {
			ref := mksquashfsImage(t, c.root, tm)
			img, _, err := Build(c.root, Options{Time: tm, AllTime: true})
			if err != nil {
				t.Fatal(err)
			}
			compareImagesWithUnsquashfs(t, writeTemp(t, "mine.sqfs", img), writeTemp(t, "ref.sqfs", ref))
			a, b := placement(t, img), placement(t, ref)
			if len(a) != len(b) {
				t.Fatalf("%d inodes vs %d", len(a), len(b))
			}
			for i := range a {
				if a[i] != b[i] {
					t.Fatalf("placement differs:\n  writer     %s\n  mksquashfs %s", a[i], b[i])
				}
			}
		})
	}
}

// placement lists, per path, what does not depend on the compressor: inode number, fragment, number of
// blocks and which of them are sparse.
func placement(t *testing.T, img []byte) []string {
	t.Helper()
	f := openImage(t, img)
	var out []string
	err := f.walkInodes("/", func(n *inode, fi FileInfo) error {
		s := fmt.Sprintf("%s ino=%d", fi.Path, n.ino)
		if n.typ == typeFile {
			full, err := f.readInode(n.ref, true)
			if err != nil {
				return err
			}
			sp := ""
			for _, w := range full.blockSizes {
				if w == 0 {
					sp += "0"
				} else {
					sp += "1"
				}
			}
			s += fmt.Sprintf(" frag=%d/%d blocks=%s", int32(full.fragIdx), full.fragOff, sp)
		}
		out = append(out, s)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}
