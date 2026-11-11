package squashfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/bits"
	"sort"
	"strings"
)

// The writer produces squashfs 4.0 images with the layout of mksquashfs (4.x) for the subset the assistant
// needs: xz blocks with a CRC32 check and a dictionary equal to the block size (at least 8 KiB, so that the
// kernel's xz decoder, which preallocates its dictionary from the block size, accepts every stream), no
// compressor options block, all inode kinds in their basic form, fragments for the files smaller than a block,
// sparse zero blocks, duplicate removal, the id table and the export (lookup) table, no extended attributes.
// The output is a pure function of the tree and the options: same input, same bytes.
//
// Layout (as mksquashfs): superblock | file data blocks and fragment blocks (in processing order) | inode
// table | directory table | fragment table | export table | id table | zero padding to 4096. Each table is a
// run of metadata blocks followed by the array of their absolute offsets; the superblock points at the arrays.
// Inodes are written children first (a directory comes after its content) and numbered in pre-order, the root
// being the last number.

// Errors specific to the writer (they wrap or are returned next to the reader's ErrLimit).
var (
	// ErrInvalidTree reports a Node tree that cannot be written as it is.
	ErrInvalidTree = errors.New("squashfs: invalid tree")
	// ErrHardLink reports a Node reachable twice in the tree (a hard link): not supported.
	ErrHardLink = errors.New("squashfs: hard links are not supported")
)

const (
	defaultBlockSize = 131072
	padSize          = 4096
	maxSymlinkWrite  = 4096
	maxIDs           = 1 << 16
)

// Node is one entry of the in-memory tree given to Write. The library never touches the host file system:
// the caller builds the tree (an image read back with FS.Tree, a synthetic tree in a test, a patched copy).
//
// Mode holds the permission bits plus setuid, setgid and sticky (07777). A Node must appear once in the tree:
// pointing at the same Node from two directories (a hard link) is refused with ErrHardLink. The root is a
// TypeDir node whose Name is ignored. Children may be in any order: the writer sorts them by name (bytes).
type Node struct {
	Name     string
	Type     FileType
	Mode     uint16
	UID, GID uint32
	MTime    uint32
	Data     []byte  // TypeFile: the content
	Target   string  // TypeSymlink: the link target
	Major    uint32  // TypeBlock and TypeChar: the device numbers
	Minor    uint32  // (Major < 4096, Minor < 1048576)
	Children []*Node // TypeDir
}

// NewDir returns an empty directory node.
func NewDir(name string, mode uint16) *Node { return &Node{Name: name, Type: TypeDir, Mode: mode} }

// NewFile returns a regular file node (the data is not copied).
func NewFile(name string, mode uint16, data []byte) *Node {
	return &Node{Name: name, Type: TypeFile, Mode: mode, Data: data}
}

// NewSymlink returns a symbolic link node.
func NewSymlink(name, target string) *Node {
	return &Node{Name: name, Type: TypeSymlink, Mode: 0o777, Target: target}
}

// NewDevice returns a block (typ TypeBlock) or character (TypeChar) device node.
func NewDevice(name string, typ FileType, mode uint16, major, minor uint32) *Node {
	return &Node{Name: name, Type: typ, Mode: mode, Major: major, Minor: minor}
}

// Add appends children to a directory node and returns it, for chaining.
func (n *Node) Add(children ...*Node) *Node {
	n.Children = append(n.Children, children...)
	return n
}

// Options tune Write. The zero value gives the images of the assistant: 128 KiB blocks, padding to 4096.
type Options struct {
	// BlockSize is the data block size: a power of two from 4096 to 1 MiB. Default 131072.
	BlockSize int
	// Time is the creation time written in the superblock; with AllTime it is also the modification time
	// of every inode (mksquashfs -all-time), otherwise each inode gets Node.MTime.
	Time    uint32
	AllTime bool
	// NoPad leaves out the zero padding to a multiple of 4096 bytes mksquashfs adds.
	NoPad bool
	// MaxSize, when positive, fails with ErrLimit if the image (padding included) would be larger.
	MaxSize int64

	// noCompress stores every block (inode, directory, fragment, data, id): tests only, it makes the
	// image comparable byte for byte with `mksquashfs -noI -noD -noF -noX`.
	noCompress bool
}

// Stats describes what Write produced.
type Stats struct {
	Info
	// Size is the number of bytes written (BytesUsed plus the padding).
	Size int64
	// DataBytes is the size of the file data and fragment blocks.
	DataBytes int64
}

type fileLoc struct {
	start   uint64
	blocks  []uint32
	fragIdx uint32
	fragOff uint32
}

type wnode struct {
	name     string
	typ      FileType
	mode     uint16
	uid, gid uint16 // indexes in the id table
	mtime    uint32
	ino      uint32
	parent   *wnode
	kids     []*wnode
	src      *Node
	nlink    uint32

	loc fileLoc // regular files
	// ref is the inode reference (compressed block offset in the inode table << 16 | offset)
	ref uint64
}

type fragEntry struct {
	start uint64
	word  uint32
}

type writer struct {
	o    Options
	bs   int
	dict int
	out  []byte

	ids    []uint32
	idIdx  map[uint32]uint16
	nInode uint32
	nodes  []*wnode // by inode number - 1

	frag     []byte
	fragEnts []fragEntry
	dups     map[[32]byte]fileLoc

	inode, dir *metaWriter
}

// Write writes the image of the tree rooted at root to w and returns what it wrote. root must be a directory.
func Write(w io.Writer, root *Node, o Options) (Stats, error) {
	img, st, err := build(root, o)
	if err != nil {
		return Stats{}, err
	}
	if _, err := w.Write(img); err != nil {
		return Stats{}, err
	}
	return st, nil
}

// Build is Write into memory.
func Build(root *Node, o Options) ([]byte, Stats, error) { return build(root, o) }

func invalid(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrInvalidTree, fmt.Sprintf(format, a...))
}

func build(root *Node, o Options) ([]byte, Stats, error) {
	if o.BlockSize == 0 {
		o.BlockSize = defaultBlockSize
	}
	if bs := o.BlockSize; bs < minBlockSize || bs > maxBlockSize || bs&(bs-1) != 0 {
		return nil, Stats{}, fmt.Errorf("squashfs: block size %d is not a power of two between %d and %d", bs, minBlockSize, maxBlockSize)
	}
	w := &writer{o: o, bs: o.BlockSize, idIdx: map[uint32]uint16{}, dups: map[[32]byte]fileLoc{}}
	w.dict = max(w.bs, metaBlockSize)
	if root == nil || root.Type != TypeDir {
		return nil, Stats{}, invalid("the root is not a directory")
	}
	rn, err := w.convert(root, nil, map[*Node]bool{}, 0, true)
	if err != nil {
		return nil, Stats{}, err
	}
	// inode numbers: pre-order, the root last
	var number func(n *wnode)
	number = func(n *wnode) {
		for _, k := range n.kids {
			w.nInode++
			k.ino = w.nInode
			w.nodes = append(w.nodes, k)
			number(k)
		}
	}
	number(rn)
	w.nInode++
	rn.ino = w.nInode
	w.nodes = append(w.nodes, rn)
	if uint64(w.nInode) >= 1<<32-1 {
		return nil, Stats{}, fmt.Errorf("%w: too many inodes", ErrLimit)
	}

	w.out = make([]byte, sbSize)
	w.inode, w.dir = newMeta(w), newMeta(w)
	if err := w.writeData(rn); err != nil {
		return nil, Stats{}, err
	}
	w.flushFragment()
	dataEnd := len(w.out)
	if err := w.writeInodes(rn); err != nil {
		return nil, Stats{}, err
	}

	// tables
	inodeStart := len(w.out)
	w.out = append(w.out, w.inode.finish()...)
	dirStart := len(w.out)
	w.out = append(w.out, w.dir.finish()...)

	fragRaw := make([]byte, 0, len(w.fragEnts)*16)
	for _, f := range w.fragEnts {
		fragRaw = binary.LittleEndian.AppendUint64(fragRaw, f.start)
		fragRaw = binary.LittleEndian.AppendUint32(fragRaw, f.word)
		fragRaw = binary.LittleEndian.AppendUint32(fragRaw, 0)
	}
	fragStart := w.writeTable(fragRaw)

	lookup := make([]byte, 0, len(w.nodes)*8)
	for _, n := range w.nodes {
		lookup = binary.LittleEndian.AppendUint64(lookup, n.ref)
	}
	lookupStart := w.writeTable(lookup)

	idRaw := make([]byte, 0, len(w.ids)*4)
	for _, id := range w.ids {
		idRaw = binary.LittleEndian.AppendUint32(idRaw, id)
	}
	idStart := w.writeTable(idRaw)

	used := len(w.out)
	if !o.NoPad {
		w.out = append(w.out, make([]byte, (padSize-used%padSize)%padSize)...)
	}
	if o.MaxSize > 0 && int64(len(w.out)) > o.MaxSize {
		return nil, Stats{}, fmt.Errorf("%w: image of %d bytes exceeds %d", ErrLimit, len(w.out), o.MaxSize)
	}

	flags := uint16(FlagDuplicates | FlagExportable | FlagNoXattrs)
	if o.noCompress {
		flags |= FlagUncompressedInodes | FlagUncompressedData | FlagUncompressedFrags
	}
	le := binary.LittleEndian
	sb := w.out[:sbSize]
	le.PutUint32(sb[0:], magic)
	le.PutUint32(sb[4:], w.nInode)
	le.PutUint32(sb[8:], o.Time)
	le.PutUint32(sb[12:], uint32(w.bs))
	le.PutUint32(sb[16:], uint32(len(w.fragEnts)))
	le.PutUint16(sb[20:], CompXZ)
	le.PutUint16(sb[22:], uint16(bits.TrailingZeros32(uint32(w.bs))))
	le.PutUint16(sb[24:], flags)
	le.PutUint16(sb[26:], uint16(len(w.ids)))
	le.PutUint16(sb[28:], 4)
	le.PutUint16(sb[30:], 0)
	le.PutUint64(sb[32:], rn.ref)
	le.PutUint64(sb[40:], uint64(used))
	le.PutUint64(sb[48:], uint64(idStart))
	le.PutUint64(sb[56:], noTable) // no xattr id table
	le.PutUint64(sb[64:], uint64(inodeStart))
	le.PutUint64(sb[72:], uint64(dirStart))
	le.PutUint64(sb[80:], uint64(fragStart))
	le.PutUint64(sb[88:], uint64(lookupStart))

	st := Stats{Size: int64(len(w.out)), DataBytes: int64(dataEnd - sbSize), Info: Info{
		Inodes: w.nInode, MkfsTime: o.Time, BlockSize: uint32(w.bs), Fragments: uint32(len(w.fragEnts)),
		Compression: CompXZ, Flags: flags, IDs: uint16(len(w.ids)), BytesUsed: uint64(used), HasExportTable: true,
	}}
	return w.out, st, nil
}

// writeTable appends a table (metadata blocks, then the array of their absolute offsets) and returns the
// absolute offset of the array.
func (w *writer) writeTable(raw []byte) int {
	m := newMeta(w)
	m.write(raw)
	blocks := m.finish()
	base := len(w.out)
	w.out = append(w.out, blocks...)
	idx := len(w.out)
	n := (len(raw) + metaBlockSize - 1) / metaBlockSize
	for i := 0; i < n; i++ {
		w.out = binary.LittleEndian.AppendUint64(w.out, uint64(base+m.starts[i]))
	}
	return idx
}

// convert validates src and builds the writer's tree (children sorted by name).
func (w *writer) convert(src *Node, parent *wnode, seen map[*Node]bool, depth int, isRoot bool) (*wnode, error) {
	if seen[src] {
		return nil, fmt.Errorf("%w: %q is reachable twice", ErrHardLink, src.Name)
	}
	seen[src] = true
	if depth > 4096 {
		return nil, invalid("directories nested deeper than 4096")
	}
	name := src.Name
	if !isRoot {
		if name == "" || name == "." || name == ".." || strings.ContainsAny(name, "/\x00") || len(name) > maxNameLen {
			return nil, invalid("entry name %q", name)
		}
	}
	if src.Mode&^0o7777 != 0 {
		return nil, invalid("%q: mode %o has bits outside 07777", name, src.Mode)
	}
	n := &wnode{name: name, typ: src.Type, mode: src.Mode, mtime: src.MTime, parent: parent, src: src, nlink: 1}
	if w.o.AllTime {
		n.mtime = w.o.Time
	}
	var err error
	if n.uid, err = w.id(src.UID); err != nil {
		return nil, err
	}
	if n.gid, err = w.id(src.GID); err != nil {
		return nil, err
	}
	switch src.Type {
	case TypeDir:
		kids := append([]*Node(nil), src.Children...)
		sort.SliceStable(kids, func(i, j int) bool { return kids[i].Name < kids[j].Name })
		n.nlink = 2
		for i, k := range kids {
			if k == nil {
				return nil, invalid("nil child in %q", name)
			}
			if i > 0 && kids[i-1].Name == k.Name {
				return nil, invalid("%q: duplicate entry %q", name, k.Name)
			}
			c, err := w.convert(k, n, seen, depth+1, false)
			if err != nil {
				return nil, err
			}
			if c.typ == TypeDir {
				n.nlink++
			}
			n.kids = append(n.kids, c)
		}
	case TypeFile:
		if uint64(len(src.Data)) >= 1<<32 {
			return nil, invalid("%q: file of %d bytes", name, len(src.Data))
		}
	case TypeSymlink:
		if src.Target == "" || len(src.Target) > maxSymlinkWrite {
			return nil, invalid("%q: symlink target of %d bytes", name, len(src.Target))
		}
	case TypeBlock, TypeChar:
		if src.Major >= 1<<12 || src.Minor >= 1<<20 {
			return nil, invalid("%q: device %d,%d", name, src.Major, src.Minor)
		}
	case TypeFifo, TypeSocket:
	default:
		return nil, invalid("%q: type %d", name, src.Type)
	}
	if src.Type != TypeDir && len(src.Children) > 0 {
		return nil, invalid("%q: children on a %v", name, src.Type)
	}
	return n, nil
}

func (w *writer) id(v uint32) (uint16, error) {
	if i, ok := w.idIdx[v]; ok {
		return i, nil
	}
	if len(w.ids) >= maxIDs {
		return 0, fmt.Errorf("%w: more than %d distinct uid/gid values", ErrLimit, maxIDs)
	}
	i := uint16(len(w.ids))
	w.ids = append(w.ids, v)
	w.idIdx[v] = i
	return i, nil
}

func allZero8(b []byte) bool {
	for len(b) >= 8 {
		if binary.LittleEndian.Uint64(b) != 0 {
			return false
		}
		b = b[8:]
	}
	return allZero(b)
}

// writeData stores the content of the regular files in pre-order (children by name) and records where.
func (w *writer) writeData(n *wnode) error {
	for _, k := range n.kids {
		switch k.typ {
		case TypeDir:
			if err := w.writeData(k); err != nil {
				return err
			}
		case TypeFile:
			if err := w.storeFile(k); err != nil {
				return err
			}
		}
	}
	return nil
}

func (w *writer) storeFile(n *wnode) error {
	data := n.src.Data
	n.loc = fileLoc{fragIdx: noFragment}
	if len(data) == 0 {
		return nil
	}
	key := sha256.Sum256(data)
	if loc, ok := w.dups[key]; ok {
		n.loc = loc
		return nil
	}
	size := len(data)
	switch {
	case size < w.bs && !allZero8(data):
		// the whole file goes to a fragment
		if len(w.frag)+size > w.bs {
			w.flushFragment()
		}
		n.loc.fragIdx = uint32(len(w.fragEnts))
		n.loc.fragOff = uint32(len(w.frag))
		w.frag = append(w.frag, data...)
	default:
		n.loc.start = uint64(len(w.out))
		for off := 0; off < size; off += w.bs {
			blk := data[off:min(off+w.bs, size)]
			if allZero8(blk) {
				n.loc.blocks = append(n.loc.blocks, 0)
				continue
			}
			out, stored := w.compress(blk)
			word := uint32(len(out))
			if stored {
				word |= 1 << 24
			}
			n.loc.blocks = append(n.loc.blocks, word)
			w.out = append(w.out, out...)
		}
	}
	w.dups[key] = n.loc
	return nil
}

func (w *writer) flushFragment() {
	if len(w.frag) == 0 {
		return
	}
	out, stored := w.compress(w.frag)
	word := uint32(len(out))
	if stored {
		word |= 1 << 24
	}
	w.fragEnts = append(w.fragEnts, fragEntry{start: uint64(len(w.out)), word: word})
	w.out = append(w.out, out...)
	w.frag = w.frag[:0]
}

// compress returns the xz stream of src, or src itself (stored true) when that is not smaller. Every call
// makes an independent xz stream (one block, CRC32 check), as the kernel reads each squashfs block alone.
func (w *writer) compress(src []byte) (out []byte, stored bool) {
	if w.o.noCompress {
		return src, true
	}
	out, err := xzEncode(src, w.dict)
	if err != nil { // only a bug in the encoder configuration
		panic(fmt.Sprintf("squashfs: xz encoder: %v", err))
	}
	if len(out) >= len(src) {
		return src, true
	}
	return out, false
}

func rdevOf(major, minor uint32) uint32 {
	return minor&0xff | major<<8 | (minor&^0xff)<<12
}

func (w *writer) inodeHeader(buf []byte, n *wnode, typ uint16) []byte {
	le := binary.LittleEndian
	buf = le.AppendUint16(buf, typ)
	buf = le.AppendUint16(buf, n.mode)
	buf = le.AppendUint16(buf, n.uid)
	buf = le.AppendUint16(buf, n.gid)
	buf = le.AppendUint32(buf, n.mtime)
	return le.AppendUint32(buf, n.ino)
}

// writeInodes writes the inodes and directory listings, a directory after its content.
func (w *writer) writeInodes(d *wnode) error {
	for _, k := range d.kids {
		if k.typ == TypeDir {
			if err := w.writeInodes(k); err != nil {
				return err
			}
		} else {
			w.writeInode(k)
		}
	}
	return w.writeDir(d)
}

func (w *writer) setRef(n *wnode) {
	blk, off := w.inode.pos()
	n.ref = uint64(blk)<<16 | uint64(off)
}

func (w *writer) writeInode(n *wnode) {
	le := binary.LittleEndian
	w.setRef(n)
	var b []byte
	switch n.typ {
	case TypeFile:
		b = w.inodeHeader(b, n, uint16(TypeFile))
		start := uint32(n.loc.start)
		b = le.AppendUint32(b, start)
		b = le.AppendUint32(b, n.loc.fragIdx)
		off := uint32(0)
		if n.loc.fragIdx != noFragment {
			off = n.loc.fragOff
		}
		b = le.AppendUint32(b, off)
		b = le.AppendUint32(b, uint32(len(n.src.Data)))
		for _, bl := range n.loc.blocks {
			b = le.AppendUint32(b, bl)
		}
	case TypeSymlink:
		b = w.inodeHeader(b, n, uint16(TypeSymlink))
		b = le.AppendUint32(b, 1)
		b = le.AppendUint32(b, uint32(len(n.src.Target)))
		b = append(b, n.src.Target...)
	case TypeBlock, TypeChar:
		b = w.inodeHeader(b, n, uint16(n.typ))
		b = le.AppendUint32(b, 1)
		b = le.AppendUint32(b, rdevOf(n.src.Major, n.src.Minor))
	default: // fifo, socket
		b = w.inodeHeader(b, n, uint16(n.typ))
		b = le.AppendUint32(b, 1)
	}
	w.inode.write(b)
}

type dirIndex struct {
	pos  int // offset of the header in the listing
	name string
}

// writeDir writes the listing of d to the directory table and then d's inode.
func (w *writer) writeDir(d *wnode) error {
	le := binary.LittleEndian
	var list []byte
	var index []dirIndex
	var hdr int // offset of the current header in list
	var (
		count    int
		startBlk uint32
		baseIno  uint32
		run      int // bytes since the start of the listing or of the last index point
	)
	for _, k := range d.kids {
		blk := uint32(k.ref >> 16)
		off := uint16(k.ref & 0xffff)
		need := 8 + len(k.name)
		newHdr := count == 0 || count == 256 || blk != startBlk ||
			int64(k.ino)-int64(baseIno) > 32767 || int64(k.ino)-int64(baseIno) < -32768
		cut := false
		if run+need > metaBlockSize && run > 0 {
			// one index entry per run of at most one metadata block of listing, as mksquashfs does
			newHdr, cut = true, true
		}
		if newHdr {
			if count > 0 {
				le.PutUint32(list[hdr:], uint32(count-1))
			}
			if cut {
				index = append(index, dirIndex{pos: len(list), name: k.name})
				run = 0
			}
			hdr = len(list)
			list = le.AppendUint32(list, 0)
			list = le.AppendUint32(list, blk)
			list = le.AppendUint32(list, k.ino)
			run += 12
			count, startBlk, baseIno = 0, blk, k.ino
		}
		list = le.AppendUint16(list, off)
		list = le.AppendUint16(list, uint16(int16(int64(k.ino)-int64(baseIno))))
		list = le.AppendUint16(list, uint16(k.typ))
		list = le.AppendUint16(list, uint16(len(k.name)-1))
		list = append(list, k.name...)
		run += need
		count++
	}
	if count > 0 {
		le.PutUint32(list[hdr:], uint32(count-1))
	}
	if len(list)+3 > 1<<32-1 {
		return fmt.Errorf("%w: directory listing too large", ErrLimit)
	}

	b0, off0 := w.dir.pos()
	b0idx := len(w.dir.starts) - 1
	w.dir.write(list)

	w.setRef(d)
	parent := d.ino
	if d.parent != nil {
		parent = d.parent.ino
	} else {
		parent = w.nInode + 1
	}
	var b []byte
	size := len(list) + 3
	if len(index) == 0 && size <= 0xffff && len(d.kids) <= 256 {
		b = w.inodeHeader(b, d, uint16(TypeDir))
		b = le.AppendUint32(b, uint32(b0))
		b = le.AppendUint32(b, d.nlink)
		b = le.AppendUint16(b, uint16(size))
		b = le.AppendUint16(b, uint16(off0))
		b = le.AppendUint32(b, parent)
	} else {
		b = w.inodeHeader(b, d, uint16(TypeDir)+7)
		b = le.AppendUint32(b, d.nlink)
		b = le.AppendUint32(b, uint32(size))
		b = le.AppendUint32(b, uint32(b0))
		b = le.AppendUint32(b, parent)
		b = le.AppendUint16(b, uint16(len(index)))
		b = le.AppendUint16(b, uint16(off0))
		b = le.AppendUint32(b, noXattr)
		for _, ix := range index {
			blkOf := w.dir.starts[b0idx+(off0+ix.pos)/metaBlockSize]
			b = le.AppendUint32(b, uint32(ix.pos))
			b = le.AppendUint32(b, uint32(blkOf))
			b = le.AppendUint32(b, uint32(len(ix.name)-1))
			b = append(b, ix.name...)
		}
	}
	w.inode.write(b)
	return nil
}

// xzEncode makes a complete .xz file (one block, LZMA2, CRC32 check, dictionary dict) of src.
func xzEncode(src []byte, dict int) ([]byte, error) {
	var buf bytes.Buffer
	zw, err := newXZWriter(&buf, dict)
	if err != nil {
		return nil, err
	}
	if _, err := zw.Write(src); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// metaWriter builds a run of metadata blocks (8192 uncompressed bytes each, framed with a 2 byte size).
type metaWriter struct {
	w      *writer
	buf    []byte
	out    []byte
	starts []int // offset of the start of every block begun: the flushed ones, then the current one
}

func newMeta(w *writer) *metaWriter {
	return &metaWriter{w: w, starts: []int{0}}
}

// pos is where the next byte goes: the offset (in the framed run) of the current block and the offset in it.
func (m *metaWriter) pos() (blk, off int) { return m.starts[len(m.starts)-1], len(m.buf) }

func (m *metaWriter) write(p []byte) {
	for len(p) > 0 {
		n := min(metaBlockSize-len(m.buf), len(p))
		m.buf = append(m.buf, p[:n]...)
		p = p[n:]
		if len(m.buf) == metaBlockSize {
			m.flush()
		}
	}
}

func (m *metaWriter) flush() {
	out, stored := m.w.compress(m.buf)
	h := uint16(len(out))
	if stored {
		h |= 0x8000
	}
	m.out = binary.LittleEndian.AppendUint16(m.out, h)
	m.out = append(m.out, out...)
	m.buf = m.buf[:0]
	m.starts = append(m.starts, len(m.out))
}

// finish returns the framed blocks, the last one possibly shorter than 8192 bytes.
func (m *metaWriter) finish() []byte {
	if len(m.buf) > 0 {
		m.flush()
	}
	return m.out
}
