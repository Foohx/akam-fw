package audio

import (
	"bytes"
	"encoding/binary"
	"errors"
)

// The Vorbis decoder library sizes its tables from numbers in the stream headers without checking
// them (a crafted comment count or codebook allocates gigabytes: a fatal out-of-memory that
// cannot be recovered). checkVorbisHeaders reads the three header packets of the first logical
// stream itself and rejects anything out of the ranges real files use, before the library sees
// the data. A stream that does not start with a Vorbis identification header (Opus, FLAC in Ogg)
// is left to the library, which refuses it without allocating.

var errVorbisHeaders = errors.New("unreadable Ogg Vorbis headers")

const (
	vorbisMaxChannels   = 16
	vorbisMaxCodebooks  = 256
	vorbisMaxEntries    = 1 << 16 // entries of one codebook
	vorbisMaxCodebookSz = 4 << 20 // entries * dimensions of one codebook
	vorbisMaxTotalSz    = 8 << 20 // entries * dimensions over all codebooks
)

// oggHeaderPackets returns the first three packets of the Ogg stream in data (the Vorbis headers).
func oggHeaderPackets(data []byte) ([][]byte, error) {
	var packets [][]byte
	var cur []byte
	for len(packets) < 3 {
		if len(data) < 27 || !bytes.HasPrefix(data, []byte("OggS")) {
			return nil, errVorbisHeaders
		}
		nseg := int(data[26])
		if len(data) < 27+nseg {
			return nil, errVorbisHeaders
		}
		lace := data[27 : 27+nseg]
		body := data[27+nseg:]
		for _, l := range lace {
			if len(body) < int(l) {
				return nil, errVorbisHeaders
			}
			cur = append(cur, body[:l]...)
			body = body[l:]
			if l < 255 {
				packets = append(packets, cur)
				cur = nil
				if len(packets) == 3 {
					return packets, nil
				}
			}
		}
		data = body
	}
	return packets, nil
}

type vbits struct {
	b   []byte
	pos int // in bits
	bad bool
}

func (r *vbits) read(n int) uint32 { // n <= 32, least significant bit first
	var v uint64
	for i := 0; i < n; i++ {
		byteI := r.pos >> 3
		if byteI >= len(r.b) {
			r.bad = true
			return 0
		}
		v |= uint64(r.b[byteI]>>(uint(r.pos)&7)&1) << uint(i)
		r.pos++
	}
	return uint32(v)
}

func (r *vbits) left() int { return len(r.b)*8 - r.pos }

// checkVorbisHeaders returns an error when data is an Ogg Vorbis stream whose headers would make
// the decoder allocate more than a real file needs.
func checkVorbisHeaders(data []byte) error {
	// only a stream that begins with a Vorbis identification header is checked
	if len(data) < 27 || len(data) < 27+int(data[26]) || !bytes.HasPrefix(data[27+int(data[26]):], []byte("\x01vorbis")) {
		return nil
	}
	pk, err := oggHeaderPackets(data)
	if err != nil {
		return err
	}
	for i, want := range []string{"\x01vorbis", "\x03vorbis", "\x05vorbis"} {
		if !bytes.HasPrefix(pk[i], []byte(want)) {
			return errVorbisHeaders
		}
	}
	if err := checkVorbisIdent(pk[0][7:]); err != nil {
		return err
	}
	if err := checkVorbisComment(pk[1][7:]); err != nil {
		return err
	}
	return checkVorbisSetup(pk[2][7:])
}

func checkVorbisIdent(h []byte) error {
	if len(h) < 23 {
		return errVorbisHeaders
	}
	le := binary.LittleEndian
	channels := int(h[4])
	rate := le.Uint32(h[5:])
	short, long := h[21]&0x0F, h[21]>>4
	if le.Uint32(h) != 0 || channels < 1 || channels > vorbisMaxChannels || rate < 1 || rate > MaxRate ||
		short < 6 || long > 13 || short > long || h[22]&1 == 0 { // block sizes 64..8192
		return errors.New("unsupported Ogg Vorbis stream parameters")
	}
	return nil
}

func checkVorbisComment(h []byte) error {
	le := binary.LittleEndian
	if len(h) < 4 {
		return errVorbisHeaders
	}
	vl := uint64(le.Uint32(h))
	h = h[4:]
	if vl > uint64(len(h)) || len(h)-int(vl) < 4 {
		return errVorbisHeaders
	}
	h = h[vl:]
	n := uint64(le.Uint32(h))
	h = h[4:]
	if n > uint64(len(h))/4 { // every comment takes at least its 4-byte length
		return errVorbisHeaders
	}
	for i := uint64(0); i < n; i++ {
		if len(h) < 4 {
			return errVorbisHeaders
		}
		l := uint64(le.Uint32(h))
		h = h[4:]
		if l > uint64(len(h)) {
			return errVorbisHeaders
		}
		h = h[l:]
	}
	return nil
}

// checkVorbisSetup walks the codebooks of the setup header (the only part whose sizes come from
// the stream without a bound; the rest is limited by field widths and the block size).
func checkVorbisSetup(h []byte) error {
	bad := errors.New("unsupported Ogg Vorbis codebook")
	r := &vbits{b: h}
	books := int(r.read(8)) + 1
	if books > vorbisMaxCodebooks {
		return bad
	}
	total := 0
	for i := 0; i < books; i++ {
		if r.read(24) != 0x564342 {
			return bad
		}
		dim := int(r.read(16))
		entries := int(r.read(24))
		if r.bad || dim < 1 || entries < 1 || entries > vorbisMaxEntries || entries*dim > vorbisMaxCodebookSz {
			return bad
		}
		if total += entries * dim; total > vorbisMaxTotalSz {
			return bad
		}
		if r.read(1) == 0 { // not ordered
			sparse := r.read(1) == 1
			for e := 0; e < entries; e++ {
				if !sparse || r.read(1) == 1 {
					r.read(5)
				}
				if r.bad {
					return bad
				}
			}
		} else {
			cur, length := 0, int(r.read(5))+1
			for cur < entries {
				if length > 32 {
					return bad
				}
				n := int(r.read(bitsFor(entries - cur)))
				if r.bad || n > entries-cur {
					return bad
				}
				cur += n
				length++
			}
		}
		switch lt := r.read(4); lt {
		case 0:
		case 1, 2:
			r.read(32)
			r.read(32)
			valueBits := int(r.read(4)) + 1
			r.read(1)
			count := entries * dim
			if lt == 1 {
				count = lookup1Values(entries, dim)
			}
			if r.bad || count*valueBits > r.left() { // the table must fit in the remaining header
				return bad
			}
			r.pos += count * valueBits
		default:
			return bad
		}
		if r.bad {
			return bad
		}
	}
	return nil
}

func bitsFor(x int) int { // ilog of the Vorbis spec
	n := 0
	for ; x > 0; x >>= 1 {
		n++
	}
	return n
}

// lookup1Values is the greatest integer v with v^dim <= entries.
func lookup1Values(entries, dim int) int {
	v := 1
	for {
		p := 1
		for i := 0; i < dim; i++ {
			p *= v + 1
			if p > entries {
				return v
			}
		}
		v++
	}
}
