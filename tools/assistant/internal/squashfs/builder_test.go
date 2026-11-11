package squashfs

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"sort"

	"github.com/ulikunitz/xz"
)

// A minimal test-only squashfs 4.0 image builder: it exists to reach layouts mksquashfs does not produce
// (extended inodes everywhere, tampered fields, loops). Metadata blocks are always stored, so inode
// references can be computed before the content is final. It is not the writer of T11.14.

type bkid struct {
	name string
	n    *bn
}

type bn struct {
	typ        FileType
	ext        bool
	mode       uint16
	uid, gid   uint32
	mtime      uint32
	data       []byte
	target     string
	rdev       uint32
	kids       []bkid
	noXattrTag bool

	ino      uint32
	ref      uint64
	pos      int
	dirPos   int
	dirBytes []byte
	blkStart uint64
	blocks   []uint32
	fragIdx  uint32
	fragOff  uint32
}

func bdir(kids ...bkid) *bn { return &bn{typ: TypeDir, mode: 0o755, mtime: 1700000000, kids: kids} }
func bfile(data []byte) *bn {
	return &bn{typ: TypeFile, mode: 0o644, mtime: 1700000000, data: data}
}
func blink(target string) *bn {
	return &bn{typ: TypeSymlink, mode: 0o777, mtime: 1700000000, target: target}
}
func bk(name string, n *bn) bkid { return bkid{name, n} }

type bopt struct {
	comp      int
	bs        int
	noFrag    bool
	storeData bool
	noSparse  bool
	export    bool
	options   bool   // write a compressor options block
	xzFilters uint32 // value written in the xz options block
	padTo     int
}

type built struct {
	img                                  []byte
	inodeStart, dirStart, fragStart      int
	idStart, lookupStart, dataStart, end int
	nodes                                []*bn
}

func le32(b []byte, v uint32) []byte { return binary.LittleEndian.AppendUint32(b, v) }
func le16(b []byte, v uint16) []byte { return binary.LittleEndian.AppendUint16(b, v) }
func le64(b []byte, v uint64) []byte { return binary.LittleEndian.AppendUint64(b, v) }

func frameMeta(stream []byte) []byte {
	var out []byte
	for len(stream) > 0 {
		n := min(len(stream), metaBlockSize)
		out = le16(out, uint16(n)|0x8000)
		out = append(out, stream[:n]...)
		stream = stream[n:]
	}
	return out
}

func metaRef(pos int) uint64 {
	return uint64(pos/metaBlockSize*(metaBlockSize+2))<<16 | uint64(pos%metaBlockSize)
}

func bcompress(comp int, bs int, p []byte) []byte {
	var b bytes.Buffer
	switch comp {
	case CompXZ:
		w, err := xz.WriterConfig{CheckSum: xz.CRC32, DictCap: max(bs, 4096)}.NewWriter(&b)
		if err != nil {
			panic(err)
		}
		w.Write(p)
		w.Close()
	default:
		w := zlib.NewWriter(&b)
		w.Write(p)
		w.Close()
	}
	return b.Bytes()
}

func inodeSize(n *bn, nblocks int) int {
	switch n.typ {
	case TypeDir:
		if n.ext {
			return 16 + 24
		}
		return 16 + 16
	case TypeFile:
		if n.ext {
			return 16 + 40 + 4*nblocks
		}
		return 16 + 16 + 4*nblocks
	case TypeSymlink:
		s := 16 + 8 + len(n.target)
		if n.ext {
			s += 4
		}
		return s
	case TypeBlock, TypeChar:
		if n.ext {
			return 16 + 12
		}
		return 16 + 8
	default:
		if n.ext {
			return 16 + 8
		}
		return 16 + 4
	}
}

func buildImage(root *bn, o bopt) built {
	if o.comp == 0 {
		o.comp = CompXZ
	}
	if o.bs == 0 {
		o.bs = 131072
	}
	var nodes []*bn
	seen := map[*bn]bool{}
	var collect func(n *bn)
	collect = func(n *bn) {
		if seen[n] {
			return
		}
		seen[n] = true
		sort.Slice(n.kids, func(i, j int) bool { return n.kids[i].name < n.kids[j].name })
		for _, k := range n.kids {
			collect(k.n)
		}
		nodes = append(nodes, n)
		n.ino = uint32(len(nodes))
	}
	collect(root)

	img := make([]byte, sbSize)
	if o.options {
		var opt []byte
		if o.comp == CompXZ {
			opt = le32(le32(nil, 131072), o.xzFilters)
		} else {
			opt = le16(le16(le32(nil, 9), 15), 0)
		}
		img = append(img, frameMeta(opt)...)
	}
	dataStart := len(img)
	var frags []struct {
		start uint64
		word  uint32
	}
	var fragBuf []byte
	flush := func() {
		if len(fragBuf) == 0 {
			return
		}
		blk, word := fragBuf, uint32(len(fragBuf))|1<<24
		if c := bcompress(o.comp, o.bs, fragBuf); !o.storeData && len(c) < len(fragBuf) {
			blk, word = c, uint32(len(c))
		}
		frags = append(frags, struct {
			start uint64
			word  uint32
		}{uint64(len(img)), word})
		img = append(img, blk...)
		fragBuf = nil
	}
	for _, n := range nodes {
		if n.typ != TypeFile {
			continue
		}
		n.fragIdx = noFragment
		n.blkStart = uint64(len(img))
		d := n.data
		for len(d) >= o.bs || (len(d) > 0 && o.noFrag) {
			chunk := d[:min(len(d), o.bs)]
			d = d[len(chunk):]
			switch c := bcompress(o.comp, o.bs, chunk); {
			case !o.noSparse && bytes.Count(chunk, []byte{0}) == len(chunk):
				n.blocks = append(n.blocks, 0)
			case o.storeData || len(c) >= len(chunk):
				n.blocks = append(n.blocks, uint32(len(chunk))|1<<24)
				img = append(img, chunk...)
			default:
				n.blocks = append(n.blocks, uint32(len(c)))
				img = append(img, c...)
			}
		}
		if len(d) > 0 {
			if len(fragBuf)+len(d) > o.bs {
				flush()
			}
			n.fragIdx, n.fragOff = uint32(len(frags)), uint32(len(fragBuf))
			fragBuf = append(fragBuf, d...)
		}
	}
	flush()

	// inode positions
	pos := 0
	for _, n := range nodes {
		n.pos = pos
		n.ref = metaRef(pos)
		pos += inodeSize(n, len(n.blocks))
	}
	// directories
	dpos := 0
	for _, n := range nodes {
		if n.typ != TypeDir {
			continue
		}
		var b []byte
		kids := n.kids
		for i := 0; i < len(kids); {
			blk := kids[i].n.ref >> 16
			j := i
			for j < len(kids) && j-i < 256 && kids[j].n.ref>>16 == blk && int(kids[j].n.ino)-int(kids[i].n.ino) < 30000 && int(kids[j].n.ino)-int(kids[i].n.ino) > -30000 {
				j++
			}
			b = le32(le32(le32(b, uint32(j-i-1)), uint32(blk)), kids[i].n.ino)
			for _, k := range kids[i:j] {
				b = le16(le16(le16(le16(b, uint16(k.n.ref&0xffff)), uint16(int16(int(k.n.ino)-int(kids[i].n.ino)))), uint16(k.n.typ)), uint16(len(k.name)-1))
				b = append(b, k.name...)
			}
			i = j
		}
		n.dirBytes, n.dirPos = b, dpos
		dpos += len(b)
	}
	var dirStream []byte
	for _, n := range nodes {
		if n.typ == TypeDir {
			dirStream = append(dirStream, n.dirBytes...)
		}
	}
	// ids
	var ids []uint32
	idIdx := func(v uint32) uint16 {
		for i, x := range ids {
			if x == v {
				return uint16(i)
			}
		}
		ids = append(ids, v)
		return uint16(len(ids) - 1)
	}
	// inodes
	var ino []byte
	for _, n := range nodes {
		t := uint16(n.typ)
		if n.ext {
			t += 7
		}
		ino = le16(ino, t)
		ino = le16(ino, n.mode)
		ino = le16(ino, idIdx(n.uid))
		ino = le16(ino, idIdx(n.gid))
		ino = le32(ino, n.mtime)
		ino = le32(ino, n.ino)
		switch n.typ {
		case TypeDir:
			nl := uint32(2)
			for _, k := range n.kids {
				if k.n.typ == TypeDir {
					nl++
				}
			}
			blk := uint32(n.dirPos / metaBlockSize * (metaBlockSize + 2))
			off := uint16(n.dirPos % metaBlockSize)
			size := len(n.dirBytes) + 3
			if n.ext {
				ino = le32(ino, nl)
				ino = le32(ino, uint32(size))
				ino = le32(ino, blk)
				ino = le32(ino, uint32(len(nodes)+1))
				ino = le16(ino, 0)
				ino = le16(ino, off)
				ino = le32(ino, noXattr)
			} else {
				ino = le32(ino, blk)
				ino = le32(ino, nl)
				ino = le16(ino, uint16(size))
				ino = le16(ino, off)
				ino = le32(ino, uint32(len(nodes)+1))
			}
		case TypeFile:
			if n.ext {
				ino = le64(ino, n.blkStart)
				ino = le64(ino, uint64(len(n.data)))
				ino = le64(ino, 0)
				ino = le32(ino, 1)
				ino = le32(ino, n.fragIdx)
				ino = le32(ino, n.fragOff)
				ino = le32(ino, noXattr)
			} else {
				ino = le32(ino, uint32(n.blkStart))
				ino = le32(ino, n.fragIdx)
				ino = le32(ino, n.fragOff)
				ino = le32(ino, uint32(len(n.data)))
			}
			for _, w := range n.blocks {
				ino = le32(ino, w)
			}
		case TypeSymlink:
			ino = le32(le32(ino, 1), uint32(len(n.target)))
			ino = append(ino, n.target...)
			if n.ext {
				ino = le32(ino, noXattr)
			}
		case TypeBlock, TypeChar:
			ino = le32(le32(ino, 1), n.rdev)
			if n.ext {
				ino = le32(ino, noXattr)
			}
		default:
			ino = le32(ino, 1)
			if n.ext {
				ino = le32(ino, noXattr)
			}
		}
	}
	b := built{nodes: nodes, dataStart: dataStart}
	b.inodeStart = len(img)
	img = append(img, frameMeta(ino)...)
	b.dirStart = len(img)
	img = append(img, frameMeta(dirStream)...)

	// tables: metadata blocks then the index of their positions
	table := func(entries []byte, per int) int {
		start := len(img)
		var ptrs []byte
		for i := 0; i < len(entries); i += per {
			ptrs = le64(ptrs, uint64(len(img)))
			img = append(img, frameMeta(entries[i:min(len(entries), i+per)])...)
		}
		idx := len(img)
		img = append(img, ptrs...)
		_ = start
		return idx
	}
	b.fragStart = int(noTable >> 1)
	if len(frags) > 0 {
		var fe []byte
		for _, f := range frags {
			fe = le32(le32(le64(fe, f.start), f.word), 0)
		}
		b.fragStart = table(fe, 8192)
	}
	b.lookupStart = int(noTable >> 1)
	if o.export {
		var le []byte
		for _, n := range nodes {
			le = le64(le, n.ref)
		}
		b.lookupStart = table(le, 8192)
	}
	var ie []byte
	for _, v := range ids {
		ie = le32(ie, v)
	}
	b.idStart = table(ie, 8192)
	b.end = len(img)

	flags := uint16(FlagNoXattrs | FlagDuplicates)
	if o.options {
		flags |= FlagCompressorOptions
	}
	if o.export {
		flags |= FlagExportable
	}
	if o.noFrag {
		flags |= FlagNoFragments
	}
	sb := img[:sbSize]
	binary.LittleEndian.PutUint32(sb[0:], magic)
	binary.LittleEndian.PutUint32(sb[4:], uint32(len(nodes)))
	binary.LittleEndian.PutUint32(sb[8:], 1700000000)
	binary.LittleEndian.PutUint32(sb[12:], uint32(o.bs))
	binary.LittleEndian.PutUint32(sb[16:], uint32(len(frags)))
	binary.LittleEndian.PutUint16(sb[20:], uint16(o.comp))
	lg := 0
	for 1<<lg < o.bs {
		lg++
	}
	binary.LittleEndian.PutUint16(sb[22:], uint16(lg))
	binary.LittleEndian.PutUint16(sb[24:], flags)
	binary.LittleEndian.PutUint16(sb[26:], uint16(len(ids)))
	binary.LittleEndian.PutUint16(sb[28:], 4)
	binary.LittleEndian.PutUint16(sb[30:], 0)
	binary.LittleEndian.PutUint64(sb[32:], root.ref)
	binary.LittleEndian.PutUint64(sb[40:], uint64(len(img)))
	binary.LittleEndian.PutUint64(sb[48:], uint64(b.idStart))
	binary.LittleEndian.PutUint64(sb[56:], noTable)
	binary.LittleEndian.PutUint64(sb[64:], uint64(b.inodeStart))
	binary.LittleEndian.PutUint64(sb[72:], uint64(b.dirStart))
	fs, ls := uint64(b.fragStart), uint64(b.lookupStart)
	if len(frags) == 0 {
		fs = noTable
	}
	if !o.export {
		ls = noTable
	}
	binary.LittleEndian.PutUint64(sb[80:], fs)
	binary.LittleEndian.PutUint64(sb[88:], ls)
	if o.padTo > 0 {
		for len(img)%o.padTo != 0 {
			img = append(img, 0)
		}
	}
	b.img = img
	return b
}

// sampleTree is a tree with every inode type, basic or extended.
func sampleTree(ext bool) *bn {
	mk := func(n *bn) *bn { n.ext = ext; return n }
	hard := mk(bfile(textBytes(3000)))
	dev := mk(&bn{typ: TypeChar, mode: 0o666, mtime: 5, rdev: 1<<8 | 3})
	root := mk(bdir(
		bk("empty", mk(bfile(nil))),
		bk("small", mk(bfile([]byte("hello\n")))),
		bk("big", mk(&bn{typ: TypeFile, mode: 0o4755, uid: 1000, gid: 100, mtime: 99, data: append(textBytes(131072*2), pseudoRandom(7, 5000)...)})),
		bk("zeros", mk(bfile(make([]byte, 300000)))),
		bk("hard1", hard), bk("hard2", hard),
		bk("link", mk(blink("small"))),
		bk("dev", dev),
		bk("blk", mk(&bn{typ: TypeBlock, mode: 0o660, uid: 0, gid: 6, rdev: 8 << 8})),
		bk("fifo", mk(&bn{typ: TypeFifo, mode: 0o600, mtime: 7})),
		bk("sock", mk(&bn{typ: TypeSocket, mode: 0o700, mtime: 8})),
		bk("sub", mk(bdir(bk("deep", mk(bdir(bk("f", mk(bfile([]byte("deep file"))))))), bk("e", mk(bdir())))))),
	)
	root.ext = ext
	return root
}
