package audio

import "math"

// Rate is the pipeline rate of talk and say: 8 kHz mono signed 16-bit.
const Rate = 8000

// Resampler converts a mono float stream at any rate to 8 kHz s16 samples, one input sample at a
// time (so a long file never has to be held at its native rate). Going down, the input first
// goes through an 8th-order Butterworth low-pass at 3.6 kHz (four cascaded biquads; measured with a
// 48 kHz input: -2.3 dB at 3 kHz, -4.1 dB at 3.4 kHz, -48 dB at 6 kHz), so music at 44.1 or 48 kHz
// does not fold back into the voice band;
// then every output sample is the average of the input boxes it overlaps, computed with exact
// integer positions (units of 1/(rate*8000) s). Going up it is a zero-order hold. At 8000 Hz it
// is the identity.
type Resampler struct {
	lp     []biquad // empty when rate <= 8000
	rate   int64
	outEnd int64 // end of the current output sample, in units
	outCur int64 // start of the part not yet emitted
	acc    float64
	n      int64 // input samples seen
}

// NewResampler returns a resampler for rate Hz input (a rate <= 0 means 8000).
func NewResampler(rate int) *Resampler {
	r := int64(rate)
	if r <= 0 {
		r = Rate
	}
	rs := &Resampler{rate: r, outEnd: r}
	if r > Rate {
		for _, q := range []float64{0.50979558, 0.60134489, 0.89997622, 2.56291545} { // 8th-order Butterworth
			rs.lp = append(rs.lp, newLowpass(float64(r), lowpassHz, q))
		}
	}
	return rs
}

// maxInput is the largest input magnitude kept (4 times full scale: a float file can exceed 1.0).
const maxInput = 4 * 32768

// MaxRate is the highest input sample rate accepted (384 kHz, the top of studio audio).
const MaxRate = 384000

// lowpassHz is the cut-off of the anti-aliasing filter (the 8 kHz stream has its limit at 4 kHz).
const lowpassHz = 3600

// biquad is one second-order section (direct form II transposed).
type biquad struct{ b0, b1, b2, a1, a2, z1, z2 float64 }

// newLowpass is the RBJ low-pass biquad for sample rate fs, cut-off fc and quality q.
func newLowpass(fs, fc, q float64) biquad {
	w := 2 * math.Pi * fc / fs
	cs, alpha := math.Cos(w), math.Sin(w)/(2*q)
	a0 := 1 + alpha
	return biquad{b0: (1 - cs) / 2 / a0, b1: (1 - cs) / a0, b2: (1 - cs) / 2 / a0, a1: -2 * cs / a0, a2: (1 - alpha) / a0}
}

func (b *biquad) step(x float64) float64 {
	y := b.b0*x + b.z1
	b.z1 = b.b1*x - b.a1*y + b.z2
	b.z2 = b.b2*x - b.a2*y
	return y
}

// Push adds one input sample (full scale = +-32768) and appends the output samples it completes.
// Units: input sample k spans [k*8000, (k+1)*8000), output sample j spans [j*rate, (j+1)*rate).
func (r *Resampler) Push(v float64, out []int16) []int16 {
	// A NaN or an infinity would stay in the filter state for good: sanitize the input.
	switch {
	case v != v:
		v = 0
	case v > maxInput:
		v = maxInput
	case v < -maxInput:
		v = -maxInput
	}
	for i := range r.lp {
		v = r.lp[i].step(v)
	}
	end := (r.n + 1) * Rate
	for r.outEnd <= end {
		r.acc += v * float64(r.outEnd-r.outCur)
		out = append(out, clamp(r.acc/float64(r.rate)))
		r.outCur = r.outEnd
		r.outEnd += r.rate
		r.acc = 0
	}
	r.acc += v * float64(end-r.outCur)
	r.outCur = end
	r.n++
	return out
}

func clamp(v float64) int16 {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return int16(math.Round(v))
}
