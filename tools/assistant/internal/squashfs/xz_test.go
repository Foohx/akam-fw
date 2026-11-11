package squashfs

import (
	"bytes"
	"encoding/binary"
	"errors"
	"hash/crc32"
	"runtime"
	"testing"

	"github.com/ulikunitz/xz"
)

func xzStream(t *testing.T, data []byte, cfg xz.WriterConfig) []byte {
	t.Helper()
	var b bytes.Buffer
	w, err := cfg.NewWriter(&b)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write(data); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// patchBlockHeader edits the first block header and fixes its CRC32.
func patchBlockHeader(s []byte, edit func(h []byte)) []byte {
	c := append([]byte(nil), s...)
	hs := (int(c[12]) + 1) * 4
	h := c[12 : 12+hs]
	edit(h)
	binary.LittleEndian.PutUint32(h[hs-4:], crc32.ChecksumIEEE(h[:hs-4]))
	return c
}

func TestXZDecode(t *testing.T) {
	data := textBytes(3000)
	for name, cfg := range map[string]xz.WriterConfig{
		"crc32":  {CheckSum: xz.CRC32, DictCap: 4096},
		"crc64":  {CheckSum: xz.CRC64, DictCap: 4096},
		"sha256": {CheckSum: xz.SHA256, DictCap: 65536},
		"none":   {CheckSum: xz.None, DictCap: 4096},
	} {
		s := xzStream(t, data, cfg)
		got, err := xzDecode(s, 3000)
		if err != nil || !bytes.Equal(got, data) {
			t.Fatalf("%s: %v", name, err)
		}
		if _, err := xzDecode(s, 2999); !errors.Is(err, ErrLimit) {
			t.Errorf("%s: output cap: %v", name, err)
		}
		// every single-byte corruption and truncation is an error, never a panic or a wrong result
		for i := range s {
			c := append([]byte(nil), s...)
			c[i] ^= 0x55
			if out, err := xzDecode(c, 4096); err == nil && !bytes.Equal(out, data) {
				t.Fatalf("%s: byte %d flipped, wrong data accepted", name, i)
			}
		}
		for n := 0; n < len(s); n++ {
			if _, err := xzDecode(s[:n], 4096); err == nil {
				t.Fatalf("%s: truncation to %d accepted", name, n)
			}
		}
	}
}

func TestXZHostile(t *testing.T) {
	before := decoderPanics.Load()
	defer func() {
		if decoderPanics.Load() != before {
			t.Error("a decoder panic was recovered")
		}
	}()
	data := textBytes(600)
	good := xzStream(t, data, xz.WriterConfig{CheckSum: xz.CRC32, DictCap: 4096})

	t.Run("huge dictionary does not allocate", func(t *testing.T) {
		for _, code := range []byte{39, 40} { // ~2 GiB and 4 GiB - 1
			s := patchBlockHeader(good, func(h []byte) { h[4] = code })
			var m0, m1 runtime.MemStats
			runtime.ReadMemStats(&m0)
			out, err := xzDecode(s, 8192)
			runtime.ReadMemStats(&m1)
			if err != nil || !bytes.Equal(out, data) {
				t.Fatalf("code %d: %v", code, err)
			}
			if d := m1.TotalAlloc - m0.TotalAlloc; d > 8<<20 {
				t.Fatalf("code %d: %d bytes allocated for a 600 byte block", code, d)
			}
		}
		if _, err := xzDecode(patchBlockHeader(good, func(h []byte) { h[4] = 41 }), 8192); !errors.Is(err, ErrCorrupt) {
			t.Fatalf("invalid dictionary code: %v", err)
		}
	})
	t.Run("more than one block", func(t *testing.T) {
		s := xzStream(t, data, xz.WriterConfig{CheckSum: xz.CRC32, DictCap: 4096, BlockSize: 200})
		if _, err := xzDecode(s, 8192); !errors.Is(err, ErrCorrupt) {
			t.Fatalf("multi-block stream: %v", err)
		}
	})
	t.Run("trailing data and concatenation", func(t *testing.T) {
		for name, s := range map[string][]byte{
			"garbage":       append(append([]byte(nil), good...), 1, 2, 3, 4),
			"second stream": append(append([]byte(nil), good...), good...),
		} {
			if _, err := xzDecode(s, 8192); !errors.Is(err, ErrCorrupt) {
				t.Fatalf("%s: %v", name, err)
			}
		}
	})
	t.Run("filters and flags", func(t *testing.T) {
		if _, err := xzDecode(patchBlockHeader(good, func(h []byte) { h[1] |= 1 }), 8192); !errors.Is(err, ErrUnsupported) {
			t.Fatalf("two filters: %v", err)
		}
		if _, err := xzDecode(patchBlockHeader(good, func(h []byte) { h[2] = 0x03 }), 8192); !errors.Is(err, ErrUnsupported) {
			t.Fatalf("delta filter: %v", err)
		}
		if _, err := xzDecode(patchBlockHeader(good, func(h []byte) { h[1] |= 0x04 }), 8192); !errors.Is(err, ErrCorrupt) {
			t.Fatalf("reserved flags: %v", err)
		}
	})
}
