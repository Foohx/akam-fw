package squashfs

import (
	"fmt"
	"io"
	"path"
)

// Tree reads the whole image into a Node tree that Write can write again (the image is rewritten, for
// example after a patch of some files). All file contents are loaded in memory, within Limits.MaxCallBytes.
// Ownership, modes, times and device numbers are kept. Nothing the writer cannot represent is silently
// dropped: an image with a hard link (a file inode reached from two directory entries, or with a link count
// above 1) fails with ErrHardLink, and one with extended attributes with ErrUnsupported.
func (f *FS) Tree() (*Node, error) {
	bud := &budget{left: f.lim.MaxCallBytes}
	nodes := map[string]*Node{}
	seen := map[uint32]string{}
	var root *Node
	err := f.walkInodes("/", func(n *inode, fi FileInfo) error {
		if fi.HasXattr {
			return unsupported("%s: extended attributes", fi.Path)
		}
		if fi.Type != TypeDir {
			if prev, dup := seen[fi.Inode]; dup || fi.Nlink > 1 {
				if !dup {
					prev = fi.Path
				}
				return fmt.Errorf("%w: %s and %s share an inode (link count %d)", ErrHardLink, prev, fi.Path, fi.Nlink)
			}
			seen[fi.Inode] = fi.Path
		}
		nd := &Node{Name: path.Base(fi.Path), Type: fi.Type, Mode: fi.Mode, UID: fi.UID, GID: fi.GID, MTime: uint32(fi.MTime)}
		switch fi.Type {
		case TypeFile:
			full, err := f.readInode(n.ref, true)
			if err != nil {
				return err
			}
			data, err := io.ReadAll(f.openInode(full, bud))
			if err != nil {
				return fmt.Errorf("%s: %w", fi.Path, err)
			}
			nd.Data = data
		case TypeSymlink:
			nd.Target = fi.Target
		case TypeBlock, TypeChar:
			nd.Major, nd.Minor = fi.Major, fi.Minor
		}
		if fi.Path == "/" {
			nd.Name = ""
			root = nd
		} else {
			parent := nodes[path.Dir(fi.Path)]
			if parent == nil {
				return corrupt("%s: parent not visited", fi.Path)
			}
			parent.Children = append(parent.Children, nd)
		}
		if fi.Type == TypeDir {
			nodes[fi.Path] = nd
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return root, nil
}
