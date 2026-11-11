package squashfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"hash/crc32"
	"hash/crc64"
	"io"

	"github.com/ulikunitz/xz/lzma"
)

// The xz decoder of the library takes the dictionary size from each block header and allocates it whatever
// the DictCap it was given (DictCap is a floor): a few bytes could ask for gigabytes, and a stream may hold
// many blocks. So the container is parsed here, strictly, and only the LZMA2 payload of the single block is
// given to lzma.Reader2 with a dictionary clamped to what the output can use (no match distance can exceed
// the bytes already produced, so a dictionary as large as the output cap is equivalent to the declared one).

var xzMagic = [6]byte{0xfd, '7', 'z', 'X', 'Z', 0}

func xzCheckSize(t byte) (int, bool) {
	switch t {
	case 0x00:
		return 0, true
	case 0x01:
		return 4, true
	case 0x04:
		return 8, true
	case 0x0a:
		return 32, true
	}
	return 0, false
}

// xzVarint decodes an xz multibyte integer (at most 9 bytes), minimal form only.
func xzVarint(b []byte) (uint64, int, bool) {
	var v uint64
	for i := 0; i < len(b) && i < 9; i++ {
		v |= uint64(b[i]&0x7f) << (7 * uint(i))
		if b[i]&0x80 == 0 {
			if b[i] == 0 && i > 0 {
				return 0, 0, false
			}
			return v, i + 1, true
		}
	}
	return 0, 0, false
}

func allZero(b []byte) bool {
	for _, c := range b {
		if c != 0 {
			return false
		}
	}
	return true
}

// xzDecode decodes src, a complete .xz file holding exactly one LZMA2 block (what mksquashfs writes), into
// at most limit bytes.
func xzDecode(src []byte, limit int) ([]byte, error) {
	le := binary.LittleEndian
	bad := func(format string, a ...any) ([]byte, error) { return nil, corrupt("xz: "+format, a...) }
	if len(src) < 12+8+12 || !bytes.Equal(src[:6], xzMagic[:]) {
		return bad("not an xz stream")
	}
	if src[6] != 0 || le.Uint32(src[8:]) != crc32.ChecksumIEEE(src[6:8]) {
		return bad("bad stream header")
	}
	checkLen, ok := xzCheckSize(src[7])
	if !ok {
		return nil, unsupported("xz check type %d", src[7])
	}
	// the block header
	hb := int(src[12])
	if hb == 0 {
		return bad("no block")
	}
	hs := (hb + 1) * 4
	if len(src) < 12+hs+12 {
		return bad("truncated block header")
	}
	h := src[12 : 12+hs]
	if le.Uint32(h[hs-4:]) != crc32.ChecksumIEEE(h[:hs-4]) {
		return bad("block header checksum mismatch")
	}
	flags := h[1]
	if flags&0x3c != 0 {
		return bad("reserved block flags")
	}
	if flags&0x03 != 0 {
		return nil, unsupported("xz block with %d filters (LZMA2 only)", int(flags&3)+1)
	}
	p := h[2 : hs-4]
	var wantComp, wantUncomp uint64
	var hasComp, hasUncomp bool
	if flags&0x40 != 0 {
		v, n, ok := xzVarint(p)
		if !ok {
			return bad("bad block header")
		}
		wantComp, hasComp, p = v, true, p[n:]
	}
	if flags&0x80 != 0 {
		v, n, ok := xzVarint(p)
		if !ok {
			return bad("bad block header")
		}
		wantUncomp, hasUncomp, p = v, true, p[n:]
	}
	id, n, ok := xzVarint(p)
	if !ok {
		return bad("bad block header")
	}
	if id != 0x21 {
		return nil, unsupported("xz filter 0x%x (LZMA2 only)", id)
	}
	p = p[n:]
	plen, n, ok := xzVarint(p)
	if !ok || plen != 1 || len(p) < n+1 || !allZero(p[n+1:]) {
		return bad("bad LZMA2 filter properties")
	}
	dict, err := lzma.DecodeDictCap(p[n])
	if err != nil {
		return bad("bad LZMA2 dictionary size: %v", err)
	}
	// the payload
	dictCap := int(min(dict, int64(max(limit, lzma.MinDictCap))))
	pos := 12 + hs
	br := bytes.NewReader(src[pos:])
	r2, err := lzma.Reader2Config{DictCap: dictCap}.NewReader2(br)
	if err != nil {
		return bad("%v", err)
	}
	var out bytes.Buffer
	if _, err := io.Copy(&out, io.LimitReader(r2, int64(limit)+1)); err != nil {
		return bad("%v", err)
	}
	if out.Len() > limit {
		return nil, limited("a block inflates to more than %d bytes", limit)
	}
	if !r2.EOS() {
		return bad("LZMA2 data without an end marker")
	}
	data := out.Bytes()
	comp := len(src) - pos - br.Len()
	pos += comp
	if hasComp && wantComp != uint64(comp) || hasUncomp && wantUncomp != uint64(len(data)) {
		return bad("block sizes disagree with the block header")
	}
	unpadded := hs + comp + checkLen
	pad := (4 - (hs+comp)%4) % 4
	if pos+pad+checkLen > len(src) || !allZero(src[pos:pos+pad]) {
		return bad("bad block padding")
	}
	pos += pad
	chk := src[pos : pos+checkLen]
	pos += checkLen
	var sum []byte
	switch src[7] {
	case 0x01:
		sum = le.AppendUint32(nil, crc32.ChecksumIEEE(data))
	case 0x04:
		sum = le.AppendUint64(nil, crc64.Checksum(data, crc64.MakeTable(crc64.ECMA)))
	case 0x0a:
		s := sha256.Sum256(data)
		sum = s[:]
	}
	if !bytes.Equal(sum, chk) {
		return bad("integrity check mismatch")
	}
	// the index: exactly one record that matches, then the footer, then nothing
	if pos >= len(src) || src[pos] != 0 {
		return bad("more than one block")
	}
	idx := src[pos:]
	i := 1
	nrec, k, ok := xzVarint(idx[i:])
	if !ok || nrec != 1 {
		return bad("the index does not list exactly one block")
	}
	i += k
	up, k, ok := xzVarint(idx[i:])
	if !ok || up != uint64(unpadded) {
		return bad("index unpadded size disagrees with the block")
	}
	i += k
	un, k, ok := xzVarint(idx[i:])
	if !ok || un != uint64(len(data)) {
		return bad("index uncompressed size disagrees with the block")
	}
	i += k
	end := i
	i += (4 - i%4) % 4
	if len(idx) != i+4+12 || !allZero(idx[end:i]) {
		return bad("bad index or trailing data")
	}
	if le.Uint32(idx[i:]) != crc32.ChecksumIEEE(idx[:i]) {
		return bad("index checksum mismatch")
	}
	f := idx[i+4:]
	if le.Uint32(f) != crc32.ChecksumIEEE(f[4:10]) || le.Uint32(f[4:]) != uint32((i+4)/4-1) ||
		!bytes.Equal(f[8:10], src[6:8]) || f[10] != 'Y' || f[11] != 'Z' {
		return bad("bad stream footer")
	}
	return data, nil
}
