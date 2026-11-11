// Package sdcard prepares, verifies and wipes the microSD cards of the install flow, in Go, without
// mkfs, mount or sfdisk: an MBR with one active FAT32-LBA partition, a FAT32 volume written directly on
// the device, and a read-only FAT32 reader to check what was written. The privileged part runs in a
// tiny re-executed subcommand (`akam sd-helper`, see helper.go); everything else is unprivileged.
package sdcard

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"path"
	"sort"
	"strings"
	"time"
	"unicode/utf16"
)

// Device is what the writer needs: random access and a sync. *os.File satisfies it.
type Device interface {
	io.ReaderAt
	io.WriterAt
	Sync() error
}

// File is one file of the card tree.
type File struct {
	Path string // slash separated, relative, no ".."
	Data []byte
}

const (
	sectorSize   = 512
	reservedSecs = 32
	numFATs      = 2
	partStartLBA = 2048 // 1 MiB alignment
	// FATLabel is the volume label.
	FATLabel = "AKAMSD"
)

// ErrTooSmall means the device cannot hold a FAT32 volume with the requested files.
var ErrTooSmall = errors.New("the device is too small for the files")

// secPerCluster follows the Microsoft table (cards of 128 MiB and more).
func secPerCluster(totalSecs int64) uint8 {
	mb := totalSecs * sectorSize / (1 << 20)
	switch {
	case mb <= 260:
		return 1
	case mb <= 8192:
		return 8
	case mb <= 16384:
		return 16
	case mb <= 32768:
		return 32
	}
	return 64
}

func dosDateTime(t time.Time) (uint16, uint16) {
	t = t.UTC()
	d := uint16(t.Year()-1980)<<9 | uint16(t.Month())<<5 | uint16(t.Day())
	tm := uint16(t.Hour())<<11 | uint16(t.Minute())<<5 | uint16(t.Second()/2)
	return d, tm
}

// ---------------------------------------------------------------- names

func validLong(name string) bool {
	if name == "" || len(name) > 255 || strings.ContainsAny(name, "\"*/:<>?\\|") || name == "." || name == ".." {
		return false
	}
	for _, r := range name {
		if r < 0x20 {
			return false
		}
	}
	return !strings.HasSuffix(name, ".") && !strings.HasSuffix(name, " ")
}

func isShort83(name string) (string, bool) {
	base, ext := name, ""
	if i := strings.LastIndex(name, "."); i >= 0 {
		base, ext = name[:i], name[i+1:]
	}
	ok := func(s string, max int, allowEmpty bool) bool {
		if s == "" {
			return allowEmpty
		}
		if len(s) > max {
			return false
		}
		for _, r := range s {
			if !(r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '_' || r == '-' || r == '~') {
				return false
			}
		}
		return true
	}
	if !ok(base, 8, false) || !ok(ext, 3, true) || strings.Count(name, ".") > 1 {
		return "", false
	}
	return fmt.Sprintf("%-8s%-3s", base, ext), true
}

func shortChar(r rune) byte {
	switch {
	case r >= 'a' && r <= 'z':
		return byte(r - 'a' + 'A')
	case r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '_', r == '-', r == '~', r == '$', r == '!', r == '#', r == '%', r == '&', r == '(', r == ')', r == '@', r == '^', r == '{', r == '}', r == '\'', r == '`':
		return byte(r)
	}
	return '_'
}

// shortFor builds a unique 8.3 name with a numeric tail (BASENA~1.EXT).
func shortFor(name string, used map[string]bool) string {
	base, ext := name, ""
	if i := strings.LastIndex(name, "."); i > 0 {
		base, ext = name[:i], name[i+1:]
	}
	clean := func(s string, max int) string {
		var b []byte
		for _, r := range s {
			if r == ' ' || r == '.' {
				continue
			}
			b = append(b, shortChar(r))
			if len(b) == max {
				break
			}
		}
		return string(b)
	}
	b, e := clean(base, 8), clean(ext, 3)
	if b == "" {
		b = "_"
	}
	for n := 1; ; n++ {
		tail := fmt.Sprintf("~%d", n)
		bb := b
		if len(bb) > 8-len(tail) {
			bb = bb[:8-len(tail)]
		}
		s := fmt.Sprintf("%-8s%-3s", bb+tail, e)
		if !used[s] {
			return s
		}
	}
}

func lfnChecksum(short string) byte {
	var sum byte
	for i := 0; i < 11; i++ {
		sum = (sum>>1 | sum<<7) + short[i]
	}
	return sum
}

// ---------------------------------------------------------------- tree

type node struct {
	name     string
	dir      bool
	data     []byte
	children []*node
	short    string
	cluster  uint32
	nclust   uint32
	slots    int
}

func buildTree(files []File) (*node, error) {
	root := &node{dir: true}
	for _, f := range files {
		p := path.Clean(f.Path)
		if p == "." || p == "" || strings.HasPrefix(p, "/") || strings.HasPrefix(p, "../") || p == ".." {
			return nil, fmt.Errorf("bad path %q", f.Path)
		}
		cur := root
		parts := strings.Split(p, "/")
		for i, part := range parts {
			if !validLong(part) {
				return nil, fmt.Errorf("name %q cannot be written on a FAT volume", part)
			}
			last := i == len(parts)-1
			var next *node
			for _, c := range cur.children {
				if c.name == part {
					next = c
				}
			}
			if next == nil {
				next = &node{name: part, dir: !last}
				cur.children = append(cur.children, next)
			}
			if last {
				if next.dir {
					return nil, fmt.Errorf("%q is both a file and a folder", f.Path)
				}
				next.data = f.Data
			} else if !next.dir {
				return nil, fmt.Errorf("%q is both a file and a folder", part)
			}
			cur = next
		}
	}
	var assign func(n *node) error
	assign = func(n *node) error {
		sort.Slice(n.children, func(i, j int) bool { return n.children[i].name < n.children[j].name })
		used := map[string]bool{}
		for _, c := range n.children {
			if s, ok := isShort83(c.name); ok {
				c.short = s
				used[s] = true
			}
		}
		slots := 0
		if n.name == "" {
			slots = 1 // volume label
		} else {
			slots = 2 // . and ..
		}
		for _, c := range n.children {
			if c.short == "" {
				c.short = shortFor(c.name, used)
				used[c.short] = true
				slots += (len(utf16.Encode([]rune(c.name)))+12)/13 + 1
			} else {
				slots++
			}
			if c.dir {
				if err := assign(c); err != nil {
					return err
				}
			}
		}
		n.slots = slots
		return nil
	}
	return root, assign(root)
}

// ---------------------------------------------------------------- writer

// WriteOptions of WriteFAT32.
type WriteOptions struct {
	Label string
	Now   time.Time
	// VolumeID is the 32-bit serial (random when zero).
	VolumeID uint32
}

// FormatMBR writes an MBR with one active FAT32-LBA partition from LBA 2048 to the end of the device
// (what `sfdisk` got from "label: dos\n, , c, *\n"). It returns the partition offset and size in bytes.
func FormatMBR(dev Device, size int64) (off, psize int64, err error) {
	total := size / sectorSize
	if total < partStartLBA+65536 {
		return 0, 0, ErrTooSmall
	}
	psecs := total - partStartLBA
	if psecs > 0xFFFFFFFF {
		psecs = 0xFFFFFFFF
	}
	mbr := make([]byte, sectorSize)
	binary.LittleEndian.PutUint32(mbr[440:], rand.Uint32())
	e := mbr[446:]
	e[0] = 0x80 // active
	e[1], e[2], e[3] = 0xFE, 0xFF, 0xFF
	e[4] = 0x0C // FAT32 LBA
	e[5], e[6], e[7] = 0xFE, 0xFF, 0xFF
	binary.LittleEndian.PutUint32(e[8:], partStartLBA)
	binary.LittleEndian.PutUint32(e[12:], uint32(psecs))
	mbr[510], mbr[511] = 0x55, 0xAA
	if _, err := dev.WriteAt(mbr, 0); err != nil {
		return 0, 0, err
	}
	return partStartLBA * sectorSize, psecs * sectorSize, nil
}

// WriteFAT32 formats a FAT32 volume in the partition [off, off+psize) and writes the files into it,
// directly on the device (no mount). The device is synced at the end.
func WriteFAT32(dev Device, off, psize int64, files []File, o WriteOptions) error {
	if o.Label == "" {
		o.Label = FATLabel
	}
	if o.Now.IsZero() {
		o.Now = time.Now()
	}
	if o.VolumeID == 0 {
		o.VolumeID = rand.Uint32()
	}
	root, err := buildTree(files)
	if err != nil {
		return err
	}
	T := psize / sectorSize
	spc := int64(secPerCluster(T))
	tmp2 := (256*spc + numFATs) / 2
	fatSz := (T - reservedSecs + tmp2 - 1) / tmp2
	nclus := (T - reservedSecs - numFATs*fatSz) / spc
	if nclus < 65525 {
		return ErrTooSmall
	}
	csize := spc * sectorSize
	dataStart := reservedSecs + numFATs*fatSz

	// allocate clusters: directories first (breadth), then files
	next := uint32(2)
	var dirs, regs []*node
	var walk func(n *node)
	walk = func(n *node) {
		if n.dir {
			dirs = append(dirs, n)
		}
		for _, c := range n.children {
			if c.dir {
				walk(c)
			} else {
				regs = append(regs, c)
			}
		}
	}
	walk(root)
	alloc := func(n *node, bytes int64) {
		c := uint32((bytes + csize - 1) / csize)
		if c == 0 && n.dir {
			c = 1
		}
		n.nclust = c
		if c > 0 {
			n.cluster = next
			next += c
		}
	}
	for _, d := range dirs {
		alloc(d, int64(d.slots+1)*32)
	}
	for _, f := range regs {
		alloc(f, int64(len(f.data)))
	}
	if int64(next-2) > nclus {
		return ErrTooSmall
	}
	used := next - 2

	fat := make([]byte, fatSz*sectorSize)
	setFAT := func(i, v uint32) { binary.LittleEndian.PutUint32(fat[i*4:], v) }
	setFAT(0, 0x0FFFFFF8)
	setFAT(1, 0x0FFFFFFF)
	chain := func(n *node) {
		for i := uint32(0); i < n.nclust; i++ {
			v := n.cluster + i + 1
			if i == n.nclust-1 {
				v = 0x0FFFFFFF
			}
			setFAT(n.cluster+i, v)
		}
	}
	for _, d := range dirs {
		chain(d)
	}
	for _, f := range regs {
		chain(f)
	}

	cOff := func(c uint32) int64 { return off + (dataStart+int64(c-2)*spc)*sectorSize }
	wr := func(b []byte, at int64) error { _, err := dev.WriteAt(b, at); return err }

	// reserved area: boot sector, FSInfo, backups
	boot := make([]byte, sectorSize)
	copy(boot, []byte{0xEB, 0x58, 0x90})
	copy(boot[3:], "MSWIN4.1")
	binary.LittleEndian.PutUint16(boot[11:], sectorSize)
	boot[13] = byte(spc)
	binary.LittleEndian.PutUint16(boot[14:], reservedSecs)
	boot[16] = numFATs
	boot[21] = 0xF8
	binary.LittleEndian.PutUint16(boot[24:], 63)
	binary.LittleEndian.PutUint16(boot[26:], 255)
	binary.LittleEndian.PutUint32(boot[28:], uint32(off/sectorSize))
	binary.LittleEndian.PutUint32(boot[32:], uint32(T))
	binary.LittleEndian.PutUint32(boot[36:], uint32(fatSz))
	binary.LittleEndian.PutUint32(boot[44:], 2)
	binary.LittleEndian.PutUint16(boot[48:], 1)
	binary.LittleEndian.PutUint16(boot[50:], 6)
	boot[64] = 0x80
	boot[66] = 0x29
	binary.LittleEndian.PutUint32(boot[67:], o.VolumeID)
	copy(boot[71:82], fmt.Sprintf("%-11s", o.Label))
	copy(boot[82:], "FAT32   ")
	boot[510], boot[511] = 0x55, 0xAA
	fsi := make([]byte, sectorSize)
	binary.LittleEndian.PutUint32(fsi[0:], 0x41615252)
	binary.LittleEndian.PutUint32(fsi[484:], 0x61417272)
	binary.LittleEndian.PutUint32(fsi[488:], uint32(nclus)-used)
	binary.LittleEndian.PutUint32(fsi[492:], next)
	binary.LittleEndian.PutUint32(fsi[508:], 0xAA550000)
	zero := make([]byte, reservedSecs*sectorSize)
	if err := wr(zero, off); err != nil {
		return err
	}
	for _, p := range []struct {
		b  []byte
		at int64
	}{{boot, 0}, {fsi, 1}, {boot, 6}, {fsi, 7}} {
		if err := wr(p.b, off+p.at*sectorSize); err != nil {
			return err
		}
	}
	for i := int64(0); i < numFATs; i++ {
		if err := wr(fat, off+(reservedSecs+i*fatSz)*sectorSize); err != nil {
			return err
		}
	}

	d, tm := dosDateTime(o.Now)
	for _, dn := range dirs {
		buf := make([]byte, int64(dn.nclust)*csize)
		pos := 0
		put := func(e []byte) { copy(buf[pos:], e); pos += 32 }
		ent := func(short string, attr byte, cl uint32, size uint32) []byte {
			e := make([]byte, 32)
			copy(e, short)
			e[11] = attr
			binary.LittleEndian.PutUint16(e[14:], tm)
			binary.LittleEndian.PutUint16(e[16:], d)
			binary.LittleEndian.PutUint16(e[18:], d)
			binary.LittleEndian.PutUint16(e[20:], uint16(cl>>16))
			binary.LittleEndian.PutUint16(e[22:], tm)
			binary.LittleEndian.PutUint16(e[24:], d)
			binary.LittleEndian.PutUint16(e[26:], uint16(cl))
			binary.LittleEndian.PutUint32(e[28:], size)
			return e
		}
		if dn.name == "" {
			put(ent(fmt.Sprintf("%-11s", o.Label), 0x08, 0, 0))
		} else {
			parent := uint32(0)
			for _, p := range dirs {
				for _, c := range p.children {
					if c == dn && p.name != "" {
						parent = p.cluster
					}
				}
			}
			put(ent(".          ", 0x10, dn.cluster, 0))
			put(ent("..         ", 0x10, parent, 0))
		}
		for _, c := range dn.children {
			if _, ok := isShort83(c.name); !ok {
				u := utf16.Encode([]rune(c.name))
				n := (len(u) + 12) / 13
				sum := lfnChecksum(c.short)
				for i := n; i >= 1; i-- {
					e := make([]byte, 32)
					e[0] = byte(i)
					if i == n {
						e[0] |= 0x40
					}
					e[11], e[13] = 0x0F, sum
					offs := []int{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}
					for k, o2 := range offs {
						idx := (i-1)*13 + k
						var ch uint16 = 0xFFFF
						if idx < len(u) {
							ch = u[idx]
						} else if idx == len(u) {
							ch = 0
						}
						binary.LittleEndian.PutUint16(e[o2:], ch)
					}
					put(e)
				}
			}
			attr := byte(0x20)
			size := uint32(len(c.data))
			if c.dir {
				attr, size = 0x10, 0
			}
			put(ent(c.short, attr, c.cluster, size))
		}
		if err := wr(buf, cOff(dn.cluster)); err != nil {
			return err
		}
	}
	for _, f := range regs {
		if len(f.data) == 0 {
			continue
		}
		if err := wr(f.data, cOff(f.cluster)); err != nil {
			return err
		}
	}
	return dev.Sync()
}

// ---------------------------------------------------------------- reader

// Tree reads a card tree (a FAT32 volume or a staged directory).
type Tree interface {
	ReadFile(rel string) ([]byte, error)
	// List returns the names (not paths) in a folder; "" is the root.
	List(rel string) ([]string, error)
	Exists(rel string) bool
}

// FATReader is a read-only FAT32 reader over a partition.
type FATReader struct {
	r        io.ReaderAt
	off      int64
	spc      int64
	fat      []byte
	dataOff  int64
	rootClus uint32
	csize    int64
	label    string
}

// ErrNotFAT32 is returned for a volume that is not a FAT32 filesystem.
var ErrNotFAT32 = errors.New("not a FAT32 volume")

// OpenFAT32 opens the FAT32 volume at off.
func OpenFAT32(r io.ReaderAt, off int64) (*FATReader, error) {
	b := make([]byte, sectorSize)
	if _, err := r.ReadAt(b, off); err != nil {
		return nil, err
	}
	if b[510] != 0x55 || b[511] != 0xAA || string(b[82:87]) != "FAT32" || binary.LittleEndian.Uint16(b[11:]) != sectorSize {
		return nil, ErrNotFAT32
	}
	f := &FATReader{r: r, off: off, spc: int64(b[13]), rootClus: binary.LittleEndian.Uint32(b[44:])}
	if f.spc == 0 {
		return nil, ErrNotFAT32
	}
	rsv := int64(binary.LittleEndian.Uint16(b[14:]))
	nf := int64(b[16])
	fsz := int64(binary.LittleEndian.Uint32(b[36:]))
	f.csize = f.spc * sectorSize
	f.dataOff = off + (rsv+nf*fsz)*sectorSize
	f.label = strings.TrimRight(string(b[71:82]), " ")
	f.fat = make([]byte, fsz*sectorSize)
	if _, err := r.ReadAt(f.fat, off+rsv*sectorSize); err != nil {
		return nil, err
	}
	return f, nil
}

// Label is the volume label.
func (f *FATReader) Label() string { return f.label }

func (f *FATReader) readChain(c uint32, limit int64) ([]byte, error) {
	var out []byte
	for n := 0; c >= 2 && c < 0x0FFFFFF8; n++ {
		if n > len(f.fat)/4 || int(c)*4+4 > len(f.fat) {
			return nil, errors.New("bad cluster chain")
		}
		buf := make([]byte, f.csize)
		if _, err := f.r.ReadAt(buf, f.dataOff+int64(c-2)*f.csize); err != nil {
			return nil, err
		}
		out = append(out, buf...)
		if limit > 0 && int64(len(out)) >= limit {
			break
		}
		c = binary.LittleEndian.Uint32(f.fat[c*4:]) & 0x0FFFFFFF
	}
	return out, nil
}

type dirEnt struct {
	name    string
	dir     bool
	cluster uint32
	size    uint32
}

func (f *FATReader) readDir(c uint32) ([]dirEnt, error) {
	raw, err := f.readChain(c, 0)
	if err != nil {
		return nil, err
	}
	var out []dirEnt
	var lfn []uint16
	for i := 0; i+32 <= len(raw); i += 32 {
		e := raw[i : i+32]
		if e[0] == 0 {
			break
		}
		if e[0] == 0xE5 {
			lfn = nil
			continue
		}
		attr := e[11]
		if attr == 0x0F {
			if e[0]&0x40 != 0 {
				lfn = nil
			}
			chunk := make([]uint16, 0, 13)
			for _, o := range []int{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30} {
				chunk = append(chunk, binary.LittleEndian.Uint16(e[o:]))
			}
			lfn = append(chunk, lfn...)
			continue
		}
		if attr&0x08 != 0 {
			lfn = nil
			continue
		}
		var name string
		if len(lfn) > 0 {
			end := len(lfn)
			for j, ch := range lfn {
				if ch == 0 || ch == 0xFFFF {
					end = j
					break
				}
			}
			name = string(utf16.Decode(lfn[:end]))
		} else {
			base := strings.TrimRight(string(e[0:8]), " ")
			ext := strings.TrimRight(string(e[8:11]), " ")
			name = base
			if ext != "" {
				name += "." + ext
			}
		}
		lfn = nil
		if name == "." || name == ".." {
			continue
		}
		cl := uint32(binary.LittleEndian.Uint16(e[20:]))<<16 | uint32(binary.LittleEndian.Uint16(e[26:]))
		out = append(out, dirEnt{name: name, dir: attr&0x10 != 0, cluster: cl, size: binary.LittleEndian.Uint32(e[28:])})
	}
	return out, nil
}

func (f *FATReader) lookup(rel string) (dirEnt, error) {
	cur := dirEnt{dir: true, cluster: f.rootClus}
	rel = path.Clean("/" + rel)
	if rel == "/" {
		return cur, nil
	}
	for _, part := range strings.Split(strings.TrimPrefix(rel, "/"), "/") {
		if !cur.dir {
			return dirEnt{}, fmt.Errorf("%s: not a folder", part)
		}
		ents, err := f.readDir(cur.cluster)
		if err != nil {
			return dirEnt{}, err
		}
		found := false
		for _, e := range ents {
			if e.name == part {
				cur, found = e, true
				break
			}
		}
		if !found {
			return dirEnt{}, fmt.Errorf("%s: no such file", rel)
		}
	}
	return cur, nil
}

// ReadFile reads a file.
func (f *FATReader) ReadFile(rel string) ([]byte, error) {
	e, err := f.lookup(rel)
	if err != nil {
		return nil, err
	}
	if e.dir {
		return nil, fmt.Errorf("%s is a folder", rel)
	}
	if e.size == 0 {
		return []byte{}, nil
	}
	b, err := f.readChain(e.cluster, int64(e.size))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) < int64(e.size) {
		return nil, fmt.Errorf("%s: truncated", rel)
	}
	return b[:e.size], nil
}

// List returns the names of a folder.
func (f *FATReader) List(rel string) ([]string, error) {
	e, err := f.lookup(rel)
	if err != nil {
		return nil, err
	}
	if !e.dir {
		return nil, fmt.Errorf("%s is not a folder", rel)
	}
	ents, err := f.readDir(e.cluster)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, x := range ents {
		out = append(out, x.name)
	}
	sort.Strings(out)
	return out, nil
}

// Exists reports whether a path exists.
func (f *FATReader) Exists(rel string) bool { _, err := f.lookup(rel); return err == nil }

// OpenCard reads the MBR of a device, checks the one FAT32 partition and opens it.
func OpenCard(r io.ReaderAt) (*FATReader, error) {
	b := make([]byte, sectorSize)
	if _, err := r.ReadAt(b, 0); err != nil {
		return nil, err
	}
	if b[510] != 0x55 || b[511] != 0xAA {
		return nil, errors.New("no partition table (MBR signature missing)")
	}
	e := b[446:]
	if e[0] != 0x80 || (e[4] != 0x0C && e[4] != 0x0B) {
		return nil, errors.New("the first partition is not an active FAT32 partition")
	}
	for i := 1; i < 4; i++ {
		if b[446+16*i+4] != 0 {
			return nil, errors.New("the card has more than one partition")
		}
	}
	return OpenFAT32(r, int64(binary.LittleEndian.Uint32(e[8:]))*sectorSize)
}
