package rtsp

import (
	"bufio"
	"encoding/binary"
	"errors"
	"io"
	"math"
)

// ErrNotWAV means the stream does not start with a RIFF/WAVE header.
var ErrNotWAV = errors.New("not a WAV file")

// ReadWAV decodes a PCM WAV (8, 16, 24 or 32 bit integer, or 32 bit float; any channel count and
// sample rate) into 8 kHz mono signed 16-bit samples: channels are averaged, the rate is
// converted by linear interpolation (with a box average when going down, against aliasing).
func ReadWAV(r io.Reader) ([]int16, error) {
	br := bufio.NewReader(r)
	var hdr [12]byte
	if _, err := io.ReadFull(br, hdr[:]); err != nil || string(hdr[:4]) != "RIFF" || string(hdr[8:12]) != "WAVE" {
		return nil, ErrNotWAV
	}
	var (
		format, channels, bits uint16
		rate                   uint32
		haveFmt                bool
		data                   []byte
	)
	for {
		var ch [8]byte
		if _, err := io.ReadFull(br, ch[:]); err != nil {
			break
		}
		size := binary.LittleEndian.Uint32(ch[4:8])
		switch string(ch[:4]) {
		case "fmt ":
			if size < 16 {
				return nil, errors.New("unreadable WAV header")
			}
			buf := make([]byte, size+size&1)
			if _, err := io.ReadFull(br, buf); err != nil {
				return nil, errors.New("unreadable WAV header")
			}
			format = binary.LittleEndian.Uint16(buf[0:2])
			channels = binary.LittleEndian.Uint16(buf[2:4])
			rate = binary.LittleEndian.Uint32(buf[4:8])
			bits = binary.LittleEndian.Uint16(buf[14:16])
			if format == 0xFFFE && size >= 26 { // WAVE_FORMAT_EXTENSIBLE: the sub-format holds the real tag
				format = binary.LittleEndian.Uint16(buf[24:26])
			}
			haveFmt = true
		case "data":
			if !haveFmt {
				return nil, errors.New("unreadable WAV header")
			}
			if size == 0 || size == 0xFFFFFFFF {
				b, _ := io.ReadAll(br) // streamed WAV: until the end
				data = b
			} else {
				data = make([]byte, size)
				n, _ := io.ReadFull(br, data)
				data = data[:n]
			}
			goto decode
		default:
			if _, err := io.CopyN(io.Discard, br, int64(size)+int64(size&1)); err != nil {
				return nil, errors.New("unreadable WAV header")
			}
		}
	}
	return nil, errors.New("unreadable WAV header: no audio data")
decode:
	if channels == 0 || rate == 0 {
		return nil, errors.New("unreadable WAV header")
	}
	bps := int(bits) / 8
	switch {
	case format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32):
	case format == 3 && bits == 32:
	default:
		return nil, errors.New("this WAV format is not supported (use PCM 8, 16, 24 or 32 bit, or 32 bit float)")
	}
	frame := bps * int(channels)
	n := len(data) / frame
	mono := make([]float64, n)
	for i := 0; i < n; i++ {
		var sum float64
		for c := 0; c < int(channels); c++ {
			o := i*frame + c*bps
			var v float64
			switch {
			case format == 3:
				v = float64(math.Float32frombits(binary.LittleEndian.Uint32(data[o:]))) * 32768
			case bits == 8:
				v = (float64(data[o]) - 128) * 256
			case bits == 16:
				v = float64(int16(binary.LittleEndian.Uint16(data[o:])))
			case bits == 24:
				v = float64(int32(uint32(data[o])<<8|uint32(data[o+1])<<16|uint32(data[o+2])<<24)>>8) / 256
			default:
				v = float64(int32(binary.LittleEndian.Uint32(data[o:]))) / 65536
			}
			sum += v
		}
		mono[i] = sum / float64(channels)
	}
	return resample8k(mono, int(rate)), nil
}

func clamp16(v float64) int16 {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return int16(math.Round(v))
}

// resample8k converts mono samples at rate to 8000 Hz.
func resample8k(in []float64, rate int) []int16 {
	if rate == 8000 {
		out := make([]int16, len(in))
		for i, v := range in {
			out[i] = clamp16(v)
		}
		return out
	}
	ratio := float64(rate) / 8000
	n := int(float64(len(in)) / ratio)
	out := make([]int16, n)
	for i := 0; i < n; i++ {
		pos := float64(i) * ratio
		if ratio > 1 { // going down: average the source samples of this output sample
			lo, hi := int(pos), int(pos+ratio)
			if hi > len(in) {
				hi = len(in)
			}
			if hi <= lo {
				hi = lo + 1
			}
			var s float64
			for k := lo; k < hi && k < len(in); k++ {
				s += in[k]
			}
			out[i] = clamp16(s / float64(hi-lo))
			continue
		}
		j := int(pos)
		f := pos - float64(j)
		v := in[j]
		if j+1 < len(in) {
			v = in[j]*(1-f) + in[j+1]*f
		}
		out[i] = clamp16(v)
	}
	return out
}

// Tone returns seconds of a 1 kHz tone at 8 kHz, -16 dBFS, like the camera's own beep.
func Tone(seconds float64) []int16 {
	k := [8]int16{0, 3673, 5194, 3673, 0, -3673, -5194, -3673}
	n := int(seconds * 8000)
	out := make([]int16, n)
	for i := range out {
		out[i] = k[i&7]
	}
	return out
}
