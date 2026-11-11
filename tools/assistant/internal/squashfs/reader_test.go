package squashfs

import (
	"bytes"
	"compress/zlib"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"sort"
	"strings"
	"sync"
	"testing"
	"testing/iotest"

	"github.com/ulikunitz/xz"
)

func openBuilt(t *testing.T, b built) *FS {
	t.Helper()
	f, err := Open(bytes.NewReader(b.img), int64(len(b.img)))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	return f
}

func expectedListing(root *bn) []ListEntry {
	var out []ListEntry
	var walk func(p string, n *bn)
	walk = func(p string, n *bn) {
		e := ListEntry{Path: p, Type: n.typ, Mode: n.mode, UID: n.uid, GID: n.gid, MTime: int64(n.mtime), Nlink: 1}
		switch n.typ {
		case TypeDir:
			e.Nlink = 2
			for _, k := range n.kids {
				if k.n.typ == TypeDir {
					e.Nlink++
				}
			}
		case TypeFile:
			e.Size = int64(len(n.data))
			s := sha256.Sum256(n.data)
			e.SHA256 = hex.EncodeToString(s[:])
		case TypeSymlink:
			e.Size, e.Target = int64(len(n.target)), n.target
		case TypeBlock, TypeChar:
			e.Major, e.Minor = (n.rdev&0xfff00)>>8, (n.rdev&0xff)|((n.rdev>>12)&0xfff00)
		}
		out = append(out, e)
		for _, k := range n.kids {
			walk(strings.TrimSuffix(p, "/")+"/"+k.name, k.n)
		}
	}
	walk("/", root)
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out
}

func listLines(l []ListEntry) string {
	var b strings.Builder
	for _, e := range l {
		b.WriteString(e.String() + "\n")
	}
	return b.String()
}

func TestBuilderImagesRoundTrip(t *testing.T) {
	opts := map[string]bopt{
		"xz":              {comp: CompXZ},
		"xz-options":      {comp: CompXZ, options: true},
		"gzip":            {comp: CompGzip},
		"gzip-options":    {comp: CompGzip, options: true},
		"stored":          {comp: CompXZ, storeData: true},
		"no-fragments":    {comp: CompGzip, noFrag: true},
		"export":          {comp: CompXZ, export: true},
		"bs4k":            {comp: CompGzip, bs: 4096, export: true},
		"bs1m":            {comp: CompXZ, bs: 1 << 20},
		"padded":          {comp: CompXZ, padTo: 4096, export: true},
		"no-sparse":       {comp: CompGzip, noSparse: true},
		"gzip-bs8k-store": {comp: CompGzip, bs: 8192, storeData: true},
	}
	for _, ext := range []bool{false, true} {
		for name, o := range opts {
			t.Run(fmt.Sprintf("%s/ext=%v", name, ext), func(t *testing.T) {
				root := sampleTree(ext)
				b := buildImage(root, o)
				f := openBuilt(t, b)
				if err := f.Verify(); err != nil {
					t.Fatal(err)
				}
				got, err := f.Listing()
				if err != nil {
					t.Fatal(err)
				}
				if g, w := listLines(got), listLines(expectedListing(root)); g != w {
					t.Fatalf("listing differs:\n%s\nwant:\n%s", g, w)
				}
				if o.export {
					tab, err := f.ExportTable()
					if err != nil || len(tab) != len(b.nodes) {
						t.Fatalf("ExportTable: %d entries, %v", len(tab), err)
					}
				}
				info := f.Info()
				if info.CompressionName() == "" || info.Inodes != uint32(len(b.nodes)) {
					t.Fatalf("Info: %+v", info)
				}
			})
		}
	}
}

func TestAPIBehaviour(t *testing.T) {
	root := sampleTree(false)
	f := openBuilt(t, buildImage(root, bopt{comp: CompGzip, bs: 4096}))

	fi, err := f.Lstat("/sub/deep/f")
	if err != nil || fi.Type != TypeFile || fi.Size != 9 || fi.Name != "f" || fi.Path != "/sub/deep/f" {
		t.Fatalf("Lstat: %+v %v", fi, err)
	}
	if fi2, err := f.Lstat("sub//deep/../deep/./f"); err != nil || fi2.Path != fi.Path {
		t.Fatalf("path cleaning: %+v %v", fi2, err)
	}
	if rf, err := f.Lstat(""); err != nil || rf.Path != "/" || rf.Type != TypeDir {
		t.Fatalf("root: %+v %v", rf, err)
	}
	if _, err := f.Lstat("/nope"); !errors.Is(err, fs.ErrNotExist) {
		t.Fatalf("missing: %v", err)
	}
	if _, err := f.Lstat("/small/x"); !errors.Is(err, ErrNotDir) {
		t.Fatalf("through a file: %v", err)
	}
	if _, err := f.Lstat("/link/x"); !errors.Is(err, ErrNotDir) {
		t.Fatalf("through a symlink (never followed): %v", err)
	}
	if _, err := f.Lstat("/a\x00b"); err == nil {
		t.Fatal("NUL in path accepted")
	}
	if l, err := f.Readlink("/link"); err != nil || l != "small" {
		t.Fatalf("Readlink: %q %v", l, err)
	}
	if _, err := f.Readlink("/small"); !errors.Is(err, ErrNotSymlink) {
		t.Fatalf("Readlink on a file: %v", err)
	}
	if _, err := f.ReadFile("/sub"); !errors.Is(err, ErrNotRegular) {
		t.Fatalf("ReadFile on a dir: %v", err)
	}
	if _, err := f.ReadDir("/small"); !errors.Is(err, ErrNotDir) {
		t.Fatalf("ReadDir on a file: %v", err)
	}
	ents, err := f.ReadDir("/")
	if err != nil || len(ents) != len(root.kids) {
		t.Fatalf("ReadDir: %d entries, %v", len(ents), err)
	}
	for i := 1; i < len(ents); i++ {
		if ents[i-1].Name >= ents[i].Name {
			t.Fatal("ReadDir is not sorted")
		}
	}
	dev, _ := f.Lstat("/dev")
	if dev.Type != TypeChar || dev.Major != 1 || dev.Minor != 3 {
		t.Fatalf("dev: %+v", dev)
	}
	big, _ := f.Lstat("/big")
	if big.Mode != 0o4755 || big.UID != 1000 || big.GID != 100 || big.MTime != 99 {
		t.Fatalf("big: %+v", big)
	}
	data, err := f.ReadFile("/big")
	if err != nil || !bytes.Equal(data, root.kids[0].n.data) {
		t.Fatalf("ReadFile(/big): %d bytes, %v", len(data), err)
	}
	// streaming with tiny reads, partial consumption and EOF
	r, err := f.OpenFile("/hard2")
	if err != nil {
		t.Fatal(err)
	}
	got, err := io.ReadAll(iotest.OneByteReader(r))
	if err != nil || !bytes.Equal(got, textBytes(3000)) || r.Size() != 3000 {
		t.Fatalf("one-byte reads: %d bytes, %v", len(got), err)
	}
	if n, err := r.Read(make([]byte, 4)); n != 0 || err != io.EOF {
		t.Fatalf("read after EOF: %d %v", n, err)
	}
	if err := iotest.TestReader(mustOpen(t, f, "/zeros"), make([]byte, 300000)); err != nil {
		t.Fatal(err)
	}
	// Walk order, SkipDir
	var seen []string
	err = f.Walk("/", func(fi FileInfo) error {
		seen = append(seen, fi.Path)
		if fi.Path == "/sub/deep" {
			return SkipDir
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range seen {
		if p == "/sub/deep/f" {
			t.Fatal("SkipDir did not skip")
		}
	}
	if seen[0] != "/" || seen[len(seen)-1] != "/zeros" {
		t.Fatalf("walk order: %v", seen)
	}
	stop := errors.New("stop")
	if err := f.Walk("/sub", func(fi FileInfo) error { return stop }); err != stop {
		t.Fatalf("walk error: %v", err)
	}
	// concurrent use
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := f.Listing(); err != nil {
				t.Error(err)
			}
		}()
	}
	wg.Wait()
}

type readerOnly struct{ io.Reader }

func mustOpen(t *testing.T, f *FS, p string) io.Reader {
	t.Helper()
	r, err := f.OpenFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return readerOnly{r}
}

func TestLargeDeviceNumbers(t *testing.T) {
	// the kernel's new_encode_dev layout: minor bits above 8 go to bits 20+
	major, minor := uint32(259), uint32(70000)
	rdev := (minor & 0xff) | (major << 8) | ((minor &^ 0xff) << 12)
	root := bdir(bk("d", &bn{typ: TypeChar, mode: 0o600, rdev: rdev}))
	f := openBuilt(t, buildImage(root, bopt{}))
	fi, err := f.Lstat("/d")
	if err != nil || fi.Major != major || fi.Minor != minor || fi.Rdev != rdev {
		t.Fatalf("%+v %v", fi, err)
	}
}

func TestOddNames(t *testing.T) {
	long := strings.Repeat("é", 127) + "x" // 255 bytes
	root := bdir(bk(long, bfile([]byte("a"))), bk("new\nline\ttab", bfile([]byte("b"))), bk("-leading", bfile(nil)), bk("日本語", bfile([]byte("c"))))
	f := openBuilt(t, buildImage(root, bopt{}))
	got, err := f.Listing()
	if err != nil {
		t.Fatal(err)
	}
	if listLines(got) != listLines(expectedListing(root)) {
		t.Fatalf("listing:\n%s", listLines(got))
	}
	if strings.Count(listLines(got), "\n") != 5 {
		t.Fatal("a name with a newline broke the one-line-per-entry format")
	}
}

// tamper helpers

func inodeFileOff(b built, n *bn, off int) int {
	p := n.pos + off
	return b.inodeStart + p/metaBlockSize*(metaBlockSize+2) + 2 + p%metaBlockSize
}

func clone(b built) []byte { return append([]byte(nil), b.img...) }

func openBytes(img []byte, lim Limits) (*FS, error) {
	return OpenLimits(bytes.NewReader(img), int64(len(img)), lim)
}

func mustFail(t *testing.T, what string, err error, target error) {
	t.Helper()
	if err == nil {
		t.Fatalf("%s: accepted", what)
	}
	if target != nil && !errors.Is(err, target) {
		t.Fatalf("%s: error %v is not %v", what, err, target)
	}
}

func TestRefusedAtOpen(t *testing.T) {
	root := sampleTree(false)
	b := buildImage(root, bopt{comp: CompXZ, options: true})
	patch16 := func(off int, v uint16) []byte { i := clone(b); binary.LittleEndian.PutUint16(i[off:], v); return i }
	patch32 := func(off int, v uint32) []byte { i := clone(b); binary.LittleEndian.PutUint32(i[off:], v); return i }
	patch64 := func(off int, v uint64) []byte { i := clone(b); binary.LittleEndian.PutUint64(i[off:], v); return i }
	cases := []struct {
		name string
		img  []byte
		want error
	}{
		{"magic", patch32(0, 1), ErrCorrupt},
		{"lzma", patch16(20, CompLZMA), ErrUnsupported},
		{"lzo", patch16(20, CompLZO), ErrUnsupported},
		{"lz4", patch16(20, CompLZ4), ErrUnsupported},
		{"zstd", patch16(20, CompZstd), ErrUnsupported},
		{"unknown compression", patch16(20, 77), ErrUnsupported},
		{"version 3", patch16(28, 3), ErrUnsupported},
		{"minor 1", patch16(30, 1), ErrUnsupported},
		{"block size not a power of two", patch32(12, 100000), ErrCorrupt},
		{"block size too small", patch32(12, 2048), ErrCorrupt},
		{"block size too big", patch32(12, 1<<21), ErrCorrupt},
		{"block log mismatch", patch16(22, 12), ErrCorrupt},
		{"zero inodes", patch32(4, 0), ErrLimit},
		{"too many inodes", patch32(4, 1<<30), ErrLimit},
		{"too many fragments", patch32(16, 1<<30), ErrLimit},
		{"no ids", patch16(26, 0), ErrCorrupt},
		{"bytes used beyond the image", patch64(40, uint64(len(b.img))+1), ErrCorrupt},
		{"id table outside", patch64(48, 1<<40), ErrCorrupt},
		{"id table at the end", patch64(48, uint64(len(b.img))), ErrCorrupt},
		{"inode table after dir table", patch64(64, uint64(b.dirStart)+1), ErrCorrupt},
		{"inode table in the superblock", patch64(64, 10), ErrCorrupt},
		{"dir table outside", patch64(72, 1<<40), ErrCorrupt},
		{"fragment table outside", patch64(80, 1<<40), ErrCorrupt},
		{"root ref outside", patch64(32, 1<<40), ErrCorrupt},
		{"root ref offset", patch64(32, 9000), ErrCorrupt},
		{"huge id count with a short table", patch16(26, 65535), ErrCorrupt},
		{"xz filters (BCJ)", patch32(96+2+4, 0x4), ErrUnsupported},
		{"xz dictionary too big", patch32(96+2, 1<<30), ErrUnsupported},
		{"xz dictionary too small", patch32(96+2, 100), ErrUnsupported},
		{"options block compressed", patch16(96, 8), ErrCorrupt},
		{"options block empty", patch16(96, 0x8000), ErrCorrupt},
		{"options block too short", patch16(96, 0x8000|4), ErrCorrupt},
		{"exportable without table", func() []byte {
			i := clone(b)
			binary.LittleEndian.PutUint16(i[24:], binary.LittleEndian.Uint16(i[24:])|FlagExportable)
			return i
		}(), ErrCorrupt},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := openBytes(c.img, Limits{})
			mustFail(t, c.name, err, c.want)
		})
	}
	t.Run("size", func(t *testing.T) {
		_, err := Open(bytes.NewReader(b.img), 50)
		mustFail(t, "tiny", err, ErrCorrupt)
		_, err = OpenLimits(bytes.NewReader(b.img), int64(len(b.img)), Limits{MaxImageSize: 1000})
		mustFail(t, "image cap", err, ErrLimit)
		_, err = Open(bytes.NewReader(b.img), 5<<20+1)
		mustFail(t, "default 5 MiB cap", err, ErrLimit)
	})
	t.Run("gzip options of 3 bytes", func(t *testing.T) {
		g := buildImage(root, bopt{comp: CompGzip, options: true})
		i := clone(g)
		binary.LittleEndian.PutUint16(i[96:], 0x8000|4)
		_, err := openBytes(i, Limits{})
		mustFail(t, "short gzip options", err, nil)
	})
}

func TestStructuralAttacks(t *testing.T) {
	t.Run("directory cycle", func(t *testing.T) {
		root := bdir()
		sub := bdir(bk("up", root))
		root.kids = []bkid{bk("sub", sub)}
		f := openBuilt(t, buildImage(root, bopt{}))
		_, err := f.Listing()
		mustFail(t, "cycle", err, ErrCorrupt)
		mustFail(t, "Verify", f.Verify(), ErrCorrupt)
		mustFail(t, "Walk", f.Walk("/", func(FileInfo) error { return nil }), ErrCorrupt)
	})
	t.Run("directory linked twice", func(t *testing.T) {
		shared := bdir(bk("f", bfile([]byte("x"))))
		root := bdir(bk("a", shared), bk("b", shared))
		f := openBuilt(t, buildImage(root, bopt{}))
		_, err := f.Listing()
		mustFail(t, "hard linked dir", err, ErrCorrupt)
	})
	t.Run("self loop", func(t *testing.T) {
		root := bdir()
		root.kids = []bkid{bk("self", root)}
		f := openBuilt(t, buildImage(root, bopt{}))
		mustFail(t, "self loop", f.Walk("/", func(FileInfo) error { return nil }), ErrCorrupt)
	})
	t.Run("too deep", func(t *testing.T) {
		leaf := bdir()
		cur := leaf
		for i := 0; i < 200; i++ {
			cur = bdir(bk("d", cur))
		}
		f := openBuilt(t, buildImage(cur, bopt{}))
		_, err := f.Listing()
		mustFail(t, "depth", err, ErrLimit)
		p := strings.Repeat("d/", 150)
		_, err = f.Lstat(p)
		mustFail(t, "deep Lstat", err, ErrLimit)
	})
	t.Run("too many entries in a directory", func(t *testing.T) {
		var kids []bkid
		for i := 0; i < 50; i++ {
			kids = append(kids, bk(fmt.Sprintf("f%02d", i), bfile(nil)))
		}
		f, err := OpenLimits(bytes.NewReader(buildImage(bdir(kids...), bopt{}).img), int64(len(buildImage(bdir(kids...), bopt{}).img)), Limits{MaxDirEntries: 10})
		if err != nil {
			t.Fatal(err)
		}
		_, err = f.ReadDir("/")
		mustFail(t, "entries", err, ErrLimit)
	})
	t.Run("name too long", func(t *testing.T) {
		root := bdir(bk(strings.Repeat("n", 300), bfile(nil)))
		f := openBuilt(t, buildImage(root, bopt{}))
		_, err := f.ReadDir("/")
		mustFail(t, "name", err, ErrCorrupt)
	})
	t.Run("bad names", func(t *testing.T) {
		for _, name := range []string{"..", ".", "a/b", "a\x00b"} {
			root := bdir(bk(name, bfile(nil)))
			f := openBuilt(t, buildImage(root, bopt{}))
			_, err := f.ReadDir("/")
			mustFail(t, fmt.Sprintf("name %q", name), err, ErrCorrupt)
		}
	})
	t.Run("duplicate names", func(t *testing.T) {
		root := bdir(bk("x", bfile([]byte("1"))), bk("x", bfile([]byte("2"))))
		f := openBuilt(t, buildImage(root, bopt{}))
		_, err := f.ReadDir("/")
		mustFail(t, "duplicates", err, ErrCorrupt)
	})
	t.Run("entry type disagrees with inode", func(t *testing.T) {
		root := bdir(bk("x", bfile([]byte("1"))))
		b := buildImage(root, bopt{})
		i := clone(b)
		// first byte of the directory table payload: header(12) then entry(offset,delta,type,size): type at +12+4
		binary.LittleEndian.PutUint16(i[b.dirStart+2+12+4:], uint16(TypeSymlink))
		f, err := openBytes(i, Limits{})
		if err != nil {
			t.Fatal(err)
		}
		_, err = f.ReadDir("/")
		mustFail(t, "type", err, ErrCorrupt)
	})
	t.Run("entry inode number out of range", func(t *testing.T) {
		root := bdir(bk("x", bfile([]byte("1"))))
		b := buildImage(root, bopt{})
		i := clone(b)
		binary.LittleEndian.PutUint32(i[b.dirStart+2+8:], 9999)
		f, _ := openBytes(i, Limits{})
		_, err := f.ReadDir("/")
		mustFail(t, "inode number", err, ErrCorrupt)
	})
	t.Run("directory header claims 256+ entries", func(t *testing.T) {
		root := bdir(bk("x", bfile([]byte("1"))))
		b := buildImage(root, bopt{})
		i := clone(b)
		binary.LittleEndian.PutUint32(i[b.dirStart+2:], 1000)
		f, _ := openBytes(i, Limits{})
		_, err := f.ReadDir("/")
		mustFail(t, "count", err, ErrCorrupt)
	})
	t.Run("directory inode block outside the table", func(t *testing.T) {
		root := bdir(bk("x", bfile([]byte("1"))))
		b := buildImage(root, bopt{})
		i := clone(b)
		binary.LittleEndian.PutUint32(i[b.dirStart+2+4:], 1<<20)
		f, _ := openBytes(i, Limits{})
		_, err := f.ReadDir("/")
		mustFail(t, "start", err, ErrCorrupt)
	})
}

func TestInodeFieldAttacks(t *testing.T) {
	root := sampleTree(true)
	b := buildImage(root, bopt{comp: CompGzip, bs: 4096})
	find := func(name string) *bn {
		for _, k := range root.kids {
			if k.name == name {
				return k.n
			}
		}
		t.Fatalf("no %s", name)
		return nil
	}
	set32 := func(n *bn, off int, v uint32) *FS {
		i := clone(b)
		binary.LittleEndian.PutUint32(i[inodeFileOff(b, n, off):], v)
		f, err := openBytes(i, Limits{})
		if err != nil {
			t.Fatal(err)
		}
		return f
	}
	set64 := func(n *bn, off int, v uint64) *FS {
		i := clone(b)
		binary.LittleEndian.PutUint64(i[inodeFileOff(b, n, off):], v)
		f, err := openBytes(i, Limits{})
		if err != nil {
			t.Fatal(err)
		}
		return f
	}
	small := find("small")
	big := find("big")
	t.Run("file size beyond the cap", func(t *testing.T) {
		f := set64(big, 16+8, 1<<45)
		_, err := f.Lstat("/big")
		mustFail(t, "size", err, ErrLimit)
	})
	t.Run("file size larger than its block list", func(t *testing.T) {
		f := set64(big, 16+8, 100<<20) // 25600 blocks: the inode table does not hold that many words
		_, err := f.ReadFile("/big")
		mustFail(t, "size", err, ErrCorrupt)
	})
	t.Run("blocks start outside", func(t *testing.T) {
		f := set64(big, 16, 1<<40)
		_, err := f.ReadFile("/big")
		mustFail(t, "start", err, ErrCorrupt)
	})
	t.Run("fragment index outside", func(t *testing.T) {
		f := set32(small, 16+28, 500)
		_, err := f.ReadFile("/small")
		mustFail(t, "fragment", err, ErrCorrupt)
	})
	t.Run("fragment offset outside the block", func(t *testing.T) {
		f := set32(small, 16+32, 4097)
		_, err := f.ReadFile("/small")
		mustFail(t, "offset", err, ErrCorrupt)
	})
	t.Run("fragment tail past the block", func(t *testing.T) {
		f := set32(small, 16+32, 4096)
		_, err := f.ReadFile("/small")
		mustFail(t, "tail", err, ErrCorrupt)
	})
	t.Run("block word too big", func(t *testing.T) {
		f := set32(big, 16+40, 1<<20)
		_, err := f.ReadFile("/big")
		mustFail(t, "word", err, ErrCorrupt)
	})
	t.Run("block words sum past the image", func(t *testing.T) {
		f := set32(big, 16+40, 4096)
		f2 := set32(big, 16+44, 4096)
		_ = f2
		_, err := f.ReadFile("/big")
		if err == nil {
			t.Log("first block still readable (it fits); not an error by itself")
		}
		f3 := set64(big, 16, uint64(len(b.img))-100)
		_, err = f3.ReadFile("/big")
		mustFail(t, "past the image", err, ErrCorrupt)
	})
	t.Run("bad inode type", func(t *testing.T) {
		i := clone(b)
		binary.LittleEndian.PutUint16(i[inodeFileOff(b, small, 0):], 99)
		f, _ := openBytes(i, Limits{})
		_, err := f.Lstat("/small")
		mustFail(t, "type", err, ErrCorrupt)
	})
	t.Run("bad uid index", func(t *testing.T) {
		i := clone(b)
		binary.LittleEndian.PutUint16(i[inodeFileOff(b, small, 4):], 60000)
		f, _ := openBytes(i, Limits{})
		_, err := f.Lstat("/small")
		mustFail(t, "uid", err, ErrCorrupt)
	})
	t.Run("bad inode number", func(t *testing.T) {
		f := set32(small, 12, 0)
		_, err := f.Lstat("/small")
		mustFail(t, "ino", err, ErrCorrupt)
	})
	t.Run("symlink target length", func(t *testing.T) {
		f := set32(find("link"), 16+4, 1<<30)
		_, err := f.Readlink("/link")
		mustFail(t, "target", err, ErrLimit)
		f = set32(find("link"), 16+4, 4000)
		_, err = f.Readlink("/link")
		mustFail(t, "target past the table", err, ErrCorrupt)
	})
	t.Run("directory size", func(t *testing.T) {
		f := set32(root, 16+4, 0xffffffff)
		_, err := f.ReadDir("/")
		mustFail(t, "huge dir", err, ErrLimit)
		f = set32(root, 16+4, 2)
		_, err = f.ReadDir("/")
		mustFail(t, "tiny dir", err, ErrCorrupt)
		f = set32(root, 16+4, 20)
		_, err = f.ReadDir("/")
		mustFail(t, "truncated dir", err, ErrCorrupt)
	})
	t.Run("directory offset", func(t *testing.T) {
		i := clone(b)
		binary.LittleEndian.PutUint16(i[inodeFileOff(b, find("sub"), 16+18):], 0xffff)
		f, _ := openBytes(i, Limits{})
		_, err := f.ReadDir("/sub")
		mustFail(t, "dir offset", err, ErrCorrupt)
	})
}

func TestBombs(t *testing.T) {
	t.Run("deflate bomb as a data block", func(t *testing.T) {
		var z bytes.Buffer
		w := zlib.NewWriter(&z)
		w.Write(make([]byte, 200<<20))
		w.Close()
		if z.Len() > 131072 {
			t.Skipf("bomb too big (%d)", z.Len())
		}
		b := buildImage(bdir(bk("f", bfile(textBytes(131072)))), bopt{comp: CompGzip, storeData: true})
		f := openBuilt(t, b)
		f.sb.Compression = CompGzip
		_, err := f.decompress(z.Bytes(), 131072)
		mustFail(t, "bomb", err, ErrLimit)
	})
	t.Run("xz bomb", func(t *testing.T) {
		var z bytes.Buffer
		w, _ := xz.WriterConfig{CheckSum: xz.CRC32, DictCap: 131072}.NewWriter(&z)
		w.Write(make([]byte, 64<<20))
		w.Close()
		b := buildImage(bdir(bk("f", bfile(textBytes(10)))), bopt{comp: CompXZ})
		f := openBuilt(t, b)
		_, err := f.decompress(z.Bytes(), metaBlockSize)
		mustFail(t, "bomb", err, ErrLimit)
	})
	t.Run("sparse file under a call budget", func(t *testing.T) {
		root := bdir(bk("holes", bfile(make([]byte, 8<<20))))
		b := buildImage(root, bopt{bs: 4096})
		f, err := openBytes(b.img, Limits{MaxCallBytes: 1 << 20})
		if err != nil {
			t.Fatal(err)
		}
		_, err = f.ReadFile("/holes")
		mustFail(t, "ReadFile", err, ErrLimit)
		_, err = f.Listing()
		mustFail(t, "Listing", err, ErrLimit)
		f, _ = openBytes(b.img, Limits{MaxFileSize: 1 << 20})
		_, err = f.Lstat("/holes")
		mustFail(t, "file cap", err, ErrLimit)
	})
	t.Run("listing budget is shared by all files", func(t *testing.T) {
		root := bdir(bk("a", bfile(textBytes(300000))), bk("b", bfile(textBytes(300000))), bk("c", bfile(textBytes(300000))))
		b := buildImage(root, bopt{})
		f, _ := openBytes(b.img, Limits{MaxCallBytes: 700000})
		if _, err := f.Listing(); !errors.Is(err, ErrLimit) {
			t.Fatalf("Listing: %v", err)
		}
		if _, err := f.ReadFile("/a"); err != nil {
			t.Fatalf("a single file is under the budget: %v", err)
		}
	})
	t.Run("metadata budget", func(t *testing.T) {
		b := buildImage(sampleTree(false), bopt{})
		_, err := openBytes(b.img, Limits{MaxMetadataBytes: 10})
		mustFail(t, "metadata", err, ErrLimit)
	})
	t.Run("metadata chain", func(t *testing.T) {
		var kids []bkid
		for i := 0; i < 2000; i++ {
			kids = append(kids, bk(fmt.Sprintf("file_number_%05d", i), bfile(nil)))
		}
		b := buildImage(bdir(kids...), bopt{})
		f, err := openBytes(b.img, Limits{MaxMetaChain: 2})
		if err != nil {
			t.Fatal(err)
		}
		_, err = f.ReadDir("/")
		mustFail(t, "chain", err, ErrLimit)
	})
	t.Run("many inodes declared", func(t *testing.T) {
		b := buildImage(sampleTree(false), bopt{})
		_, err := openBytes(b.img, Limits{MaxInodes: 3})
		mustFail(t, "inodes", err, ErrLimit)
	})
}

func TestTruncationsAndCorruption(t *testing.T) {
	for _, ext := range []bool{false, true} {
		b := buildImage(sampleTree(ext), bopt{comp: CompGzip, bs: 4096, export: true, options: true})
		lim := Limits{MaxCallBytes: 8 << 20, MaxMetadataBytes: 4 << 20}
		exercise := func(img []byte, size int64) {
			f, err := OpenLimits(bytes.NewReader(img), size, lim)
			if err != nil {
				return
			}
			_, _ = f.Listing()
			_, _ = f.ExportTable()
		}
		t.Logf("image of %d bytes, metadata %d..%d", len(b.img), b.inodeStart, b.end)
		for n := 0; n < len(b.img); n += 1 + n/32 {
			exercise(b.img[:n], int64(n))
			// the image claims its full size but the reader stops early
			exercise(b.img[:n], int64(len(b.img)))
		}
		// a corrupted byte, anywhere in the metadata and in the first data
		for i := 0; i < len(b.img); i++ {
			if i > b.dataStart+300 && i < b.inodeStart {
				i = b.inodeStart - 1
				continue
			}
			c := clone(b)
			c[i] ^= 0xff
			exercise(c, int64(len(c)))
			if i%3 == 0 {
				c[i] = 0
				exercise(c, int64(len(c)))
			}
		}
	}
}

func TestZlibStreamErrors(t *testing.T) {
	b := buildImage(bdir(bk("f", bfile(textBytes(5000)))), bopt{comp: CompGzip, bs: 4096})
	f := openBuilt(t, b)
	for _, bad := range [][]byte{nil, {1, 2, 3}, bytes.Repeat([]byte{0x78, 0x9c}, 100)} {
		if _, err := f.decompress(bad, 100); !errors.Is(err, ErrCorrupt) && !errors.Is(err, ErrLimit) {
			t.Fatalf("decompress(%v): %v", bad, err)
		}
	}
	bx := buildImage(bdir(bk("f", bfile(textBytes(5000)))), bopt{comp: CompXZ})
	fx := openBuilt(t, bx)
	for _, bad := range [][]byte{nil, {0xfd, '7', 'z', 'X', 'Z', 0}, bytes.Repeat([]byte{0xfd}, 100)} {
		if _, err := fx.decompress(bad, 100); err == nil {
			t.Fatalf("xz accepted %v", bad)
		}
	}
}

func FuzzOpen(f *testing.F) {
	for _, ext := range []bool{false, true} {
		for _, o := range []bopt{{comp: CompXZ, export: true}, {comp: CompGzip, bs: 4096, options: true}} {
			root := bdir(bk("a", bfile(textBytes(5000))), bk("l", &bn{typ: TypeSymlink, ext: ext, mode: 0o777, target: "a"}), bk("d", bdir(bk("z", bfile(nil)))))
			f.Add(buildImage(root, o).img)
		}
	}
	if seed, err := readTestdata(); err == nil {
		for _, s := range seed {
			f.Add(s)
		}
	}
	lim := Limits{MaxImageSize: 1 << 20, MaxCallBytes: 8 << 20, MaxMetadataBytes: 4 << 20, MaxFileSize: 8 << 20, MaxInodes: 5000}
	f.Fuzz(func(t *testing.T, img []byte) {
		fsys, err := OpenLimits(bytes.NewReader(img), int64(len(img)), lim)
		if err != nil {
			return
		}
		_, _ = fsys.Listing()
		_ = fsys.Verify()
		_, _ = fsys.Lstat("/a")
		_, _ = fsys.ReadDir("/")
		_, _ = fsys.ReadFile("/a")
		_, _ = fsys.ExportTable()
	})
}
