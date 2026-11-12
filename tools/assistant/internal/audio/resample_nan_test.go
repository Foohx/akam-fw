package audio

import (
	"math"
	"testing"
)

// A NaN or an infinity in the input must not stay in the filter state: the output afterwards is
// the same as for a clean signal.
func TestResamplerSurvivesNaNAndInf(t *testing.T) {
	rs := NewResampler(48000)
	var out []int16
	for i := 0; i < 48000; i++ {
		v := 1000.0
		switch i {
		case 100:
			v = math.NaN()
		case 101:
			v = math.Inf(1)
		case 102:
			v = math.Inf(-1)
		case 103:
			v = 1e300
		}
		out = rs.Push(v, out)
	}
	if len(out) != 8000 {
		t.Fatalf("%d samples", len(out))
	}
	for i := 4000; i < len(out); i++ {
		if out[i] < 990 || out[i] > 1010 {
			t.Fatalf("sample %d = %d: the filter state is poisoned", i, out[i])
		}
	}
}

func TestResamplerClampsHugeInput(t *testing.T) {
	rs := NewResampler(8000)
	out := rs.Push(1e30, nil)
	if len(out) != 1 || out[0] != 32767 {
		t.Errorf("%v", out)
	}
	if out = rs.Push(math.NaN(), nil); len(out) != 1 || out[0] != 0 {
		t.Errorf("%v", out)
	}
}
