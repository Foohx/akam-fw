package audio

import "math"

// Rate is the pipeline rate of talk and say: 8 kHz mono signed 16-bit.
const Rate = 8000

// Resampler converts a mono float stream at any rate to 8 kHz s16 samples, one input sample at a
// time (so a long file never has to be held at its native rate). Every input sample is a box of
// width 1/rate seconds and every output sample is the average of the boxes it overlaps, computed
// with exact integer positions (units of 1/(rate*8000) s): a box average going down (against
// aliasing) and a zero-order hold going up. At 8000 Hz it is the identity.
type Resampler struct {
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
	return &Resampler{rate: r, outEnd: r}
}

// Push adds one input sample (full scale = +-32768) and appends the output samples it completes.
// Units: input sample k spans [k*8000, (k+1)*8000), output sample j spans [j*rate, (j+1)*rate).
func (r *Resampler) Push(v float64, out []int16) []int16 {
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
