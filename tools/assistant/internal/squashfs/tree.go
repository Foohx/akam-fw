package squashfs

import (
	"errors"
	"fmt"
	"io/fs"
	"path"
	"strings"
)

type dirEntry struct {
	name string
	typ  FileType
	ino  uint32
	ref  uint64
}

// readDirEntries returns the entries of a directory inode, in stored order, validated: names are non-empty,
// at most 256 bytes, free of "/" and NUL, not "." or "..", and strictly increasing (so no duplicates).
func (f *FS) readDirEntries(d *inode) ([]dirEntry, error) {
	if d.size < 3 {
		return nil, corrupt("directory of %d bytes", d.size)
	}
	if d.size-3 > uint64(f.lim.MaxDirBytes) {
		return nil, limited("directory of %d bytes (cap %d)", d.size, f.lim.MaxDirBytes)
	}
	remaining := int64(d.size - 3)
	if remaining == 0 {
		return nil, nil
	}
	if uint64(d.dirBlock) >= uint64(f.sb.dirEnd)-f.sb.dirStart {
		return nil, corrupt("directory block %d is outside the directory table", d.dirBlock)
	}
	c, err := f.newCursor(int64(f.sb.dirStart)+int64(d.dirBlock), int(d.dirOff), f.sb.dirEnd)
	if err != nil {
		return nil, err
	}
	var out []dirEntry
	itab := f.sb.dirStart - f.sb.inodeStart
	for remaining > 0 {
		if remaining < 12 {
			return nil, corrupt("directory header overruns the directory")
		}
		count, err1 := c.u32()
		start, err2 := c.u32()
		base, err3 := c.u32()
		if err := errors.Join(err1, err2, err3); err != nil {
			return nil, err
		}
		remaining -= 12
		if count > 255 {
			return nil, corrupt("directory header of %d entries", uint64(count)+1)
		}
		if uint64(start) >= itab {
			return nil, corrupt("directory entry inode block %d is outside the inode table", start)
		}
		for i := uint32(0); i <= count; i++ {
			if remaining < 8 {
				return nil, corrupt("directory entry overruns the directory")
			}
			off, e1 := c.u16()
			delta, e2 := c.u16()
			typ, e3 := c.u16()
			ns, e4 := c.u16()
			if err := errors.Join(e1, e2, e3, e4); err != nil {
				return nil, err
			}
			remaining -= 8
			nl := int64(ns) + 1
			if nl > maxNameLen || nl > remaining {
				return nil, corrupt("directory entry name of %d bytes", nl)
			}
			if off >= metaBlockSize {
				return nil, corrupt("directory entry inode offset %d", off)
			}
			buf := make([]byte, nl)
			if err := c.read(buf); err != nil {
				return nil, err
			}
			remaining -= nl
			name := string(buf)
			if name == "." || name == ".." || strings.ContainsAny(name, "/\x00") {
				return nil, corrupt("invalid directory entry name %q", name)
			}
			if typ > 7 && typ <= 14 {
				typ -= 7
			}
			if typ < 1 || typ > 7 {
				return nil, corrupt("directory entry type %d", typ)
			}
			ino := int64(base) + int64(int16(delta))
			if ino < 1 || ino > int64(f.sb.Inodes) {
				return nil, corrupt("directory entry inode number %d out of %d", ino, f.sb.Inodes)
			}
			if n := len(out); n > 0 && out[n-1].name >= name {
				return nil, corrupt("directory entries not strictly sorted at %q", name)
			}
			if len(out) >= f.lim.MaxDirEntries {
				return nil, limited("more than %d entries in a directory", f.lim.MaxDirEntries)
			}
			out = append(out, dirEntry{name: name, typ: FileType(typ), ino: uint32(ino), ref: uint64(start)<<16 | uint64(off)})
		}
	}
	return out, nil
}

// child loads the inode an entry refers to and checks it agrees with the entry.
func (f *FS) child(e dirEntry, blocks bool) (*inode, error) {
	n, err := f.readInode(e.ref, blocks)
	if err != nil {
		return nil, fmt.Errorf("%q: %w", e.name, err)
	}
	if n.typ != e.typ || n.ino != e.ino {
		return nil, corrupt("%q: directory entry (type %v, inode %d) disagrees with inode (type %v, inode %d)", e.name, e.typ, e.ino, n.typ, n.ino)
	}
	return n, nil
}

func (f *FS) rootInode() (*inode, error) { return f.readInode(f.sb.root, false) }

// cleanPath normalises name into an absolute slash path and its components.
func (f *FS) cleanPath(name string) (string, []string, error) {
	if strings.ContainsRune(name, 0) {
		return "", nil, fmt.Errorf("squashfs: invalid path %q", name)
	}
	p := path.Clean("/" + name)
	if p == "/" {
		return p, nil, nil
	}
	parts := strings.Split(p[1:], "/")
	if len(parts) > f.lim.MaxDepth {
		return "", nil, limited("path of %d components (cap %d)", len(parts), f.lim.MaxDepth)
	}
	return p, parts, nil
}

func (f *FS) resolve(name string, blocks bool) (*inode, string, error) {
	p, parts, err := f.cleanPath(name)
	if err != nil {
		return nil, "", err
	}
	cur, err := f.rootInode()
	if err != nil {
		return nil, "", err
	}
	for i, part := range parts {
		if cur.typ != typeDir {
			return nil, "", &fs.PathError{Op: "lstat", Path: p, Err: ErrNotDir}
		}
		ents, err := f.readDirEntries(cur)
		if err != nil {
			return nil, "", err
		}
		var found *dirEntry
		for k := range ents {
			if ents[k].name == part {
				found = &ents[k]
				break
			}
		}
		if found == nil {
			return nil, "", &fs.PathError{Op: "lstat", Path: p, Err: fs.ErrNotExist}
		}
		if cur, err = f.child(*found, blocks && i == len(parts)-1); err != nil {
			return nil, "", err
		}
	}
	return cur, p, nil
}

// Lstat describes the entry at name (an absolute or relative slash path; "/" or "" is the root). Symbolic
// links are never followed.
func (f *FS) Lstat(name string) (FileInfo, error) {
	n, p, err := f.resolve(name, false)
	if err != nil {
		return FileInfo{}, err
	}
	return f.info(n, p, path.Base(p)), nil
}

// ReadDir lists a directory in stored (sorted) order.
func (f *FS) ReadDir(name string) ([]FileInfo, error) {
	n, p, err := f.resolve(name, false)
	if err != nil {
		return nil, err
	}
	return f.readDirInfos(n, p)
}

func (f *FS) readDirInfos(n *inode, p string) ([]FileInfo, error) {
	if n.typ != typeDir {
		return nil, &fs.PathError{Op: "readdir", Path: p, Err: ErrNotDir}
	}
	ents, err := f.readDirEntries(n)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", p, err)
	}
	out := make([]FileInfo, 0, len(ents))
	for _, e := range ents {
		c, err := f.child(e, false)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", p, err)
		}
		out = append(out, f.info(c, path.Join(p, e.name), e.name))
	}
	return out, nil
}

// Readlink returns the target of the symbolic link at name.
func (f *FS) Readlink(name string) (string, error) {
	n, p, err := f.resolve(name, false)
	if err != nil {
		return "", err
	}
	if n.typ != typeSymlink {
		return "", &fs.PathError{Op: "readlink", Path: p, Err: ErrNotSymlink}
	}
	return n.target, nil
}

// SkipDir, returned by a Walk callback on a directory, skips its content.
var SkipDir = errors.New("squashfs: skip this directory")

// Walk calls fn for the entry at root and, for a directory, for everything below it, depth first, each
// directory in stored (sorted) order. A directory reached twice (a cycle or a hard linked directory) is
// corrupt. The walk stops at the first error of fn (SkipDir excepted).
func (f *FS) Walk(root string, fn func(FileInfo) error) error {
	return f.walkInodes(root, func(_ *inode, fi FileInfo) error { return fn(fi) })
}

func (f *FS) walkInodes(root string, fn func(*inode, FileInfo) error) error {
	n, p, err := f.resolve(root, false)
	if err != nil {
		return err
	}
	w := &walker{f: f, fn: fn, seen: map[uint32]struct{}{}, max: 2 * int(f.sb.Inodes)}
	err = w.visit(n, p, path.Base(p), 0)
	if errors.Is(err, SkipDir) {
		err = nil
	}
	return err
}

type walker struct {
	f     *FS
	fn    func(*inode, FileInfo) error
	seen  map[uint32]struct{} // directory inode numbers
	count int
	max   int
}

func (w *walker) visit(n *inode, p, name string, depth int) error {
	if w.count++; w.count > w.max {
		return limited("more than %d entries", w.max)
	}
	if err := w.fn(n, w.f.info(n, p, name)); err != nil {
		if errors.Is(err, SkipDir) && n.typ == typeDir {
			return nil
		}
		return err
	}
	if n.typ != typeDir {
		return nil
	}
	if depth >= w.f.lim.MaxDepth {
		return limited("directories nested deeper than %d", w.f.lim.MaxDepth)
	}
	if _, dup := w.seen[n.ino]; dup {
		return corrupt("directory %s (inode %d) is reachable twice: loop", p, n.ino)
	}
	w.seen[n.ino] = struct{}{}
	ents, err := w.f.readDirEntries(n)
	if err != nil {
		return fmt.Errorf("%s: %w", p, err)
	}
	for _, e := range ents {
		c, err := w.f.child(e, false)
		if err != nil {
			return fmt.Errorf("%s: %w", p, err)
		}
		if err := w.visit(c, path.Join(p, e.name), e.name, depth+1); err != nil {
			if errors.Is(err, SkipDir) {
				return nil // SkipDir from a non-directory skips the rest of this directory
			}
			return err
		}
	}
	return nil
}
