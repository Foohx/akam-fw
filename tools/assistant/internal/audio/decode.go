package audio

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/hajimehoshi/go-mp3"
	"github.com/jfreymuth/oggvorbis"
	"github.com/mewkiz/flac"
)

// ErrUnsupported means the data is not in a format that is decoded in process (the caller may
// fall back to ffmpeg).
var ErrUnsupported = errors.New("not an MP3, FLAC or Ogg Vorbis file")

// MaxSeconds is the longest audio decoded in process (a 30 minute file is 28 MB of samples).
const MaxSeconds = 30 * 60

const maxSamples = MaxSeconds * Rate

// Format names the container of data by its magic bytes (never by a file name): "mp3", "flac",
// "ogg" or "" when it is none of them (WAV is detected by the caller).
func Format(data []byte) string {
	switch {
	case bytes.HasPrefix(data, []byte("fLaC")):
		return "flac"
	case bytes.HasPrefix(data, []byte("OggS")):
		return "ogg"
	case bytes.HasPrefix(data, []byte("ID3")):
		return "mp3"
	case len(data) >= 2 && data[0] == 0xFF && data[1]&0xE0 == 0xE0 && data[1]&0x06 != 0: // MPEG frame sync, layer bits not 00
		return "mp3"
	}
	return ""
}

// Decode decodes an MP3, FLAC or Ogg Vorbis file to 8 kHz mono signed 16-bit samples (channels
// averaged, rate converted). ErrUnsupported means another format; any other error means damaged
// or unsupported content of one of the three. It never panics on bad input.
func Decode(data []byte) (pcm []int16, err error) {
	defer func() {
		if r := recover(); r != nil {
			pcm, err = nil, fmt.Errorf("damaged audio data (%v)", r)
		}
	}()
	switch Format(data) {
	case "mp3":
		return decodeMP3(data)
	case "flac":
		return decodeFLAC(data)
	case "ogg":
		return decodeOgg(data)
	}
	return nil, ErrUnsupported
}

var errTooLong = fmt.Errorf("the audio is longer than %d minutes", MaxSeconds/60)

func decodeMP3(data []byte) ([]int16, error) {
	d, err := mp3.NewDecoder(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("unreadable MP3: %w", err)
	}
	rs := NewResampler(d.SampleRate())
	var out []int16
	buf := make([]byte, 4096*4) // stereo 16-bit frames, always
	for {
		n, rerr := d.Read(buf)
		for i := 0; i+4 <= n; i += 4 {
			l := int16(uint16(buf[i]) | uint16(buf[i+1])<<8)
			r := int16(uint16(buf[i+2]) | uint16(buf[i+3])<<8)
			out = rs.Push((float64(l)+float64(r))/2, out)
		}
		if len(out) > maxSamples {
			return nil, errTooLong
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			if len(out) > 0 { // a damaged tail: keep what was decoded
				break
			}
			return nil, fmt.Errorf("unreadable MP3: %w", rerr)
		}
	}
	if len(out) == 0 {
		return nil, errors.New("the MP3 holds no audio")
	}
	return out, nil
}

func decodeFLAC(data []byte) ([]int16, error) {
	st, err := flac.New(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("unreadable FLAC: %w", err)
	}
	info := st.Info
	if info.SampleRate == 0 || info.NChannels == 0 || info.BitsPerSample == 0 || info.BitsPerSample > 32 {
		return nil, errors.New("unreadable FLAC header")
	}
	scale := math.Ldexp(1, 16-int(info.BitsPerSample)) // to 16-bit full scale
	rs := NewResampler(int(info.SampleRate))
	var out []int16
	for {
		fr, ferr := st.ParseNext()
		if ferr == io.EOF {
			break
		}
		if ferr != nil {
			if len(out) > 0 {
				break
			}
			return nil, fmt.Errorf("unreadable FLAC: %w", ferr)
		}
		if len(fr.Subframes) == 0 {
			continue
		}
		n := len(fr.Subframes[0].Samples)
		for i := 0; i < n; i++ {
			var sum float64
			for _, sf := range fr.Subframes {
				if i < len(sf.Samples) {
					sum += float64(sf.Samples[i])
				}
			}
			out = rs.Push(sum/float64(len(fr.Subframes))*scale, out)
		}
		if len(out) > maxSamples {
			return nil, errTooLong
		}
	}
	if len(out) == 0 {
		return nil, errors.New("the FLAC holds no audio")
	}
	return out, nil
}

func decodeOgg(data []byte) ([]int16, error) {
	r, err := oggvorbis.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("unreadable Ogg file (only Vorbis audio is built in): %w", err)
	}
	ch := r.Channels()
	if ch <= 0 || r.SampleRate() <= 0 {
		return nil, errors.New("unreadable Ogg Vorbis header")
	}
	rs := NewResampler(r.SampleRate())
	var out []int16
	buf := make([]float32, 4096*ch)
	for {
		n, rerr := r.Read(buf)
		for i := 0; i+ch <= n; i += ch {
			var sum float64
			for c := 0; c < ch; c++ {
				sum += float64(buf[i+c])
			}
			out = rs.Push(sum/float64(ch)*32768, out)
		}
		if len(out) > maxSamples {
			return nil, errTooLong
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			if len(out) > 0 {
				break
			}
			return nil, fmt.Errorf("unreadable Ogg Vorbis: %w", rerr)
		}
	}
	if len(out) == 0 {
		return nil, errors.New("the Ogg file holds no audio")
	}
	return out, nil
}
