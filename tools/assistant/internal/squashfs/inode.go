package squashfs

import (
	"fmt"
)

// FileType is the kind of an entry.
type FileType uint8

const (
	typeDir     = FileType(1)
	typeFile    = FileType(2)
	typeSymlink = FileType(3)
	typeBlock   = FileType(4)
	typeChar    = FileType(5)
	typeFifo    = FileType(6)
	typeSocket  = FileType(7)
)

// Exported names of the entry kinds.
const (
	TypeDir     = typeDir
	TypeFile    = typeFile
	TypeSymlink = typeSymlink
	TypeBlock   = typeBlock
	TypeChar    = typeChar
	TypeFifo    = typeFifo
	TypeSocket  = typeSocket
)

// Letter is the one-letter code of the type (d f l b c p s, as in a long listing).
func (t FileType) Letter() string {
	if t >= 1 && t <= 7 {
		return "dflbcps"[t-1 : t]
	}
	return "?"
}

func (t FileType) String() string {
	switch t {
	case typeDir:
		return "dir"
	case typeFile:
		return "file"
	case typeSymlink:
		return "symlink"
	case typeBlock:
		return "block"
	case typeChar:
		return "char"
	case typeFifo:
		return "fifo"
	case typeSocket:
		return "socket"
	}
	return fmt.Sprintf("type%d", t)
}

// FileInfo describes one entry. Mode holds the permission bits and setuid, setgid and sticky (07777).
type FileInfo struct {
	Path     string
	Name     string
	Type     FileType
	Mode     uint16
	UID, GID uint32
	MTime    int64 // seconds since the epoch
	Inode    uint32
	Nlink    uint32
	// Size is the content size of a regular file and the target length of a symlink; for a directory it is
	// the stored size of its listing (layout dependent), 0 for the other types.
	Size int64
	// Target is the target of a symlink.
	Target string
	// Rdev is the raw device number of a block or character device; Major and Minor decode it.
	Rdev         uint32
	Major, Minor uint32
	// HasXattr tells that the inode points to extended attributes (not interpreted by this package).
	HasXattr bool
	// Ref is the inode reference (inode table block offset << 16 | offset in the block).
	Ref uint64
}

type inode struct {
	ref                 uint64
	typ                 FileType
	ext                 bool
	mode                uint16
	uid, gid            uint32
	mtime               uint32
	ino                 uint32
	nlink               uint32
	size                uint64
	dirBlock            uint32
	dirOff              uint16
	parent              uint32
	blocksStart         uint64
	fragIdx             uint32
	fragOff             uint32
	blockSizes          []uint32
	target              string
	rdev                uint32
	hasXattr            bool
	sumCompressedBlocks uint64
}

func (f *FS) idAt(i uint16) (uint32, error) {
	if int(i) >= len(f.ids) {
		return 0, corrupt("id index %d out of %d", i, len(f.ids))
	}
	return f.ids[i], nil
}

// readInode decodes the inode at ref. With blocks the data block list of a regular file is loaded too.
func (f *FS) readInode(ref uint64, blocks bool) (*inode, error) {
	blk, off := ref>>16, int(ref&0xffff)
	if blk >= f.sb.dirStart-f.sb.inodeStart {
		return nil, corrupt("inode reference 0x%x is outside the inode table", ref)
	}
	c, err := f.newCursor(int64(f.sb.inodeStart+blk), off, f.sb.inodeEnd)
	if err != nil {
		return nil, fmt.Errorf("inode 0x%x: %w", ref, err)
	}
	n, err := f.parseInode(c, ref, blocks)
	if err != nil {
		return nil, fmt.Errorf("inode 0x%x: %w", ref, err)
	}
	return n, nil
}

func (f *FS) parseInode(c *cursor, ref uint64, blocks bool) (*inode, error) {
	var h [16]byte
	if err := c.read(h[:]); err != nil {
		return nil, err
	}
	le := func(o int) uint32 { return uint32(h[o]) | uint32(h[o+1])<<8 | uint32(h[o+2])<<16 | uint32(h[o+3])<<24 }
	t := uint16(h[0]) | uint16(h[1])<<8
	n := &inode{ref: ref}
	n.mode = (uint16(h[2]) | uint16(h[3])<<8) & 07777
	uidx := uint16(h[4]) | uint16(h[5])<<8
	gidx := uint16(h[6]) | uint16(h[7])<<8
	n.mtime = le(8)
	n.ino = le(12)
	if t < 1 || t > 14 {
		return nil, corrupt("inode type %d", t)
	}
	n.ext = t > 7
	if n.ext {
		t -= 7
	}
	n.typ = FileType(t)
	var err error
	if n.uid, err = f.idAt(uidx); err != nil {
		return nil, err
	}
	if n.gid, err = f.idAt(gidx); err != nil {
		return nil, err
	}
	if n.ino == 0 || n.ino > f.sb.Inodes {
		return nil, corrupt("inode number %d out of %d", n.ino, f.sb.Inodes)
	}
	n.hasXattr = false
	var x struct{ err error }
	rd32 := func() uint32 {
		v, e := c.u32()
		if e != nil && x.err == nil {
			x.err = e
		}
		return v
	}
	rd16 := func() uint16 {
		v, e := c.u16()
		if e != nil && x.err == nil {
			x.err = e
		}
		return v
	}
	rd64 := func() uint64 {
		v, e := c.u64()
		if e != nil && x.err == nil {
			x.err = e
		}
		return v
	}
	switch n.typ {
	case typeDir:
		if !n.ext {
			n.dirBlock = rd32()
			n.nlink = rd32()
			n.size = uint64(rd16())
			n.dirOff = rd16()
			n.parent = rd32()
		} else {
			n.nlink = rd32()
			n.size = uint64(rd32())
			n.dirBlock = rd32()
			n.parent = rd32()
			rd16() // index count: the index is not used
			n.dirOff = rd16()
			n.hasXattr = rd32() != noXattr
		}
		if x.err == nil && n.dirOff >= metaBlockSize {
			return nil, corrupt("directory offset %d", n.dirOff)
		}
	case typeFile:
		if !n.ext {
			n.blocksStart = uint64(rd32())
			n.fragIdx = rd32()
			n.fragOff = rd32()
			n.size = uint64(rd32())
			n.nlink = 1
		} else {
			n.blocksStart = rd64()
			n.size = rd64()
			rd64() // sparse bytes
			n.nlink = rd32()
			n.fragIdx = rd32()
			n.fragOff = rd32()
			n.hasXattr = rd32() != noXattr
		}
		if x.err != nil {
			return nil, x.err
		}
		if n.size > uint64(f.lim.MaxFileSize) {
			return nil, limited("file size %d exceeds %d", n.size, f.lim.MaxFileSize)
		}
		bs := uint64(f.sb.BlockSize)
		if n.fragIdx != noFragment && n.fragIdx >= f.sb.Fragments {
			return nil, corrupt("fragment index %d out of %d", n.fragIdx, f.sb.Fragments)
		}
		if n.fragIdx != noFragment && n.fragOff > f.sb.BlockSize {
			return nil, corrupt("fragment offset %d", n.fragOff)
		}
		if n.blocksStart > uint64(f.sb.lastUsed) {
			return nil, corrupt("file data at %d is outside the image", n.blocksStart)
		}
		if blocks {
			nb := n.size / bs
			if n.fragIdx == noFragment && n.size%bs != 0 {
				nb++
			}
			if err := f.readBlockList(c, n, nb); err != nil {
				return nil, err
			}
		}
	case typeSymlink:
		n.nlink = rd32()
		l := rd32()
		if x.err != nil {
			return nil, x.err
		}
		if l > maxSymlink {
			return nil, limited("symlink target of %d bytes", l)
		}
		buf := make([]byte, l)
		if err := c.read(buf); err != nil {
			return nil, err
		}
		n.target = string(buf)
		n.size = uint64(l)
		if n.ext {
			n.hasXattr = rd32() != noXattr
		}
	case typeBlock, typeChar:
		n.nlink = rd32()
		n.rdev = rd32()
		if n.ext {
			n.hasXattr = rd32() != noXattr
		}
	case typeFifo, typeSocket:
		n.nlink = rd32()
		if n.ext {
			n.hasXattr = rd32() != noXattr
		}
	}
	if x.err != nil {
		return nil, x.err
	}
	return n, nil
}

// readBlockList loads nb data block size words. It grows the list as words are really read, so a bogus
// size never drives an allocation.
func (f *FS) readBlockList(c *cursor, n *inode, nb uint64) error {
	if nb > uint64(f.lim.MaxFileSize)/minBlockSize+1 {
		return limited("%d data blocks", nb)
	}
	var buf [4 * 512]byte
	sum := uint64(0)
	left := nb
	bs := f.sb.BlockSize
	for left > 0 {
		k := min(left, 512)
		if err := c.read(buf[:k*4]); err != nil {
			return err
		}
		for i := uint64(0); i < k; i++ {
			w := uint32(buf[i*4]) | uint32(buf[i*4+1])<<8 | uint32(buf[i*4+2])<<16 | uint32(buf[i*4+3])<<24
			if w>>25 != 0 || w&0xffffff > bs {
				return corrupt("data block word 0x%x (block size %d)", w, bs)
			}
			sum += uint64(w & 0xffffff)
			n.blockSizes = append(n.blockSizes, w)
		}
		left -= k
	}
	if n.blocksStart+sum > uint64(f.sb.lastUsed) {
		return corrupt("file data (%d bytes at %d) ends after the image data (%d)", sum, n.blocksStart, f.sb.lastUsed)
	}
	n.sumCompressedBlocks = sum
	return nil
}

func (f *FS) info(n *inode, path, name string) FileInfo {
	fi := FileInfo{
		Path: path, Name: name, Type: n.typ, Mode: n.mode, UID: n.uid, GID: n.gid, MTime: int64(n.mtime),
		Inode: n.ino, Nlink: n.nlink, HasXattr: n.hasXattr, Ref: n.ref,
	}
	switch n.typ {
	case typeFile, typeSymlink, typeDir:
		fi.Size = int64(n.size)
	}
	if n.typ == typeSymlink {
		fi.Target = n.target
	}
	if n.typ == typeBlock || n.typ == typeChar {
		fi.Rdev = n.rdev
		fi.Major = (n.rdev & 0xfff00) >> 8
		fi.Minor = (n.rdev & 0xff) | ((n.rdev >> 12) & 0xfff00)
	}
	return fi
}
