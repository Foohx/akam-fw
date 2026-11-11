package squashfs

import (
	"bytes"
	"encoding/binary"
	"io"
	"io/fs"
)

// FileReader streams the content of one regular file. It is not safe for concurrent use; its total output
// is bounded by Limits.MaxCallBytes.
type FileReader struct {
	f    *FS
	n    *inode
	bud  *budget
	pos  uint64 // bytes already loaded into buf
	idx  int    // next data block
	disk uint64 // disk offset of the next data block
	buf  []byte
	bp   int
	err  error
}

// Size is the size of the file.
func (r *FileReader) Size() int64 { return int64(r.n.size) }

// OpenFile opens the regular file at name for streaming.
func (f *FS) OpenFile(name string) (*FileReader, error) {
	n, p, err := f.resolve(name, true)
	if err != nil {
		return nil, err
	}
	if n.typ != typeFile {
		return nil, &fs.PathError{Op: "open", Path: p, Err: ErrNotRegular}
	}
	return f.openInode(n, &budget{left: f.lim.MaxCallBytes}), nil
}

func (f *FS) openInode(n *inode, b *budget) *FileReader {
	return &FileReader{f: f, n: n, bud: b, disk: n.blocksStart}
}

// ReadFile returns the whole content of the regular file at name (at most Limits.MaxFileSize bytes).
func (f *FS) ReadFile(name string) ([]byte, error) {
	r, err := f.OpenFile(name)
	if err != nil {
		return nil, err
	}
	var buf bytes.Buffer
	buf.Grow(int(min(r.n.size, 1<<20))) // never trust the declared size for the allocation
	if _, err := io.Copy(&buf, r); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// Read implements io.Reader.
func (r *FileReader) Read(p []byte) (int, error) {
	for r.bp == len(r.buf) {
		if r.err != nil {
			return 0, r.err
		}
		if r.pos >= r.n.size {
			r.err = io.EOF
			return 0, io.EOF
		}
		if err := r.fill(); err != nil {
			r.err = err
			return 0, err
		}
	}
	n := copy(p, r.buf[r.bp:])
	r.bp += n
	return n, nil
}

func (r *FileReader) fill() error {
	f, n := r.f, r.n
	bs := uint64(f.sb.BlockSize)
	r.bp = 0
	if r.idx < len(n.blockSizes) {
		want := min(bs, n.size-uint64(r.idx)*bs)
		w := n.blockSizes[r.idx]
		csize := uint64(w & 0xffffff)
		stored := w&(1<<24) != 0
		if err := r.bud.take(int64(want)); err != nil {
			return err
		}
		if csize == 0 { // sparse block
			r.buf = make([]byte, want)
		} else {
			if r.disk < sbSize || r.disk+csize > uint64(f.sb.lastUsed) {
				return corrupt("data block at %d (%d bytes) is outside the image", r.disk, csize)
			}
			raw := make([]byte, csize)
			if err := f.readAt(raw, int64(r.disk), f.sb.lastUsed); err != nil {
				return err
			}
			if stored {
				if csize != want {
					return corrupt("stored data block of %d bytes, %d expected", csize, want)
				}
				r.buf = raw
			} else {
				out, err := f.decompress(raw, int(want))
				if err != nil {
					return err
				}
				if uint64(len(out)) != want {
					return corrupt("data block inflates to %d bytes, %d expected", len(out), want)
				}
				r.buf = out
			}
		}
		r.disk += csize
		r.idx++
		r.pos += want
		return nil
	}
	// the tail, stored in a fragment
	tail := n.size - uint64(len(n.blockSizes))*bs
	if n.fragIdx == noFragment || tail == 0 || tail >= bs {
		return corrupt("file of %d bytes has no data left to read", n.size)
	}
	if err := r.bud.take(int64(tail)); err != nil {
		return err
	}
	frag, err := f.fragment(n.fragIdx, r.bud)
	if err != nil {
		return err
	}
	if uint64(n.fragOff)+tail > uint64(len(frag)) {
		return corrupt("fragment tail (%d bytes at %d) is outside the fragment block (%d)", tail, n.fragOff, len(frag))
	}
	r.buf = frag[n.fragOff : uint64(n.fragOff)+tail]
	r.pos += tail
	return nil
}

// fragment returns the decompressed fragment block idx (cached, never modified).
func (f *FS) fragment(idx uint32, b *budget) ([]byte, error) {
	f.mu.Lock()
	if f.fragOK && f.fragIdx == idx {
		d := f.fragData
		f.mu.Unlock()
		return d, nil
	}
	f.mu.Unlock()
	if int(idx/512) >= len(f.fragPtrs) {
		return nil, corrupt("fragment %d out of %d", idx, f.sb.Fragments)
	}
	blk, err := f.metaAt(int64(f.fragPtrs[idx/512]), f.sb.lastUsed)
	if err != nil {
		return nil, err
	}
	o := int(idx%512) * 16
	if o+16 > len(blk.data) {
		return nil, corrupt("fragment table block too short for fragment %d", idx)
	}
	start := binary.LittleEndian.Uint64(blk.data[o:])
	w := binary.LittleEndian.Uint32(blk.data[o+8:])
	csize := uint64(w & 0xffffff)
	if w>>25 != 0 || csize == 0 || csize > uint64(f.sb.BlockSize) {
		return nil, corrupt("fragment %d has size word 0x%x", idx, w)
	}
	if start < sbSize || start+csize > uint64(f.sb.lastUsed) {
		return nil, corrupt("fragment %d (%d bytes at %d) is outside the image", idx, csize, start)
	}
	raw := make([]byte, csize)
	if err := f.readAt(raw, int64(start), f.sb.lastUsed); err != nil {
		return nil, err
	}
	data := raw
	if w&(1<<24) == 0 {
		if err := b.take(int64(f.sb.BlockSize)); err != nil {
			return nil, err
		}
		if data, err = f.decompress(raw, int(f.sb.BlockSize)); err != nil {
			return nil, err
		}
	}
	f.mu.Lock()
	f.fragIdx, f.fragData, f.fragOK = idx, data, true
	f.mu.Unlock()
	return data, nil
}
