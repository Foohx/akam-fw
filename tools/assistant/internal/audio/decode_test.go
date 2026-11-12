package audio

import (
	"bytes"
	"errors"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
)

// goertzel returns the power of the component at freq Hz in 8 kHz samples.
func goertzel(s []int16, freq float64) float64 {
	w := 2 * math.Pi * freq / Rate
	c := 2 * math.Cos(w)
	var s1, s2 float64
	for _, v := range s {
		s0 := float64(v) + c*s1 - s2
		s2, s1 = s1, s0
	}
	return (s1*s1 + s2*s2 - c*s1*s2) / float64(len(s))
}

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// The fixtures are 0.5 s of a 440 Hz sine made for this repository (synthetic, no third-party audio):
// MP3 22.05 kHz mono, FLAC 16 kHz stereo 16-bit, Ogg Vorbis 44.1 kHz stereo.
func TestDecodeFixtures(t *testing.T) {
	for _, name := range []string{"sine_mono_22k.mp3", "sine_stereo_16k.flac", "sine_stereo_44k.ogg"} {
		t.Run(name, func(t *testing.T) {
			data := fixture(t, name)
			pcm, err := Decode(data)
			if err != nil {
				t.Fatal(err)
			}
			// 0.5 s = 4000 samples; codecs add or drop a little padding
			if len(pcm) < 3500 || len(pcm) > 5500 {
				t.Fatalf("%d samples, want about 4000", len(pcm))
			}
			sig, off := goertzel(pcm, 440), goertzel(pcm, 1500)
			if sig < 100*off || sig < 1e5 {
				t.Errorf("440 Hz power %.0f vs 1500 Hz %.0f: the tone is not there", sig, off)
			}
		})
	}
}

func TestFormatDetectionByMagicBytes(t *testing.T) {
	for name, want := range map[string]string{"sine_mono_22k.mp3": "mp3", "sine_stereo_16k.flac": "flac", "sine_stereo_44k.ogg": "ogg"} {
		if got := Format(fixture(t, name)); got != want {
			t.Errorf("%s: %q, want %q", name, got, want)
		}
	}
	// the file name plays no role: a renamed file is still found, and a lie is not believed
	if got := Format([]byte("ID3\x04\x00")); got != "mp3" {
		t.Errorf("ID3: %q", got)
	}
	if got := Format([]byte{0xFF, 0xFB, 0x90, 0x00}); got != "mp3" {
		t.Errorf("frame sync: %q", got)
	}
	for _, b := range [][]byte{nil, {}, []byte("RIFF"), []byte("hello world"), {0xFF}, {0xFF, 0xE0}} {
		if got := Format(b); got != "" {
			t.Errorf("%q: %q, want none", b, got)
		}
	}
	if _, err := Decode([]byte("not audio at all")); !errors.Is(err, ErrUnsupported) {
		t.Errorf("unknown data: %v", err)
	}
}

// Damaged input must come back as an error (or, for a truncated stream, as the audio before the
// damage), never as a panic or a hang.
func TestDecodeMalformedNeverPanics(t *testing.T) {
	var inputs [][]byte
	for _, name := range []string{"sine_mono_22k.mp3", "sine_stereo_16k.flac", "sine_stereo_44k.ogg"} {
		data := fixture(t, name)
		for _, n := range []int{0, 1, 3, 4, 8, 20, 64, 200, len(data) / 3, len(data) / 2, len(data) - 1} {
			if n <= len(data) {
				inputs = append(inputs, data[:n])
			}
		}
		rng := rand.New(rand.NewSource(1))
		for i := 0; i < 60; i++ { // flip a few random bytes
			d := bytes.Clone(data)
			for k := 0; k < 1+i%5; k++ {
				d[rng.Intn(len(d))] ^= byte(1 + rng.Intn(255))
			}
			inputs = append(inputs, d)
		}
		for i := 0; i < 20; i++ { // magic followed by noise
			junk := make([]byte, 50+rng.Intn(2000))
			rng.Read(junk)
			inputs = append(inputs, append(bytes.Clone(data[:4]), junk...))
		}
	}
	for _, magic := range []string{"fLaC", "OggS", "ID3\x03", "\xff\xfb\x90\x00"} {
		inputs = append(inputs, []byte(magic), append([]byte(magic), make([]byte, 4000)...))
	}
	for i, in := range inputs {
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Errorf("input %d (%d bytes): panic %v", i, len(in), r)
				}
			}()
			pcm, err := Decode(in)
			if err == nil && len(pcm) == 0 {
				t.Errorf("input %d: no error and no audio", i)
			}
		}()
	}
}

func TestDecodeEmptyAudioIsAnError(t *testing.T) {
	if _, err := Decode([]byte("fLaC")); err == nil {
		t.Error("a FLAC with no header decoded")
	}
	if _, err := Decode([]byte("OggS")); err == nil {
		t.Error("an Ogg with no header decoded")
	}
}

func TestResamplerLengthsAndIdentity(t *testing.T) {
	// 8 kHz is the identity
	rs := NewResampler(8000)
	var out []int16
	for i := 0; i < 100; i++ {
		out = rs.Push(float64(i*100-5000), out)
	}
	for i, v := range out {
		if int(v) != i*100-5000 {
			t.Fatalf("identity: sample %d = %d", i, v)
		}
	}
	// one second of input gives 8000 samples (give or take the last partial one) at any rate, and
	// a constant stays constant once the filter has settled
	for _, rate := range []int{6000, 11025, 16000, 22050, 32000, 44100, 48000, 96000} {
		rs := NewResampler(rate)
		var o []int16
		for i := 0; i < rate; i++ {
			o = rs.Push(1000, o)
		}
		if len(o) < 7999 || len(o) > 8000 {
			t.Errorf("%d Hz: %d samples out of one second", rate, len(o))
		}
		for _, v := range o[200:] {
			if v < 998 || v > 1002 {
				t.Fatalf("%d Hz: a constant came out as %d", rate, v)
			}
		}
	}
	// 2:1 without the filter is a box average: only checked on the upsampling-free 8 kHz path above;
	// clipping
	if got := NewResampler(8000).Push(1e9, nil); got[0] != 32767 {
		t.Errorf("clip: %v", got)
	}
}

func sineRMS(rate int, freq float64, seconds int) (in, out float64) {
	rs := NewResampler(rate)
	var o []int16
	var si float64
	n := rate * seconds
	for i := 0; i < n; i++ {
		x := 10000 * math.Sin(2*math.Pi*freq*float64(i)/float64(rate))
		si += x * x
		o = rs.Push(x, o)
	}
	o = o[400:] // let the filter settle
	var so float64
	for _, v := range o {
		so += float64(v) * float64(v)
	}
	return math.Sqrt(si / float64(n)), math.Sqrt(so / float64(len(o)))
}

// Music at 44.1 or 48 kHz must not fold back into the voice band, and the voice band stays.
func TestResamplerRejectsAliasesAndKeepsPassband(t *testing.T) {
	for _, rate := range []int{44100, 48000} {
		// without a low-pass 6 kHz would fold to 2 kHz at 8 kHz output, 7 kHz to 1 kHz, 10 kHz to 2 kHz
		for _, f := range []float64{6000, 7000, 10000} {
			in, out := sineRMS(rate, f, 2)
			if out > in*0.01 {
				t.Errorf("%d Hz, %.0f Hz tone: output RMS %.1f of %.1f (more than -40 dB)", rate, f, out, in)
			}
		}
		for _, f := range []float64{300, 1000, 2000} {
			in, out := sineRMS(rate, f, 2)
			lo := 0.9 // within 1 dB; at 2 kHz the box average of the decimation already costs about 1 dB
			if f >= 2000 {
				lo = 0.8
			}
			if out < in*lo || out > in*1.1 {
				t.Errorf("%d Hz, %.0f Hz tone: output RMS %.1f of %.1f (passband too uneven)", rate, f, out, in)
			}
		}
	}
}
