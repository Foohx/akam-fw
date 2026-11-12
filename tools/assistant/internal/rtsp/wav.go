package rtsp

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/Foohx/akam-fw/tools/assistant/internal/audio"
)

// ErrNotWAV means the stream does not start with a RIFF/WAVE header.
var ErrNotWAV = errors.New("not a WAV file")

// maxFmtChunk bounds the "fmt " chunk: a real one is 16, 18 or 40 bytes; the size in the header is
// never trusted for an allocation.
const maxFmtChunk = 256

// ReadWAV decodes a PCM WAV (8, 16, 24 or 32 bit integer, or 32 bit float; any channel count and
// sample rate up to audio.MaxRate) into 8 kHz mono signed 16-bit samples: channels are averaged and
// the rate goes through audio.Resampler (low-pass, then box average). The data is streamed frame by
// frame: sizes in the header are only limits, never allocation sizes, so a header claiming more
// than the file holds just ends at the end of the file. At most audio.MaxSeconds of audio are
// decoded. It never panics on bad input.
func ReadWAV(r io.Reader) (pcm []int16, err error) {
	defer func() {
		if x := recover(); x != nil {
			pcm, err = nil, fmt.Errorf("damaged WAV data (%v)", x)
		}
	}()
	br := bufio.NewReader(r)
	var hdr [12]byte
	if _, err := io.ReadFull(br, hdr[:]); err != nil || string(hdr[:4]) != "RIFF" || string(hdr[8:12]) != "WAVE" {
		return nil, ErrNotWAV
	}
	var (
		format, channels, bits uint16
		rate                   uint32
		haveFmt                bool
		dataSize               int64
	)
	bad := errors.New("unreadable WAV header")
find:
	for {
		var ch [8]byte
		if _, err := io.ReadFull(br, ch[:]); err != nil {
			return nil, errors.New("unreadable WAV header: no audio data")
		}
		size := int64(binary.LittleEndian.Uint32(ch[4:8])) // int64: size+size&1 cannot wrap
		switch string(ch[:4]) {
		case "fmt ":
			if size < 16 || size > maxFmtChunk {
				return nil, bad
			}
			buf := make([]byte, size+size&1)
			if _, err := io.ReadFull(br, buf); err != nil {
				return nil, bad
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
				return nil, bad
			}
			dataSize = size
			break find
		default:
			if _, err := io.CopyN(io.Discard, br, size+size&1); err != nil {
				return nil, bad
			}
		}
	}
	if channels == 0 || rate == 0 {
		return nil, bad
	}
	if rate > audio.MaxRate {
		return nil, fmt.Errorf("the sample rate (%d Hz) is not supported", rate)
	}
	bps := int(bits) / 8
	switch {
	case format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32):
	case format == 3 && bits == 32:
	default:
		return nil, errors.New("this WAV format is not supported (use PCM 8, 16, 24 or 32 bit, or 32 bit float)")
	}
	var src io.Reader = br
	if dataSize != 0 && dataSize != 0xFFFFFFFF { // 0 or all ones: streamed WAV, until the end
		src = io.LimitReader(br, dataSize)
	}
	frame := bps * int(channels)
	maxFrames := int64(audio.MaxSeconds) * int64(rate)
	rs := audio.NewResampler(int(rate))
	buf := make([]byte, frame)
	var out []int16
	for n := int64(0); ; n++ {
		if _, err := io.ReadFull(src, buf); err != nil {
			break // end of data (a partial last frame is dropped)
		}
		if n >= maxFrames {
			return nil, fmt.Errorf("the audio is longer than %d minutes", audio.MaxSeconds/60)
		}
		var sum float64
		for c := 0; c < int(channels); c++ {
			o := c * bps
			var v float64
			switch {
			case format == 3:
				v = float64(math.Float32frombits(binary.LittleEndian.Uint32(buf[o:]))) * 32768
			case bits == 8:
				v = (float64(buf[o]) - 128) * 256
			case bits == 16:
				v = float64(int16(binary.LittleEndian.Uint16(buf[o:])))
			case bits == 24:
				v = float64(int32(uint32(buf[o])<<8|uint32(buf[o+1])<<16|uint32(buf[o+2])<<24)>>8) / 256
			default:
				v = float64(int32(binary.LittleEndian.Uint32(buf[o:]))) / 65536
			}
			sum += v
		}
		out = rs.Push(sum/float64(channels), out)
	}
	return out, nil
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
