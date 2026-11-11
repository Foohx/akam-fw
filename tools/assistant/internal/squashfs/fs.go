package squashfs

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"math/bits"
	"sync"
	"sync/atomic"
)

const (
	sbSize        = 96
	magic         = 0x73717368
	metaBlockSize = 8192
	noFragment    = 0xffffffff
	noTable       = ^uint64(0)
	noXattr       = 0xffffffff
	minBlockSize  = 4096
	maxBlockSize  = 1 << 20
	maxXzDict     = 8 << 20
	maxSymlink    = 4096
	maxNameLen    = 256
)

// Compression identifiers of the superblock.
const (
	CompGzip = 1
	CompLZMA = 2
	CompLZO  = 3
	CompXZ   = 4
	CompLZ4  = 5
	CompZstd = 6
)

// Superblock flags.
const (
	FlagUncompressedInodes = 0x1
	FlagUncompressedData   = 0x2
	FlagUncompressedFrags  = 0x8
	FlagNoFragments        = 0x10
	FlagAlwaysFragments    = 0x20
	FlagDuplicates         = 0x40
	FlagExportable         = 0x80
	FlagUncompressedXattrs = 0x100
	FlagNoXattrs           = 0x200
	FlagCompressorOptions  = 0x400
	FlagUncompressedIDs    = 0x800
)

// Info is the decoded superblock of an image.
type Info struct {
	Inodes         uint32
	MkfsTime       uint32
	BlockSize      uint32
	Fragments      uint32
	Compression    int
	Flags          uint16
	IDs            uint16
	BytesUsed      uint64
	HasXattrTable  bool
	HasExportTable bool
	// XzDictSize is the dictionary size of the xz compressor options (0 when absent).
	XzDictSize uint32
}

type superblock struct {
	Info
	root                       uint64
	idStart, xattrStart        uint64
	inodeStart, dirStart       uint64
	fragStart, lookupStart     uint64
	inodeEnd, dirEnd, lastUsed int64
}

// FS is an opened image. It is safe for concurrent use.
type FS struct {
	r    io.ReaderAt
	size int64
	lim  Limits
	sb   superblock

	ids      []uint32
	fragPtrs []uint64

	mu        sync.Mutex
	meta      map[int64]*metaBlock
	metaBytes int64
	fragIdx   uint32
	fragData  []byte
	fragOK    bool
}

type metaBlock struct {
	data []byte
	next int64
}

// Info returns the superblock facts.
func (f *FS) Info() Info { return f.sb.Info }

// Open reads and validates the superblock and the id, fragment and export table indexes of the image of the
// given size, with DefaultLimits.
func Open(r io.ReaderAt, size int64) (*FS, error) { return OpenLimits(r, size, Limits{}) }

// OpenLimits is Open with explicit limits (zero fields take their default).
func OpenLimits(r io.ReaderAt, size int64, lim Limits) (*FS, error) {
	lim = lim.filled()
	if size < sbSize {
		return nil, corrupt("image of %d bytes is smaller than a superblock", size)
	}
	if size > lim.MaxImageSize {
		return nil, limited("image of %d bytes exceeds %d", size, lim.MaxImageSize)
	}
	f := &FS{r: r, size: size, lim: lim, meta: map[int64]*metaBlock{}}
	var raw [sbSize]byte
	if err := f.readAt(raw[:], 0, size); err != nil {
		return nil, err
	}
	if err := f.parseSuperblock(raw[:]); err != nil {
		return nil, err
	}
	if err := f.loadTables(); err != nil {
		return nil, err
	}
	root, err := f.readInode(f.sb.root, false)
	if err != nil {
		return nil, fmt.Errorf("root inode: %w", err)
	}
	if root.typ != typeDir {
		return nil, corrupt("root inode is not a directory")
	}
	return f, nil
}

func (f *FS) readAt(p []byte, off, limit int64) error {
	if off < 0 || int64(len(p)) > limit-off {
		return corrupt("read of %d bytes at %d is outside the image (%d)", len(p), off, limit)
	}
	n, err := f.r.ReadAt(p, off)
	if n == len(p) {
		return nil
	}
	if err == nil || err == io.EOF {
		err = io.ErrUnexpectedEOF
	}
	return fmt.Errorf("%w: reading image: %v", ErrCorrupt, err)
}

func (f *FS) parseSuperblock(b []byte) error {
	le := binary.LittleEndian
	if le.Uint32(b[0:]) != magic {
		return corrupt("bad magic")
	}
	sb := &f.sb
	sb.Inodes = le.Uint32(b[4:])
	sb.MkfsTime = le.Uint32(b[8:])
	sb.BlockSize = le.Uint32(b[12:])
	sb.Fragments = le.Uint32(b[16:])
	sb.Compression = int(le.Uint16(b[20:]))
	blockLog := le.Uint16(b[22:])
	sb.Flags = le.Uint16(b[24:])
	sb.IDs = le.Uint16(b[26:])
	major, minor := le.Uint16(b[28:]), le.Uint16(b[30:])
	sb.root = le.Uint64(b[32:])
	sb.BytesUsed = le.Uint64(b[40:])
	sb.idStart = le.Uint64(b[48:])
	sb.xattrStart = le.Uint64(b[56:])
	sb.inodeStart = le.Uint64(b[64:])
	sb.dirStart = le.Uint64(b[72:])
	sb.fragStart = le.Uint64(b[80:])
	sb.lookupStart = le.Uint64(b[88:])

	if major != 4 || minor != 0 {
		return unsupported("squashfs version %d.%d (only 4.0)", major, minor)
	}
	bs := sb.BlockSize
	if bs < minBlockSize || bs > maxBlockSize || bs&(bs-1) != 0 || uint32(blockLog) != uint32(bits.TrailingZeros32(bs)) {
		return corrupt("block size %d (log %d) is not valid", bs, blockLog)
	}
	if sb.Inodes == 0 || sb.Inodes > f.lim.MaxInodes {
		return limited("inode count %d (cap %d)", sb.Inodes, f.lim.MaxInodes)
	}
	if sb.Fragments > f.lim.MaxInodes {
		return limited("fragment count %d (cap %d)", sb.Fragments, f.lim.MaxInodes)
	}
	if sb.IDs == 0 {
		return corrupt("empty id table")
	}
	if sb.BytesUsed < sbSize || sb.BytesUsed > uint64(f.size) {
		return corrupt("bytes used %d is outside the image (%d)", sb.BytesUsed, f.size)
	}
	used := int64(sb.BytesUsed)
	f.sb.lastUsed = used
	sb.HasXattrTable = sb.xattrStart != noTable
	sb.HasExportTable = sb.Flags&FlagExportable != 0 && sb.lookupStart != noTable

	first := int64(sbSize)
	switch sb.Compression {
	case CompGzip, CompXZ:
	case CompLZMA, CompLZO, CompLZ4, CompZstd:
		return unsupported("%s compression", compName(sb.Compression))
	default:
		return unsupported("compression id %d", sb.Compression)
	}
	if sb.Flags&FlagCompressorOptions != 0 {
		blk, err := f.rawMeta(sbSize, used, true)
		if err != nil {
			return fmt.Errorf("compressor options: %w", err)
		}
		first = blk.next
		switch sb.Compression {
		case CompXZ:
			if len(blk.data) < 8 {
				return corrupt("xz compressor options of %d bytes", len(blk.data))
			}
			dict := binary.LittleEndian.Uint32(blk.data)
			if filters := binary.LittleEndian.Uint32(blk.data[4:]); filters != 0 {
				return unsupported("xz filters 0x%x (BCJ)", filters)
			}
			if dict < 4096 || dict > maxXzDict {
				return unsupported("xz dictionary of %d bytes", dict)
			}
			sb.XzDictSize = dict
		case CompGzip:
			if len(blk.data) < 8 {
				return corrupt("gzip compressor options of %d bytes", len(blk.data))
			}
		}
	}

	if uint64(first) > uint64(sb.inodeStart) || sb.inodeStart >= sb.dirStart || sb.dirStart > sb.BytesUsed {
		return corrupt("inode table at %d, directory table at %d is not valid", sb.inodeStart, sb.dirStart)
	}
	f.sb.inodeEnd = int64(sb.dirStart)
	// The directory table ends where the next table begins (the fragment, export, id and xattr indexes
	// follow it; the fragment metadata blocks come first, so this bound is loose but safe).
	end := used
	for _, s := range []uint64{sb.fragStart, sb.lookupStart, sb.idStart, sb.xattrStart} {
		if s != noTable && s > sb.dirStart && int64(s) < end {
			end = int64(s)
		}
	}
	f.sb.dirEnd = end
	if sb.Fragments > 0 {
		if sb.fragStart == noTable || sb.fragStart >= sb.BytesUsed {
			return corrupt("fragment table start %d", sb.fragStart)
		}
	}
	if sb.idStart == noTable || sb.idStart >= sb.BytesUsed {
		return corrupt("id table start %d", sb.idStart)
	}
	if sb.Flags&FlagExportable != 0 && (sb.lookupStart == noTable || sb.lookupStart >= sb.BytesUsed) {
		return corrupt("exportable image without a valid export table (%d)", sb.lookupStart)
	}
	if sb.root>>16 >= sb.dirStart-sb.inodeStart || sb.root&0xffff >= metaBlockSize {
		return corrupt("root inode reference 0x%x", sb.root)
	}
	return nil
}

func compName(id int) string {
	switch id {
	case CompGzip:
		return "gzip"
	case CompLZMA:
		return "lzma"
	case CompLZO:
		return "lzo"
	case CompXZ:
		return "xz"
	case CompLZ4:
		return "lz4"
	case CompZstd:
		return "zstd"
	}
	return fmt.Sprintf("compression %d", id)
}

// CompressionName returns "xz", "gzip", "lzo"... for the compression of the image.
func (i Info) CompressionName() string { return compName(i.Compression) }

// loadTables reads the id table and the fragment index; the export table is only range-checked here.
func (f *FS) loadTables() error {
	sb := &f.sb
	used := f.sb.lastUsed
	ptrs, err := f.readIndex(sb.idStart, int(sb.IDs), 2048)
	if err != nil {
		return fmt.Errorf("id table: %w", err)
	}
	f.ids = make([]uint32, 0, sb.IDs)
	for i, p := range ptrs {
		n := min(2048, int(sb.IDs)-i*2048)
		blk, err := f.metaAt(int64(p), used)
		if err != nil {
			return fmt.Errorf("id table: %w", err)
		}
		if len(blk.data) < n*4 {
			return corrupt("id table block of %d bytes holds fewer than %d ids", len(blk.data), n)
		}
		for j := 0; j < n; j++ {
			f.ids = append(f.ids, binary.LittleEndian.Uint32(blk.data[j*4:]))
		}
	}
	if sb.Fragments > 0 {
		if f.fragPtrs, err = f.readIndex(sb.fragStart, int(sb.Fragments), 512); err != nil {
			return fmt.Errorf("fragment table: %w", err)
		}
	}
	if sb.HasExportTable {
		if _, err = f.readIndex(sb.lookupStart, int(sb.Inodes), 1024); err != nil {
			return fmt.Errorf("export table: %w", err)
		}
	}
	return nil
}

// readIndex reads the array of metadata block pointers of a table of count entries, perBlock per block.
func (f *FS) readIndex(start uint64, count, perBlock int) ([]uint64, error) {
	n := (count + perBlock - 1) / perBlock
	used := f.sb.lastUsed
	if start > uint64(used) || uint64(n)*8 > uint64(used)-start {
		return nil, corrupt("index of %d pointers at %d is outside the image", n, start)
	}
	buf := make([]byte, n*8)
	if err := f.readAt(buf, int64(start), used); err != nil {
		return nil, err
	}
	out := make([]uint64, n)
	for i := range out {
		out[i] = binary.LittleEndian.Uint64(buf[i*8:])
		if out[i] < sbSize || out[i] >= start {
			return nil, corrupt("table block pointer %d is outside the image or after its index", out[i])
		}
	}
	return out, nil
}

// ExportTable returns the inode reference of inode number i+1 at index i. It fails if the image has no
// export table.
func (f *FS) ExportTable() ([]uint64, error) {
	if !f.sb.HasExportTable {
		return nil, fmt.Errorf("squashfs: the image has no export table")
	}
	n := int(f.sb.Inodes)
	ptrs, err := f.readIndex(f.sb.lookupStart, n, 1024)
	if err != nil {
		return nil, err
	}
	out := make([]uint64, 0, n)
	for i, p := range ptrs {
		blk, err := f.metaAt(int64(p), f.sb.lastUsed)
		if err != nil {
			return nil, err
		}
		k := min(1024, n-i*1024)
		if len(blk.data) < k*8 {
			return nil, corrupt("export table block of %d bytes holds fewer than %d entries", len(blk.data), k)
		}
		for j := 0; j < k; j++ {
			out = append(out, binary.LittleEndian.Uint64(blk.data[j*8:]))
		}
	}
	return out, nil
}

// metaAt decodes the metadata block at abs; its raw bytes must end before end.
func (f *FS) metaAt(abs, end int64) (*metaBlock, error) {
	return f.rawMeta(abs, end, false)
}

// rawMeta decodes the metadata block at abs. needStored makes a compressed block an error (the compressor
// options block is always stored).
func (f *FS) rawMeta(abs, end int64, needStored bool) (*metaBlock, error) {
	if abs < sbSize || abs > end-2 {
		return nil, corrupt("metadata block at %d is outside [%d, %d)", abs, sbSize, end)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if b, ok := f.meta[abs]; ok && b.next <= end {
		return b, nil
	}
	var h [2]byte
	if err := f.readAt(h[:], abs, end); err != nil {
		return nil, err
	}
	v := binary.LittleEndian.Uint16(h[:])
	size := int(v & 0x7fff)
	stored := v&0x8000 != 0
	if size == 0 || size > metaBlockSize {
		return nil, corrupt("metadata block at %d has size %d", abs, size)
	}
	if needStored && !stored {
		return nil, corrupt("metadata block at %d is compressed", abs)
	}
	raw := make([]byte, size)
	if err := f.readAt(raw, abs+2, end); err != nil {
		return nil, err
	}
	data := raw
	if !stored {
		var err error
		if data, err = f.decompress(raw, metaBlockSize); err != nil {
			return nil, fmt.Errorf("metadata block at %d: %w", abs, err)
		}
		if len(data) == 0 {
			return nil, corrupt("metadata block at %d is empty", abs)
		}
	}
	f.metaBytes += int64(len(data))
	if f.metaBytes > f.lim.MaxMetadataBytes {
		return nil, limited("more than %d bytes of decoded metadata", f.lim.MaxMetadataBytes)
	}
	b := &metaBlock{data: data, next: abs + 2 + int64(size)}
	f.meta[abs] = b
	return b, nil
}

// decoderPanics counts the panics of the third party decoders turned into errors by decompress. The recover
// is a last resort; tests assert that it never fires on the inputs they feed, so that it cannot hide a bug.
var decoderPanics atomic.Int64

// decompress inflates one block, refusing more than max bytes.
func (f *FS) decompress(src []byte, max int) (out []byte, err error) {
	defer func() {
		if r := recover(); r != nil {
			decoderPanics.Add(1)
			// the decoders are third party code reading untrusted data
			out, err = nil, corrupt("decoder failure: %v", r)
		}
	}()
	var r io.Reader
	switch f.sb.Compression {
	case CompXZ:
		return xzDecode(src, max)
	case CompGzip:
		r, err = zlib.NewReader(bytes.NewReader(src))
		if err != nil {
			return nil, fmt.Errorf("%w: zlib: %v", ErrCorrupt, err)
		}
	default:
		return nil, unsupported("%s compression", compName(f.sb.Compression))
	}
	var buf bytes.Buffer
	if _, err = io.Copy(&buf, io.LimitReader(r, int64(max)+1)); err != nil {
		return nil, fmt.Errorf("%w: %s: %v", ErrCorrupt, compName(f.sb.Compression), err)
	}
	if buf.Len() > max {
		return nil, limited("a block inflates to more than %d bytes", max)
	}
	return buf.Bytes(), nil
}

// cursor reads a byte stream made of consecutive metadata blocks.
type cursor struct {
	f      *FS
	end    int64
	blk    []byte
	pos    int
	next   int64
	chain  int
	nChain int
}

func (f *FS) newCursor(abs int64, intra int, end int64) (*cursor, error) {
	c := &cursor{f: f, end: end, nChain: f.lim.MaxMetaChain}
	b, err := f.metaAt(abs, end)
	if err != nil {
		return nil, err
	}
	if intra >= len(b.data) {
		return nil, corrupt("offset %d is outside the metadata block at %d (%d bytes)", intra, abs, len(b.data))
	}
	c.blk, c.pos, c.next, c.chain = b.data, intra, b.next, 1
	return c, nil
}

// read fills p from the stream.
func (c *cursor) read(p []byte) error {
	for len(p) > 0 {
		if c.pos == len(c.blk) {
			if c.chain >= c.nChain {
				return limited("metadata chain longer than %d blocks", c.nChain)
			}
			if c.next >= c.end {
				return corrupt("metadata stream ends inside a structure")
			}
			b, err := c.f.metaAt(c.next, c.end)
			if err != nil {
				return err
			}
			c.blk, c.pos, c.next = b.data, 0, b.next
			c.chain++
		}
		n := copy(p, c.blk[c.pos:])
		c.pos += n
		p = p[n:]
	}
	return nil
}

func (c *cursor) u16() (uint16, error) {
	var b [2]byte
	err := c.read(b[:])
	return binary.LittleEndian.Uint16(b[:]), err
}

func (c *cursor) u32() (uint32, error) {
	var b [4]byte
	err := c.read(b[:])
	return binary.LittleEndian.Uint32(b[:]), err
}

func (c *cursor) u64() (uint64, error) {
	var b [8]byte
	err := c.read(b[:])
	return binary.LittleEndian.Uint64(b[:]), err
}
