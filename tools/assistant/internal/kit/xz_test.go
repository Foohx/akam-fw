package kit_test

import (
	"archive/tar"
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"io"
	"math/rand"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/ulikunitz/xz"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func writeTemp(t *testing.T, b []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "k.tar.xz")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// readAll opens p with the default limits and reads it to the end.
func readAll(p string) ([]byte, error) {
	rc, err := kit.XZReader(p, kit.XZLimits{})
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(rc)
}

// sample is compressible but not trivial data, a few hundred KiB.
func sample() []byte {
	r := rand.New(rand.NewSource(1))
	var b bytes.Buffer
	for b.Len() < 300<<10 {
		b.WriteString(strings.Repeat("akam kit line ", r.Intn(20)+1))
		b.WriteByte(byte('a' + r.Intn(26)))
	}
	return b.Bytes()
}

func varint(v uint64) []byte {
	var out []byte
	for v >= 0x80 {
		out = append(out, byte(v)|0x80)
		v >>= 7
	}
	return append(out, byte(v))
}

func le32(v uint32) []byte { return binary.LittleEndian.AppendUint32(nil, v) }

func pad4(b []byte) []byte {
	for len(b)%4 != 0 {
		b = append(b, 0)
	}
	return b
}

// blockHeader is a 12-byte block header with one LZMA2 filter of the given dictionary code.
func blockHeader(dictCode byte) []byte {
	h := []byte{2, 0x00, 0x21, 0x01, dictCode, 0, 0, 0}
	return append(h, le32(crc32.ChecksumIEEE(h))...)
}

type rec struct{ unpadded, uncompressed uint64 }

func index(recs ...rec) []byte {
	b := append([]byte{0}, varint(uint64(len(recs)))...)
	for _, r := range recs {
		b = append(b, varint(r.unpadded)...)
		b = append(b, varint(r.uncompressed)...)
	}
	b = pad4(b)
	return append(b, le32(crc32.ChecksumIEEE(b))...)
}

// handBuilt is a stream with a CRC32 check whose block data is garbage: only the pre-scan may look at it.
func handBuilt(dictCode byte, recs ...rec) []byte {
	hdr := []byte{0xFD, '7', 'z', 'X', 'Z', 0, 0, 0x01}
	hdr = append(hdr, le32(crc32.ChecksumIEEE(hdr[6:8]))...)
	out := append(hdr, blockHeader(dictCode)...)
	out = append(out, make([]byte, 8+4)...) // 8 bytes of "compressed data", 4 of CRC32 check
	idx := index(recs...)
	out = append(out, idx...)
	ftr := append(le32(uint32(len(idx)/4-1)), 0, 0x01)
	out = append(out, le32(crc32.ChecksumIEEE(ftr))...)
	out = append(out, ftr...)
	return append(out, 'Y', 'Z')
}

func goodRec() rec { return rec{unpadded: 12 + 8 + 4, uncompressed: 100} }

func TestXZValidDecodes(t *testing.T) {
	want := sample()
	got, err := readAll(writeTemp(t, kittest.XZ(t, want)))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("decode: %v equal=%v", err, bytes.Equal(got, want))
	}
}

func TestXZTruncatedErrors(t *testing.T) {
	b := kittest.XZ(t, sample())
	for _, n := range []int{0, 5, 12, 30, len(b) / 2, len(b) - 13, len(b) - 12, len(b) - 1} {
		if _, err := readAll(writeTemp(t, b[:n])); err == nil {
			t.Errorf("truncated to %d of %d bytes: no error", n, len(b))
		}
	}
}

func TestXZBitFlipErrors(t *testing.T) {
	b := kittest.XZ(t, sample())
	for _, off := range []int{len(b) / 4, len(b) / 2, len(b) * 3 / 4, len(b) - 40} {
		c := append([]byte(nil), b...)
		c[off] ^= 0x10
		if got, err := readAll(writeTemp(t, c)); err == nil {
			t.Errorf("flip at %d: no error (got %d bytes)", off, len(got))
		}
	}
}

func TestXZConcatenatedAndTrailingRefused(t *testing.T) {
	b := kittest.XZ(t, sample())
	if _, err := readAll(writeTemp(t, append(append([]byte(nil), b...), b...))); err == nil {
		t.Error("two concatenated streams accepted")
	}
	if _, err := readAll(writeTemp(t, append(append([]byte(nil), b...), 0, 0, 0, 0))); err == nil {
		t.Error("trailing stream padding accepted")
	}
	if _, err := readAll(writeTemp(t, append(append([]byte(nil), b...), "junk"...))); err == nil {
		t.Error("trailing garbage accepted")
	}
}

func TestXZMultiBlockRefused(t *testing.T) {
	r := goodRec()
	_, err := kit.XZReader(writeTemp(t, handBuilt(0x16, r, r)), kit.XZLimits{})
	if err == nil || !strings.Contains(err.Error(), "block") {
		t.Fatalf("two blocks: %v", err)
	}
	// Sanity: the same builder with one record passes the pre-scan.
	rc, err := kit.XZReader(writeTemp(t, handBuilt(0x16, goodRec())), kit.XZLimits{})
	if err != nil {
		t.Fatalf("hand-built single block refused by the pre-scan: %v", err)
	}
	rc.Close()
}

func TestXZHugeDictionaryRefusedWithoutAllocating(t *testing.T) {
	p := writeTemp(t, handBuilt(40, goodRec())) // code 40: 4 GiB - 1
	runtime.GC()
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	_, err := kit.XZReader(p, kit.XZLimits{})
	runtime.ReadMemStats(&b)
	if err == nil || !strings.Contains(err.Error(), "dictionary") {
		t.Fatalf("4 GiB dictionary: %v", err)
	}
	if d := b.TotalAlloc - a.TotalAlloc; d > 128<<20 {
		t.Errorf("the refusal allocated %d bytes", d)
	}
	// Code 28 is 64 MiB (accepted by the pre-scan), code 30 is 128 MiB (refused).
	rc, err := kit.XZReader(writeTemp(t, handBuilt(28, goodRec())), kit.XZLimits{})
	if err != nil {
		t.Errorf("a 64 MiB dictionary header was refused: %v", err)
	} else {
		rc.Close()
	}
	if _, err := kit.XZReader(writeTemp(t, handBuilt(30, goodRec())), kit.XZLimits{}); err == nil {
		t.Error("a 128 MiB dictionary was accepted")
	}
}

func TestXZ64MiBDictionaryAccepted(t *testing.T) {
	var out bytes.Buffer
	w, err := xz.WriterConfig{DictCap: 64 << 20}.NewWriter(&out)
	if err != nil {
		t.Fatal(err)
	}
	want := sample()
	if _, err := w.Write(want); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	got, err := readAll(writeTemp(t, out.Bytes()))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("64 MiB dictionary stream: %v", err)
	}
	if _, err := kit.XZReader(writeTemp(t, out.Bytes()), kit.XZLimits{MaxDict: 32 << 20}); err == nil {
		t.Error("a 64 MiB dictionary passed a 32 MiB cap")
	}
}

func TestXZDeclaredSizeAboveOneGiBRefused(t *testing.T) {
	r := goodRec()
	r.uncompressed = 1<<30 + 1
	if _, err := kit.XZReader(writeTemp(t, handBuilt(0x16, r)), kit.XZLimits{}); err == nil || !strings.Contains(err.Error(), "uncompressed") {
		t.Fatalf("index declaring more than 1 GiB: %v", err)
	}
	r.uncompressed = 1 << 30
	rc, err := kit.XZReader(writeTemp(t, handBuilt(0x16, r)), kit.XZLimits{})
	if err != nil {
		t.Fatalf("exactly 1 GiB refused: %v", err)
	}
	rc.Close()
}

// A lying index (the stream decodes to more than it declares) is an error in the decoder, and the
// 1 GiB backstop of readArchive fails on its own as soon as more than its limit is read.
func TestXZLyingIndexAndBackstop(t *testing.T) {
	data := sample()
	b := kittest.XZ(t, data)
	idxSize := (int(binary.LittleEndian.Uint32(b[len(b)-8:len(b)-4])) + 1) * 4
	idxOff := len(b) - 12 - idxSize
	idx := b[idxOff : idxOff+idxSize]
	pos := 2 // after the indicator and the record count (1)
	readV := func() uint64 {
		var v uint64
		for i := 0; ; i++ {
			c := idx[pos]
			pos++
			v |= uint64(c&0x7f) << (7 * uint(i))
			if c&0x80 == 0 {
				return v
			}
		}
	}
	r := rec{unpadded: readV(), uncompressed: readV()}
	if r.uncompressed != uint64(len(data)) {
		t.Fatalf("parsed index says %d, want %d", r.uncompressed, len(data))
	}
	r.uncompressed = 1000
	newIdx := index(r)
	if len(newIdx) != idxSize {
		t.Fatalf("index size changed (%d -> %d)", idxSize, len(newIdx))
	}
	lie := append(append([]byte(nil), b[:idxOff]...), newIdx...)
	lie = append(lie, b[idxOff+idxSize:]...)
	if got, err := readAll(writeTemp(t, lie)); err == nil {
		t.Errorf("a lying index decoded cleanly (%d bytes)", len(got))
	}

	rc, err := kit.XZReader(writeTemp(t, b), kit.XZLimits{})
	if err != nil {
		t.Fatal(err)
	}
	defer rc.Close()
	if _, err := io.Copy(io.Discard, kit.NewLimitReader(rc, 1000)); err == nil || !strings.Contains(err.Error(), "larger than") {
		t.Errorf("limitReader did not stop a stream over its limit: %v", err)
	}
}

func TestXZPeakMemoryOfAKitDecode(t *testing.T) {
	var out bytes.Buffer
	w, err := xz.WriterConfig{DictCap: 64 << 20}.NewWriter(&out) // what xz -9e declares
	if err != nil {
		t.Fatal(err)
	}
	_, _ = w.Write(sample())
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	p := writeTemp(t, out.Bytes())
	runtime.GC()
	var a, b runtime.MemStats
	runtime.ReadMemStats(&a)
	if _, err := readAll(p); err != nil {
		t.Fatal(err)
	}
	runtime.ReadMemStats(&b)
	if d := b.TotalAlloc - a.TotalAlloc; d > 100<<20 {
		t.Errorf("decoding allocated %d bytes (limit 100 MiB)", d)
	}
}

func TestSymlinkFailureFallsBackToCopyAndSidecar(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{
		ManifestExtra: "link dropbearmulti 777 overlay/bin/dropbearkey\n",
		TarMutate: func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/overlay/bin/dropbearkey", Type: tar.TypeSymlink, Mode: 0o777, Linkname: "dropbearmulti"})
		},
	})
	to := filepath.Join(t.TempDir(), "kit")
	r, err, _ := verify(t, b, func(o *kit.Options) {
		o.ExtractTo = to
		o.SetSymlink(func(string, string) error { return os.ErrPermission })
	})
	if err != nil {
		t.Fatalf("verification with no symlink privilege: %v", err)
	}
	r.Close()
	for _, l := range []string{"dropbear", "dropbearkey"} {
		p := filepath.Join(to, "overlay", "bin", l)
		st, err := os.Lstat(p)
		if err != nil || !st.Mode().IsRegular() {
			t.Fatalf("%s is not a regular file: %v", l, err)
		}
		got, _ := os.ReadFile(p)
		want, _ := os.ReadFile(filepath.Join(to, "bin", "dropbearmulti"))
		if !bytes.Equal(got, want) {
			t.Errorf("%s is not a copy of dropbearmulti", l)
		}
	}
	side, err := os.ReadFile(filepath.Join(to, ".akam-links"))
	if err != nil || string(side) != "overlay/bin/dropbear dropbearmulti\noverlay/bin/dropbearkey dropbearmulti\n" {
		t.Fatalf("sidecar: %q %v", side, err)
	}
	// Directory mode accepts the copy form...
	var o kit.Options
	if _, err := o.VerifyExtracted(to); err != nil {
		t.Fatalf("directory mode on the copy form: %v", err)
	}
	// ...and refuses a copy that differs from its target.
	if err := os.WriteFile(filepath.Join(to, "overlay", "bin", "dropbear"), []byte("other\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := o.VerifyExtracted(to); err == nil {
		t.Error("a tampered link copy was accepted")
	}
}

func TestSidecarInconsistentWithManifestRefused(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	to := filepath.Join(t.TempDir(), "kit")
	r, err, _ := verify(t, b, func(o *kit.Options) { o.ExtractTo = to })
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
	// The links are real symlinks here: a sidecar claiming one as a copy is inconsistent.
	if err := os.WriteFile(filepath.Join(to, ".akam-links"), []byte("overlay/bin/dropbear dropbearmulti\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var o kit.Options
	if _, err := o.VerifyExtracted(to); err == nil {
		t.Error("a sidecar over a real symlink was accepted")
	}
	if err := os.WriteFile(filepath.Join(to, ".akam-links"), []byte("bin/flash_tool dropbearmulti\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := o.VerifyExtracted(to); err == nil {
		t.Error("a sidecar line for another path was accepted")
	}
}

func TestWindowsReservedNamesRefused(t *testing.T) {
	isLocal := func(name string) bool {
		for _, p := range strings.Split(name, "/") {
			switch strings.ToUpper(strings.TrimSuffix(p, filepath.Ext(p))) {
			case "NUL", "CON", "PRN", "AUX", "COM1", "LPT1":
				return false
			}
		}
		return true
	}
	for _, n := range []string{"NUL", "con", "docs/Aux.txt"} {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Files: map[string][]byte{n: []byte("x\n")}})
		_, err, _ := verify(t, b, func(o *kit.Options) { o.SetIsLocal(isLocal) })
		wantFail(t, err, "not a safe local path")
	}
	// With the real filepath.IsLocal the plain kit passes.
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	r, err, _ := verify(t, b, nil)
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
}

func TestModeComparisonCanBeDisabled(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	to := filepath.Join(t.TempDir(), "kit")
	r, err, _ := verify(t, b, func(o *kit.Options) { o.ExtractTo = to })
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
	if err := os.Chmod(filepath.Join(to, "supported_hardware.txt"), 0o600); err != nil {
		t.Fatal(err)
	}
	var o kit.Options
	if _, err := o.VerifyExtracted(to); err == nil {
		t.Fatal("a mode mismatch was accepted")
	}
	defer kit.SetModesMatter(kit.SetModesMatter(false))
	if _, err := o.VerifyExtracted(to); err != nil {
		t.Errorf("mode comparison still on: %v", err)
	}
}

func TestKittestAvailable(t *testing.T) {
	if !kittest.Available() {
		t.Fatal("kittest.Available() must be true everywhere")
	}
}
