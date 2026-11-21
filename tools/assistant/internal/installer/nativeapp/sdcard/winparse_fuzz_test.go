package sdcard

import (
	"math/rand"
	"strings"
	"testing"
)

// Whatever the device answers, the parsers never panic: random buffers, and valid buffers with bytes flipped
// and cut short.
func TestWinParsersNeverPanic(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	seeds := [][]byte{
		descBuf(busUsb, true, "V", "P", "R", "S"),
		layoutBuf(0, 1, [16]byte{}, partSpec{mbrType: 0x0C}, partSpec{mbrType: 0x42}),
		layoutBuf(1, 0, [16]byte{9}, partSpec{isGPT: true, gpt: gptGUID(gptESP)}),
		extentsBuf([3]int64{1, 0, 100}, [3]int64{2, 0, 100}),
		geoBuf(512, 1<<30),
	}
	all := func(b []byte) {
		ParseDeviceDescriptor(b)
		ParseGeometryEx(b)
		ParseLengthInfo(b)
		if l, err := ParseDriveLayoutEx(b); err == nil {
			l.Classify()
		}
		DiskExtentCount(b)
		ParseDiskExtents(b)
	}
	for i := 0; i < 20000; i++ {
		var b []byte
		if i%2 == 0 {
			b = make([]byte, rng.Intn(600))
			rng.Read(b)
		} else {
			b = append([]byte(nil), seeds[rng.Intn(len(seeds))]...)
			for k := rng.Intn(6); k > 0 && len(b) > 0; k-- {
				b[rng.Intn(len(b))] = byte(rng.Intn(256))
			}
			b = b[:rng.Intn(len(b)+1)]
		}
		all(b)
	}
	for n := 0; n < 700; n++ { // every length of every seed
		for _, s := range seeds {
			if n <= len(s) {
				all(s[:n])
			}
		}
	}
}

// Strings from the device cannot carry control characters or the identity separator.
func TestDescriptorStringsArePrintable(t *testing.T) {
	d, err := ParseDeviceDescriptor(descBuf(busUsb, true, "Ge\x1b[2Jn|x", "P\xffQ", "1.0", "S"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.ContainsAny(d.Vendor+d.Product, "\x1b|\xff") || d.Vendor != "Ge?[2Jn?x" {
		t.Errorf("%q %q", d.Vendor, d.Product)
	}
}

// A drive size that is not a whole number of sectors is refused.
func TestBuildWinFactsRefusesRaggedSize(t *testing.T) {
	_, err := BuildWinFacts(2, WinDescriptor{BusType: busUsb}, WinGeometry{BytesPerSector: 512, DiskSize: 1<<30 + 1}, 0, WinLayout{}, false)
	if err == nil {
		t.Error("ragged size accepted")
	}
}
