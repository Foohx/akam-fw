package kit

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"

	"github.com/ulikunitz/xz"
	"github.com/ulikunitz/xz/lzma"
)

// XZLimits are the caps checked on the .xz structure before any byte is decompressed.
type XZLimits struct {
	// MaxDict is the largest LZMA2 dictionary the stream may declare (default 64 MiB, what `xz -9e` writes).
	MaxDict int64
	// MaxUncompressed is the largest uncompressed size the index may declare (default 1 GiB, maxStream).
	MaxUncompressed int64
}

// DefaultXZLimits are the limits used for a kit.
var DefaultXZLimits = XZLimits{MaxDict: 64 << 20, MaxUncompressed: maxStream}

// xzDecompress is the default Options.Decompress: a kit is decoded in process, never by an external program.
func xzDecompress(file string) (io.ReadCloser, error) { return XZReader(file, DefaultXZLimits) }

// xzReadCloser closes the file under the decoder.
type xzReadCloser struct {
	*xz.Reader
	f *os.File
}

func (x *xzReadCloser) Close() error { return x.f.Close() }

// XZReader opens file as a single-stream, single-block, LZMA2-only .xz and returns its decoded bytes.
// The structure (stream header, block header, footer and index: a few dozen bytes at each end of the
// file) is checked against lim BEFORE the decoder exists, because the library's DictCap is a floor, not a
// cap: a stream that declares a 4 GiB dictionary would otherwise make it allocate that much. At the end
// of the stream the reader reports an error (never a clean end) when the data is truncated or the
// integrity check fails.
func XZReader(file string, lim XZLimits) (io.ReadCloser, error) {
	if lim.MaxDict <= 0 {
		lim.MaxDict = DefaultXZLimits.MaxDict
	}
	if lim.MaxUncompressed <= 0 {
		lim.MaxUncompressed = DefaultXZLimits.MaxUncompressed
	}
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	dict, err := xzPrescan(f, st.Size(), lim)
	if err != nil {
		f.Close()
		return nil, err
	}
	if _, err := f.Seek(0, io.SeekStart); err != nil {
		f.Close()
		return nil, err
	}
	r, err := xz.ReaderConfig{SingleStream: true, DictCap: int(dict)}.NewReader(f)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("xz: %w", err)
	}
	return &xzReadCloser{Reader: r, f: f}, nil
}

var xzMagic = []byte{0xFD, '7', 'z', 'X', 'Z', 0x00}

// xzCheckSize is the size of the integrity check per stream flag (only the types the library supports).
func xzCheckSize(t byte) (int64, bool) {
	switch t {
	case 0x00:
		return 0, true
	case 0x01:
		return 4, true
	case 0x04:
		return 8, true
	case 0x0A:
		return 32, true
	}
	return 0, false
}

// xzVarint decodes an xz multibyte integer (at most 9 bytes) and returns the bytes consumed.
func xzVarint(b []byte) (uint64, int, error) {
	var v uint64
	for i := 0; i < len(b) && i < 9; i++ {
		v |= uint64(b[i]&0x7f) << (7 * uint(i))
		if b[i]&0x80 == 0 {
			if b[i] == 0 && i > 0 {
				return 0, 0, errors.New("non-minimal integer")
			}
			return v, i + 1, nil
		}
	}
	return 0, 0, errors.New("bad integer")
}

// xzPrescan checks the file structure and returns the declared LZMA2 dictionary size. It reads only the
// first block header and the footer plus index.
func xzPrescan(f io.ReaderAt, size int64, lim XZLimits) (int64, error) {
	bad := func(format string, a ...any) (int64, error) {
		return 0, fmt.Errorf("xz: "+format, a...)
	}
	if size < 12+12+12 {
		return bad("file too short to be a kit (.tar.xz)")
	}
	var hdr [12]byte
	if _, err := f.ReadAt(hdr[:], 0); err != nil {
		return bad("cannot read the stream header: %v", err)
	}
	if string(hdr[:6]) != string(xzMagic) {
		return bad("not an .xz file")
	}
	if crc32.ChecksumIEEE(hdr[6:8]) != binary.LittleEndian.Uint32(hdr[8:12]) {
		return bad("stream header checksum mismatch")
	}
	if hdr[6] != 0 || hdr[7]&0xf0 != 0 {
		return bad("unsupported stream flags")
	}
	checkSize, ok := xzCheckSize(hdr[7] & 0x0f)
	if !ok {
		return bad("unsupported integrity check type %d", hdr[7]&0x0f)
	}

	var ftr [12]byte
	if _, err := f.ReadAt(ftr[:], size-12); err != nil {
		return bad("cannot read the stream footer: %v", err)
	}
	if string(ftr[10:]) != "YZ" {
		return bad("no stream footer at the end of the file (trailing data or truncated)")
	}
	if crc32.ChecksumIEEE(ftr[4:10]) != binary.LittleEndian.Uint32(ftr[0:4]) {
		return bad("stream footer checksum mismatch")
	}
	if ftr[8] != hdr[6] || ftr[9] != hdr[7] {
		return bad("stream header and footer flags differ")
	}
	idxSize := (int64(binary.LittleEndian.Uint32(ftr[4:8])) + 1) * 4
	// One record is at most 1 + 1 + 9 + 9 bytes + padding + CRC32 = 28: a larger index lists several blocks.
	if idxSize > 32 {
		return bad("the stream has more than one block (the kit must be one LZMA2 block, as xz -T1 writes)")
	}
	if size < 12+idxSize+12 {
		return bad("truncated file")
	}
	idx := make([]byte, idxSize)
	if _, err := f.ReadAt(idx, size-12-idxSize); err != nil {
		return bad("cannot read the index: %v", err)
	}
	if crc32.ChecksumIEEE(idx[:idxSize-4]) != binary.LittleEndian.Uint32(idx[idxSize-4:]) {
		return bad("index checksum mismatch")
	}
	if idx[0] != 0 {
		return bad("bad index indicator")
	}
	body := idx[1 : idxSize-4]
	nrec, n, err := xzVarint(body)
	if err != nil {
		return bad("bad index: %v", err)
	}
	if nrec != 1 {
		return bad("the stream has %d blocks, exactly one is accepted", nrec)
	}
	body = body[n:]
	unpadded, n, err := xzVarint(body)
	if err != nil {
		return bad("bad index: %v", err)
	}
	body = body[n:]
	uncompressed, n, err := xzVarint(body)
	if err != nil {
		return bad("bad index: %v", err)
	}
	for _, p := range body[n:] {
		if p != 0 {
			return bad("bad index padding")
		}
	}
	if unpadded < uint64(5+checkSize) || unpadded > 1<<62 || uncompressed > 1<<62 {
		return bad("bad index record")
	}
	if uncompressed > uint64(lim.MaxUncompressed) {
		return bad("the stream declares %d bytes uncompressed, more than the %d allowed", uncompressed, lim.MaxUncompressed)
	}
	// The whole file must be this one stream: header + block + index + footer. This refuses a
	// concatenation of streams (the footer at the end would belong to the last one) and trailing data.
	if int64(unpadded+3)/4*4+12+idxSize+12 != size {
		return bad("the file is not exactly one stream (concatenated streams or trailing data)")
	}

	// First (and only) block header.
	var bh [1024]byte
	if _, err := f.ReadAt(bh[:1], 12); err != nil {
		return bad("cannot read the block header: %v", err)
	}
	if bh[0] == 0 {
		return bad("the stream has no block")
	}
	hs := (int64(bh[0]) + 1) * 4
	if 12+hs > size {
		return bad("truncated block header")
	}
	if _, err := f.ReadAt(bh[:hs], 12); err != nil {
		return bad("cannot read the block header: %v", err)
	}
	h := bh[:hs]
	if crc32.ChecksumIEEE(h[:hs-4]) != binary.LittleEndian.Uint32(h[hs-4:]) {
		return bad("block header checksum mismatch")
	}
	flags := h[1]
	if flags&0x3c != 0 {
		return bad("unsupported block flags")
	}
	if flags&0x03 != 0 {
		return bad("the block uses more than one filter (only LZMA2 is accepted)")
	}
	p := h[2 : hs-4]
	if flags&0x40 != 0 { // compressed size
		_, n, err := xzVarint(p)
		if err != nil {
			return bad("bad block header: %v", err)
		}
		p = p[n:]
	}
	if flags&0x80 != 0 { // uncompressed size
		v, n, err := xzVarint(p)
		if err != nil {
			return bad("bad block header: %v", err)
		}
		if v != uncompressed {
			return bad("block header and index disagree on the uncompressed size")
		}
		p = p[n:]
	}
	id, n, err := xzVarint(p)
	if err != nil {
		return bad("bad block header: %v", err)
	}
	if id != 0x21 {
		return bad("the block filter is 0x%x, only LZMA2 (0x21) is accepted", id)
	}
	p = p[n:]
	plen, n, err := xzVarint(p)
	if err != nil || plen != 1 || len(p) < n+1 {
		return bad("bad LZMA2 filter properties")
	}
	for _, z := range p[n+1:] {
		if z != 0 {
			return bad("bad block header padding")
		}
	}
	dict, err := lzma.DecodeDictCap(p[n])
	if err != nil {
		return bad("bad LZMA2 dictionary size: %v", err)
	}
	if dict > lim.MaxDict {
		return bad("the stream declares a %d byte dictionary, more than the %d allowed", dict, lim.MaxDict)
	}
	return dict, nil
}
