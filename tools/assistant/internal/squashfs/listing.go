package squashfs

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"sort"
)

// ListEntry is one line of the canonical listing: everything that identifies an entry independently of the
// layout of the image (block positions, directory sizes and ordering are left out).
type ListEntry struct {
	Path         string
	Type         FileType
	Mode         uint16
	UID, GID     uint32
	MTime        int64
	Nlink        uint32
	Size         int64 // file content size, symlink target length, 0 otherwise
	Target       string
	Major, Minor uint32 // block and character devices
	SHA256       string // hex digest of the content of a regular file
}

// String is the canonical text form: quoted path, type letter, octal mode, uid, gid, mtime, nlink, size,
// "major,minor", quoted link target ("-" when none) and the sha256 ("-" when none).
func (e ListEntry) String() string {
	target, sum := "-", "-"
	if e.Type == typeSymlink {
		target = fmt.Sprintf("%q", e.Target)
	}
	if e.SHA256 != "" {
		sum = e.SHA256
	}
	return fmt.Sprintf("%q %s %04o %d %d %d %d %d %d,%d %s %s", e.Path, e.Type.Letter(), e.Mode, e.UID, e.GID, e.MTime, e.Nlink, e.Size, e.Major, e.Minor, target, sum)
}

// Listing returns the canonical listing of the whole image, sorted by path (byte order), hashing the content
// of every regular file. The bytes decompressed by the call are capped by Limits.MaxCallBytes.
func (f *FS) Listing() ([]ListEntry, error) {
	bud := &budget{left: f.lim.MaxCallBytes}
	var out []ListEntry
	err := f.walkInodes("/", func(n *inode, fi FileInfo) error {
		e := ListEntry{Path: fi.Path, Type: fi.Type, Mode: fi.Mode, UID: fi.UID, GID: fi.GID, MTime: fi.MTime,
			Nlink: fi.Nlink, Major: fi.Major, Minor: fi.Minor}
		switch fi.Type {
		case typeSymlink:
			e.Target, e.Size = fi.Target, fi.Size
		case typeFile:
			e.Size = fi.Size
			full, err := f.readInode(n.ref, true)
			if err != nil {
				return err
			}
			h := sha256.New()
			if _, err := io.Copy(h, f.openInode(full, bud)); err != nil {
				return fmt.Errorf("%s: %w", fi.Path, err)
			}
			e.SHA256 = hex.EncodeToString(h.Sum(nil))
		}
		out = append(out, e)
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

// WriteListing writes Listing, one ListEntry.String per line.
func (f *FS) WriteListing(w io.Writer) error {
	l, err := f.Listing()
	if err != nil {
		return err
	}
	for _, e := range l {
		if _, err := fmt.Fprintln(w, e.String()); err != nil {
			return err
		}
	}
	return nil
}

// Verify walks the whole image, decodes every data block, and checks the cross-references: the inode count
// of the superblock, inode numbers, and the export table when present (each inode number must map to the
// reference found by the walk).
func (f *FS) Verify() error {
	bud := &budget{left: f.lim.MaxCallBytes}
	refs := map[uint32]uint64{}
	err := f.walkInodes("/", func(n *inode, fi FileInfo) error {
		if old, ok := refs[n.ino]; ok {
			if old != n.ref {
				return corrupt("inode number %d has two references (0x%x, 0x%x)", n.ino, old, n.ref)
			}
		} else {
			refs[n.ino] = n.ref
		}
		if fi.Type == typeFile {
			full, err := f.readInode(n.ref, true)
			if err != nil {
				return err
			}
			if _, err := io.Copy(io.Discard, f.openInode(full, bud)); err != nil {
				return fmt.Errorf("%s: %w", fi.Path, err)
			}
		}
		return nil
	})
	if err != nil {
		return err
	}
	if uint32(len(refs)) != f.sb.Inodes {
		return corrupt("%d inodes reachable, the superblock says %d", len(refs), f.sb.Inodes)
	}
	if f.sb.HasExportTable {
		tab, err := f.ExportTable()
		if err != nil {
			return err
		}
		for ino, ref := range refs {
			if tab[ino-1] != ref {
				return corrupt("export table maps inode %d to 0x%x, found at 0x%x", ino, tab[ino-1], ref)
			}
		}
	}
	return nil
}
