package audio

import (
	"bytes"
	"encoding/binary"
	"runtime"
	"strings"
	"testing"
)

var oggCRCTable = func() (t [256]uint32) {
	for i := range t {
		r := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if r&0x80000000 != 0 {
				r = r<<1 ^ 0x04c11db7
			} else {
				r <<= 1
			}
		}
		t[i] = r
	}
	return
}()

func oggCRC(page []byte) uint32 {
	var c uint32
	for _, b := range page {
		c = c<<8 ^ oggCRCTable[byte(c>>24)^b]
	}
	return c
}

// patchOgg copies data, applies edit at the offset of marker+delta, and fixes the CRC of the page
// holding that offset.
func patchOgg(t *testing.T, data []byte, marker string, delta int, edit []byte) []byte {
	t.Helper()
	out := append([]byte(nil), data...)
	at := bytes.Index(out, []byte(marker))
	if at < 0 {
		t.Fatalf("no %q in the fixture", marker)
	}
	at += delta
	copy(out[at:], edit)
	for off := 0; off+27 <= len(out); {
		nseg := int(out[off+26])
		size := 27 + nseg
		for _, l := range out[off+27 : off+27+nseg] {
			size += int(l)
		}
		if at >= off && at < off+size {
			binary.LittleEndian.PutUint32(out[off+22:], 0)
			binary.LittleEndian.PutUint32(out[off+22:], oggCRC(out[off:off+size]))
			return out
		}
		off += size
	}
	t.Fatal("offset not in a page")
	return nil
}

// Crafted Vorbis headers that the library would turn into gigabytes are refused before it runs,
// and quickly, with a small memory footprint.
func TestOggCraftedHeadersAreRejected(t *testing.T) {
	good := fixture(t, "sine_stereo_44k.ogg")
	if _, err := Decode(good); err != nil {
		t.Fatalf("the unpatched fixture: %v", err)
	}
	vendorLen := func() int { // comment header: type, "vorbis", vendor length, vendor, count
		at := bytes.Index(good, []byte("\x03vorbis"))
		return int(binary.LittleEndian.Uint32(good[at+7:]))
	}()
	cases := map[string][]byte{
		"comment count 0x7fffffff": patchOgg(t, good, "\x03vorbis", 7+4+vendorLen, []byte{0xff, 0xff, 0xff, 0x7f}),
		"vendor length huge":       patchOgg(t, good, "\x03vorbis", 7, []byte{0xff, 0xff, 0xff, 0x7f}),
		"codebook with 0 entries":  patchOgg(t, good, "\x05vorbis", 7+6, []byte{0, 0, 0}),
		"codebook 16M entries":     patchOgg(t, good, "\x05vorbis", 7+6, []byte{0xff, 0xff, 0xff}),
		"codebook dimension 0":     patchOgg(t, good, "\x05vorbis", 7+4, []byte{0, 0}),
		"255 channels":             patchOgg(t, good, "\x01vorbis", 7+4, []byte{255}),
		"sample rate 4 GHz":        patchOgg(t, good, "\x01vorbis", 7+5, []byte{0xff, 0xff, 0xff, 0xff}),
		"block size 2^15":          patchOgg(t, good, "\x01vorbis", 7+21, []byte{0xf6}),
	}
	for name, data := range cases {
		var before, after runtime.MemStats
		runtime.ReadMemStats(&before)
		pcm, err := Decode(data)
		runtime.ReadMemStats(&after)
		t.Logf("%s: %v", name, err)
		if err == nil || pcm != nil {
			t.Errorf("%s: accepted (%d samples)", name, len(pcm))
		}
		if grew := after.TotalAlloc - before.TotalAlloc; grew > 64<<20 {
			t.Errorf("%s: %d MiB allocated", name, grew>>20)
		}
	}
}

func TestOggHeaderCheckLeavesOtherStreamsAlone(t *testing.T) {
	opus := fixture(t, "sine_opus.opus")
	if err := checkVorbisHeaders(opus); err != nil {
		t.Errorf("an Opus stream is not for this check: %v", err)
	}
	if _, err := Decode(opus); err == nil || strings.Contains(err.Error(), "panic") {
		t.Errorf("opus: %v", err)
	}
	if err := checkVorbisHeaders(fixture(t, "sine_stereo_44k.ogg")); err != nil {
		t.Errorf("the real file: %v", err)
	}
	if err := checkVorbisHeaders([]byte("OggS")); err != nil && !strings.Contains(err.Error(), "Vorbis") {
		t.Errorf("short: %v", err)
	}
}

func TestLookup1Values(t *testing.T) {
	for _, c := range []struct{ e, d, want int }{{1, 1, 1}, {8, 3, 2}, {9, 3, 2}, {27, 3, 3}, {100, 2, 10}, {65536, 4, 16}, {5, 1, 5}} {
		if got := lookup1Values(c.e, c.d); got != c.want {
			t.Errorf("lookup1Values(%d,%d) = %d, want %d", c.e, c.d, got, c.want)
		}
	}
}
