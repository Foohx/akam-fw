package rtsp

import (
	"bytes"
	"encoding/binary"
	"math"
	"runtime"
	"testing"
)

func le32(v uint32) []byte { return binary.LittleEndian.AppendUint32(nil, v) }

// A 20-byte file whose "fmt " chunk claims 0xFFFFFFFF bytes used to wrap size+size&1 to 0 and panic.
func TestWAVHugeFmtChunkIsRefused(t *testing.T) {
	b := append([]byte("RIFF"), le32(12)...)
	b = append(b, "WAVEfmt "...)
	b = append(b, le32(0xFFFFFFFF)...)
	if len(b) != 20 {
		t.Fatalf("test file is %d bytes", len(b))
	}
	if _, err := ReadWAV(bytes.NewReader(b)); err == nil {
		t.Error("accepted")
	}
	// an odd, still too large size
	b = append([]byte("RIFF"), le32(12)...)
	b = append(b, "WAVEfmt "...)
	b = append(b, le32(0xFFFFFFFE)...)
	if _, err := ReadWAV(bytes.NewReader(b)); err == nil {
		t.Error("accepted")
	}
}

// A 48-byte file whose data chunk claims 4 GiB must not allocate it (nor a float64 per frame).
func TestWAVHugeDataSizeAllocatesNothing(t *testing.T) {
	for _, size := range []uint32{0xFFFFFFFF, 0xFFFFFFF0, 0x7FFFFFFF} {
		b := append([]byte("RIFF"), le32(40)...)
		b = append(b, "WAVEfmt "...)
		b = append(b, le32(16)...)
		b = append(b, 1, 0, 1, 0)     // PCM mono
		b = append(b, le32(8000)...)  // rate
		b = append(b, le32(16000)...) // byte rate
		b = append(b, 2, 0, 16, 0)    // block align, bits
		b = append(b, "data"...)
		b = append(b, le32(size)...)
		b = append(b, 1, 0, 2, 0) // 2 samples
		if len(b) != 48 {
			t.Fatalf("test file is %d bytes", len(b))
		}
		var m0, m1 runtime.MemStats
		runtime.ReadMemStats(&m0)
		out, err := ReadWAV(bytes.NewReader(b))
		runtime.ReadMemStats(&m1)
		if err != nil || len(out) != 2 {
			t.Errorf("size %#x: %v %v", size, out, err)
		}
		if m1.TotalAlloc-m0.TotalAlloc > 1<<20 {
			t.Errorf("size %#x: %d bytes allocated", size, m1.TotalAlloc-m0.TotalAlloc)
		}
	}
}

// An unknown chunk claiming more than the file holds, and absurd formats, are errors, not panics.
func TestWAVHostileHeaders(t *testing.T) {
	skip := append([]byte("RIFF"), le32(100)...)
	skip = append(skip, "WAVEJUNK"...)
	skip = append(skip, le32(0xFFFFFFFF)...)
	if _, err := ReadWAV(bytes.NewReader(skip)); err == nil {
		t.Error("skip chunk past the end accepted")
	}
	// 4 GHz and 1 MHz sample rates are refused (limit 384 kHz)
	for _, rate := range []uint32{0xFFFFFFFF, 1_000_000} {
		if _, err := ReadWAV(bytes.NewReader(wavBytes(rate, 1, 16, make([]byte, 64)))); err == nil {
			t.Errorf("rate %d accepted", rate)
		}
	}
	// 65535 channels: a frame is read, nothing else is trusted
	if _, err := ReadWAV(bytes.NewReader(wavBytes(8000, 0xFFFF, 16, make([]byte, 64)))); err != nil {
		t.Errorf("65535 channels with a partial frame: %v", err)
	}
}

// More than MaxSeconds of audio is refused instead of filling the memory.
func TestWAVTooLongIsRefused(t *testing.T) {
	// a 1 Hz "sample rate" keeps the file tiny: 30*60 s = 1800 frames are allowed, 1801 are not
	if out, err := ReadWAV(bytes.NewReader(wavBytes(1, 1, 8, bytes.Repeat([]byte{128}, 1800)))); err != nil {
		t.Errorf("1800 s: %v (%d samples)", err, len(out))
	}
	if _, err := ReadWAV(bytes.NewReader(wavBytes(1, 1, 8, bytes.Repeat([]byte{128}, 1801)))); err == nil {
		t.Error("1801 s accepted")
	}
}

// The WAV path has the same low-pass as the other formats: a 6 kHz tone at 48 kHz does not fold back.
func TestWAVLowPassesBeforeDecimating(t *testing.T) {
	var data []byte
	for i := 0; i < 48000; i++ {
		v := int16(10000 * math.Sin(2*math.Pi*6000*float64(i)/48000))
		data = binary.LittleEndian.AppendUint16(data, uint16(v))
	}
	out, err := ReadWAV(bytes.NewReader(wavBytes(48000, 1, 16, data)))
	if err != nil || len(out) != 8000 {
		t.Fatalf("%v %d", err, len(out))
	}
	var sum float64
	for _, v := range out[1000:] {
		sum += float64(v) * float64(v)
	}
	if rms := math.Sqrt(sum / float64(len(out)-1000)); rms > 150 {
		t.Errorf("alias RMS %.0f (a 6 kHz tone of RMS 7071 should be 48 dB down)", rms)
	}
}

// NaN and Inf samples in a float WAV do not poison the output.
func TestWAVFloatNaN(t *testing.T) {
	var data []byte
	for i := 0; i < 4800; i++ {
		v := float32(0.1)
		if i == 10 {
			v = float32(math.NaN())
		}
		if i == 11 {
			v = float32(math.Inf(1))
		}
		data = binary.LittleEndian.AppendUint32(data, math.Float32bits(v))
	}
	b := wavBytes(48000, 1, 32, data)
	binary.LittleEndian.PutUint16(b[20:], 3) // IEEE float
	out, err := ReadWAV(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	if v := out[len(out)-1]; v < 3000 || v > 3500 {
		t.Errorf("last sample %d, want about 3277", v)
	}
}
