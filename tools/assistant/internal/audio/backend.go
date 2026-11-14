package audio

import (
	"context"
	"encoding/binary"
	"io"
	"math"
	"sync"
)

// MicBackend is a built-in microphone: no external program, the audio comes from the sound system
// of the OS through a native protocol (PulseAudio / PipeWire on Linux, WASAPI on Windows).
type MicBackend interface {
	// Name is shown by `akam doctor` and in messages, e.g. "PulseAudio / PipeWire".
	Name() string
	// Available reports whether a sound server can be reached now (it opens nothing that stays open).
	Available() bool
	// Open starts the default microphone; the stream is raw 8 kHz mono s16le and ends on Close.
	Open(ctx context.Context) (io.ReadCloser, error)
}

// pipeStream turns a push source (a callback fed by a sound system thread) into an io.ReadCloser.
// The producer calls Push; Close (or the end of ctx) stops the producer through stop, then
// unblocks any reader. Safe to close more than once.
type pipeStream struct {
	pr   *io.PipeReader
	pw   *io.PipeWriter
	stop func()
	done chan struct{}
	once sync.Once
}

func newPipeStream(stop func()) *pipeStream {
	pr, pw := io.Pipe()
	return &pipeStream{pr: pr, pw: pw, stop: stop, done: make(chan struct{})}
}

// Push writes s16 samples as little-endian bytes; it blocks while the reader is slow and returns
// an error once the stream is closed.
func (p *pipeStream) Push(s []int16) error {
	b := make([]byte, 2*len(s))
	for i, v := range s {
		binary.LittleEndian.PutUint16(b[2*i:], uint16(v))
	}
	_, err := p.pw.Write(b)
	return err
}

func (p *pipeStream) Read(b []byte) (int, error) { return p.pr.Read(b) }

// Fail ends the stream with err (the reader sees it after the data already pushed).
func (p *pipeStream) Fail(err error) { _ = p.pw.CloseWithError(err) }

func (p *pipeStream) Close() error {
	p.once.Do(func() {
		close(p.done)
		// first unblock a producer stuck in Push, then stop it
		_ = p.pw.CloseWithError(io.ErrClosedPipe)
		_ = p.pr.Close()
		if p.stop != nil {
			p.stop()
		}
	})
	return nil
}

// watchContext closes the stream when ctx ends; the goroutine ends with the stream.
func (p *pipeStream) watchContext(ctx context.Context) {
	go func() {
		select {
		case <-ctx.Done():
			_ = p.Close()
		case <-p.done:
		}
	}()
}

// limited ends a stream after n bytes (a time limit of talk N); closing it closes the source.
type limited struct {
	r io.ReadCloser
	n int64
}

func (l *limited) Read(b []byte) (int, error) {
	if l.n <= 0 {
		return 0, io.EOF
	}
	if int64(len(b)) > l.n {
		b = b[:l.n]
	}
	n, err := l.r.Read(b)
	l.n -= int64(n)
	return n, err
}

func (l *limited) Close() error { return l.r.Close() }

// LimitSeconds wraps a raw 8 kHz mono s16le stream so that it ends after seconds (0 = no limit).
func LimitSeconds(r io.ReadCloser, seconds int) io.ReadCloser {
	if seconds <= 0 {
		return r
	}
	return &limited{r: r, n: int64(seconds) * Rate * 2}
}

// PCMConverter turns interleaved capture frames of any layout into 8 kHz mono s16 samples.
type PCMConverter struct {
	Channels int
	Float    bool // 32-bit IEEE float samples; else signed integers of Bits bits
	Bits     int  // 16, 24 or 32 for integers
	rs       *Resampler
}

// NewPCMConverter returns a converter for frames at rate Hz.
func NewPCMConverter(rate, channels, bits int, float bool) *PCMConverter {
	return &PCMConverter{Channels: channels, Float: float, Bits: bits, rs: NewResampler(rate)}
}

// BytesPerFrame is the size of one interleaved frame.
func (c *PCMConverter) BytesPerFrame() int { return c.Channels * c.Bits / 8 }

// Convert appends the 8 kHz mono samples of data (a whole number of frames; a partial frame at
// the end is ignored). silent true counts the frames as digital silence whatever data holds.
func (c *PCMConverter) Convert(data []byte, silent bool, out []int16) []int16 {
	bpf := c.BytesPerFrame()
	if bpf <= 0 || c.Channels <= 0 {
		return out
	}
	bps := c.Bits / 8
	for off := 0; off+bpf <= len(data); off += bpf {
		var sum float64
		if !silent {
			for ch := 0; ch < c.Channels; ch++ {
				p := data[off+ch*bps:]
				switch {
				case c.Float && c.Bits == 32:
					sum += float64(math.Float32frombits(binary.LittleEndian.Uint32(p))) * 32768
				case c.Bits == 16:
					sum += float64(int16(binary.LittleEndian.Uint16(p)))
				case c.Bits == 24:
					sum += float64(int32(uint32(p[0])<<8|uint32(p[1])<<16|uint32(p[2])<<24)>>8) / 256
				case c.Bits == 32:
					sum += float64(int32(binary.LittleEndian.Uint32(p))) / 65536
				}
			}
		}
		out = c.rs.Push(sum/float64(c.Channels), out)
	}
	return out
}
