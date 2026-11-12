package squashfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/ulikunitz/xz/lzma"
)

// bigLimits lets the reader open the images of the tests, some of which are bigger than the APP partition.
var bigLimits = Limits{MaxImageSize: 1 << 30, MaxInodes: 1 << 20, MaxMetadataBytes: 1 << 30}

func openImage(t testing.TB, img []byte) *FS {
	t.Helper()
	f, err := OpenLimits(bytes.NewReader(img), int64(len(img)), bigLimits)
	if err != nil {
		t.Fatalf("the reader refuses the image: %v", err)
	}
	return f
}

// treeListing computes the canonical listing the image of root must have, from the tree alone.
func treeListing(root *Node, o Options) []ListEntry {
	var out []ListEntry
	var rec func(n *Node, p string)
	rec = func(n *Node, p string) {
		mt := int64(n.MTime)
		if o.AllTime {
			mt = int64(o.Time)
		}
		e := ListEntry{Path: p, Type: n.Type, Mode: n.Mode, UID: n.UID, GID: n.GID, MTime: mt, Nlink: 1}
		switch n.Type {
		case TypeDir:
			e.Nlink = 2
			for _, c := range n.Children {
				if c.Type == TypeDir {
					e.Nlink++
				}
			}
		case TypeFile:
			e.Size = int64(len(n.Data))
			s := sha256.Sum256(n.Data)
			e.SHA256 = hex.EncodeToString(s[:])
		case TypeSymlink:
			e.Size, e.Target = int64(len(n.Target)), n.Target
		case TypeBlock, TypeChar:
			e.Major, e.Minor = n.Major, n.Minor
		}
		out = append(out, e)
		for _, c := range n.Children {
			rec(c, path.Join(p, c.Name))
		}
	}
	rec(root, "/")
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out
}

func diffListings(t testing.TB, what string, got, want []ListEntry) {
	t.Helper()
	if len(got) != len(want) {
		t.Errorf("%s: %d entries, want %d", what, len(got), len(want))
	}
	bad := 0
	for i := 0; i < min(len(got), len(want)); i++ {
		if got[i] != want[i] {
			t.Errorf("%s: entry %d\n  got  %s\n  want %s", what, i, got[i], want[i])
			if bad++; bad > 5 {
				return
			}
		}
	}
}

// rawSB is the superblock read straight from the bytes of an image.
type rawSB struct {
	inodes, mkfsTime, blockSize, fragments       uint32
	comp, blockLog, flags, ids, major, minor     uint16
	root, bytesUsed, idStart, xattrStart         uint64
	inodeStart, dirStart, fragStart, lookupStart uint64
}

func parseRawSB(img []byte) rawSB {
	le := binary.LittleEndian
	return rawSB{
		inodes: le.Uint32(img[4:]), mkfsTime: le.Uint32(img[8:]), blockSize: le.Uint32(img[12:]), fragments: le.Uint32(img[16:]),
		comp: le.Uint16(img[20:]), blockLog: le.Uint16(img[22:]), flags: le.Uint16(img[24:]), ids: le.Uint16(img[26:]),
		major: le.Uint16(img[28:]), minor: le.Uint16(img[30:]),
		root: le.Uint64(img[32:]), bytesUsed: le.Uint64(img[40:]), idStart: le.Uint64(img[48:]), xattrStart: le.Uint64(img[56:]),
		inodeStart: le.Uint64(img[64:]), dirStart: le.Uint64(img[72:]), fragStart: le.Uint64(img[80:]), lookupStart: le.Uint64(img[88:]),
	}
}

// metaFrames walks the metadata blocks in [start, end) and returns for each its offset, payload, and
// whether it is stored. The blocks must tile the range exactly.
type metaFrame struct {
	off     int
	payload []byte
	stored  bool
}

func metaFrames(img []byte, start, end int) ([]metaFrame, error) {
	var out []metaFrame
	for p := start; p < end; {
		if p+2 > end {
			return nil, fmt.Errorf("metadata block header at %d overruns the range [%d,%d)", p, start, end)
		}
		h := binary.LittleEndian.Uint16(img[p:])
		n := int(h & 0x7fff)
		if n == 0 || n > metaBlockSize || p+2+n > end {
			return nil, fmt.Errorf("metadata block at %d has size %d (range ends at %d)", p, n, end)
		}
		out = append(out, metaFrame{off: p, payload: img[p+2 : p+2+n], stored: h&0x8000 != 0})
		p += 2 + n
	}
	return out, nil
}

// checkLayout verifies the table layout the kernel's sanity checks and mksquashfs rely on: inode table,
// directory table, fragment blocks and index, export blocks and index, id blocks and index, in this order,
// adjacent, the index of each table right after its blocks and pointing at them, bytes_used at the end of
// the id index, xattr start all ones. It returns every metadata frame (for the xz checks).
func checkLayout(t testing.TB, img []byte) []metaFrame {
	t.Helper()
	sb := parseRawSB(img)
	le := binary.LittleEndian
	ceil := func(a, b int) int { return (a + b - 1) / b }
	nFrag, nLook, nIDs := ceil(int(sb.fragments), 512), ceil(int(sb.inodes), 1024), ceil(int(sb.ids), 2048)
	if sb.xattrStart != ^uint64(0) {
		t.Errorf("xattr_id_table_start 0x%x, want all ones", sb.xattrStart)
	}
	if sb.flags&FlagNoXattrs == 0 || sb.flags&FlagExportable == 0 || sb.flags&FlagDuplicates == 0 {
		t.Errorf("flags 0x%x lack no-xattrs, exportable or duplicates", sb.flags)
	}
	var frames []metaFrame
	add := func(what string, start, end int) []metaFrame {
		fr, err := metaFrames(img, start, end)
		if err != nil {
			t.Fatalf("%s table: %v", what, err)
		}
		frames = append(frames, fr...)
		return fr
	}
	index := func(what string, at, n int) []int {
		if at+8*n > len(img) {
			t.Fatalf("%s index at %d overruns the image", what, at)
		}
		out := make([]int, n)
		for i := range out {
			out[i] = int(le.Uint64(img[at+8*i:]))
		}
		return out
	}
	checkIndex := func(what string, fr []metaFrame, idx []int) {
		if len(fr) != len(idx) {
			t.Errorf("%s: %d blocks but %d index entries", what, len(fr), len(idx))
			return
		}
		for i := range fr {
			if fr[i].off != idx[i] {
				t.Errorf("%s: index entry %d is %d, the block is at %d", what, i, idx[i], fr[i].off)
			}
		}
	}
	// the data ends where the inode table starts (no compressor options block)
	if sb.inodeStart < sbSize || sb.inodeStart >= sb.dirStart || sb.dirStart > sb.fragStart || sb.fragStart > sb.lookupStart || sb.lookupStart >= sb.idStart || sb.idStart >= sb.bytesUsed {
		t.Fatalf("table order: inode %d dir %d frag %d lookup %d id %d used %d", sb.inodeStart, sb.dirStart, sb.fragStart, sb.lookupStart, sb.idStart, sb.bytesUsed)
	}
	add("inode", int(sb.inodeStart), int(sb.dirStart))
	fidx := index("fragment", int(sb.fragStart), nFrag)
	fragFirst := int(sb.fragStart)
	if nFrag > 0 {
		fragFirst = fidx[0]
	}
	add("directory", int(sb.dirStart), fragFirst)
	checkIndex("fragment", add("fragment", fragFirst, int(sb.fragStart)), fidx)
	lookFirst := int(sb.fragStart) + 8*nFrag
	lidx := index("export", int(sb.lookupStart), nLook)
	checkIndex("export", add("export", lookFirst, int(sb.lookupStart)), lidx)
	idFirst := int(sb.lookupStart) + 8*nLook
	iidx := index("id", int(sb.idStart), nIDs)
	checkIndex("id", add("id", idFirst, int(sb.idStart)), iidx)
	if int(sb.bytesUsed) != int(sb.idStart)+8*nIDs {
		t.Errorf("bytes_used %d, the id index ends at %d", sb.bytesUsed, int(sb.idStart)+8*nIDs)
	}
	for _, b := range img[sb.bytesUsed:] {
		if b != 0 {
			t.Errorf("non-zero padding after bytes_used")
			break
		}
	}
	return frames
}

// xzStreams collects every compressed (non-stored) block of an image: metadata blocks, fragment blocks and
// file data blocks, each a complete .xz stream, with the bytes it must decode to.
type xzBlock struct {
	stream, plain []byte
	what          string
}

func collectXZ(t testing.TB, img []byte, f *FS) []xzBlock {
	t.Helper()
	var out []xzBlock
	seen := map[int]bool{}
	for _, fr := range checkLayout(t, img) {
		if fr.stored {
			continue
		}
		plain, err := xzDecode(fr.payload, metaBlockSize)
		if err != nil {
			t.Fatalf("metadata block at %d: %v", fr.off, err)
		}
		out = append(out, xzBlock{fr.payload, plain, fmt.Sprintf("metadata block at %d", fr.off)})
		seen[fr.off] = true
	}
	bs := int(f.sb.BlockSize)
	add := func(what string, start int, word uint32, want int) {
		if word&(1<<24) != 0 || word&0xffffff == 0 || seen[start] {
			return
		}
		seen[start] = true
		raw := img[start : start+int(word&0xffffff)]
		plain, err := xzDecode(raw, bs)
		if err != nil {
			t.Fatalf("%s at %d: %v", what, start, err)
		}
		out = append(out, xzBlock{raw, plain, fmt.Sprintf("%s at %d", what, start)})
	}
	err := f.Walk("/", func(fi FileInfo) error {
		if fi.Type != TypeFile {
			return nil
		}
		in, err := f.readInode(fi.Ref, true)
		if err != nil {
			return err
		}
		pos := int(in.blocksStart)
		for _, w := range in.blockSizes {
			add("data block of "+fi.Path, pos, w, bs)
			pos += int(w & 0xffffff)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < int(f.sb.Fragments); i++ {
		blk, err := f.metaAt(int64(f.fragPtrs[i/512]), f.sb.lastUsed)
		if err != nil {
			t.Fatal(err)
		}
		o := i % 512 * 16
		add(fmt.Sprintf("fragment %d", i), int(binary.LittleEndian.Uint64(blk.data[o:])), binary.LittleEndian.Uint32(blk.data[o+8:]), bs)
	}
	return out
}

// checkXZParameters verifies what the kernel's xz decoder needs of one stream: stream flags 0 and a CRC32
// check, one LZMA2 filter whose declared dictionary is at most max(block size, 8 KiB).
func checkXZParameters(t testing.TB, b xzBlock, maxDict int64) {
	t.Helper()
	s := b.stream
	if len(s) < 24 || !bytes.Equal(s[:6], xzMagic[:]) || s[6] != 0 || s[7] != 0x01 {
		t.Errorf("%s: stream header % x is not 'xz, CRC32 check'", b.what, s[:min(8, len(s))])
		return
	}
	hs := (int(s[12]) + 1) * 4
	h := s[12 : 12+hs]
	flags := h[1]
	if flags&0x03 != 0 {
		t.Errorf("%s: more than one filter", b.what)
		return
	}
	p := h[2 : hs-4]
	if flags&0x40 != 0 {
		_, n, _ := xzVarint(p)
		p = p[n:]
	}
	if flags&0x80 != 0 {
		_, n, _ := xzVarint(p)
		p = p[n:]
	}
	if p[0] != 0x21 || p[1] != 1 {
		t.Errorf("%s: filter is not LZMA2 with one property byte", b.what)
		return
	}
	dict, err := lzma.DecodeDictCap(p[2])
	if err != nil || dict > maxDict {
		t.Errorf("%s: declared dictionary %d (%v), kernel maximum %d", b.what, dict, err, maxDict)
	}
}

// systemXZ decodes the concatenation of the streams with the xz program and compares with the expected
// plain text (skips when xz is not installed). Concatenated streams are valid input for xz.
func systemXZ(t *testing.T, blocks []xzBlock) {
	t.Helper()
	if _, err := exec.LookPath("xz"); err != nil {
		t.Skip("xz is not installed")
	}
	var in, want bytes.Buffer
	for _, b := range blocks {
		in.Write(b.stream)
		want.Write(b.plain)
	}
	cmd := exec.Command("xz", "-dc", "--no-warn")
	cmd.Stdin = &in
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		t.Fatalf("xz -dc of %d blocks: %v: %s", len(blocks), err, errb.String())
	}
	if !bytes.Equal(out.Bytes(), want.Bytes()) {
		t.Fatalf("xz decodes the %d blocks to %d bytes that differ from the expected %d", len(blocks), out.Len(), want.Len())
	}
	// every stream alone must pass `xz -t` too (the concatenation could hide a stream xz merely skips)
	step := max(1, len(blocks)/40)
	for i := 0; i < len(blocks); i += step {
		c := exec.Command("xz", "-t")
		c.Stdin = bytes.NewReader(blocks[i].stream)
		if o, err := c.CombinedOutput(); err != nil {
			t.Fatalf("xz -t of %s: %v: %s", blocks[i].what, err, o)
		}
	}
}

func unsquashfsText(t *testing.T, args ...string) string {
	t.Helper()
	return run(t, "unsquashfs", args...)
}

// sbLines are the `unsquashfs -s` lines that must be the same for two images of the same tree (all but the
// size of the filesystem).
func sbLines(s string) []string {
	var out []string
	for _, ln := range strings.Split(strings.TrimRight(s, "\n"), "\n") {
		if strings.HasPrefix(ln, "Filesystem size") || strings.HasPrefix(ln, "Found a valid") {
			continue
		}
		out = append(out, ln)
	}
	return out
}

func writeTemp(t testing.TB, name string, data []byte) string {
	t.Helper()
	p := t.TempDir() + "/" + name
	if err := os.WriteFile(p, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

// firstDiff describes the first differing byte of two images.
func firstDiff(a, b []byte) string {
	n := min(len(a), len(b))
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return fmt.Sprintf("first difference at byte %d (0x%x): %02x vs %02x (lengths %d and %d)", i, i, a[i], b[i], len(a), len(b))
		}
	}
	return fmt.Sprintf("same prefix, lengths %d and %d", len(a), len(b))
}

// appFakeCopy copies tests/fixtures/app_fake into a temp dir and creates the lib/libfake.so symlink there:
// the repository never tracks a symlink, but the image builders must keep the ones a real APP tree has.
func appFakeCopy(t testing.TB, src string) string {
	t.Helper()
	dst := t.TempDir()
	err := filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		target := filepath.Join(dst, rel)
		info, err := d.Info()
		if err != nil {
			return err
		}
		if d.IsDir() {
			return os.MkdirAll(target, info.Mode().Perm())
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		return os.WriteFile(target, b, info.Mode().Perm())
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("libfake.so.1", filepath.Join(dst, "lib", "libfake.so")); err != nil {
		t.Skipf("cannot create symlinks here: %v", err)
	}
	return dst
}
