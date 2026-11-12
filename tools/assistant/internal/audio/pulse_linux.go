//go:build linux

package audio

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/jfreymuth/pulse"
)

// pulseSession is the part of a PulseAudio connection the microphone needs (a fake in tests).
type pulseSession interface {
	// Record starts recording the default source as 8 kHz mono s16 (the server converts) and calls
	// push for every chunk; stop ends it.
	Record(push func([]int16) error) (stop func(), err error)
	Close()
}

type pulseBackend struct {
	connect func() (pulseSession, error)
}

// newPulseBackend is the real backend: it talks to $PULSE_SERVER or the user's socket, which
// PipeWire (pipewire-pulse) serves as well.
func newPulseBackend() MicBackend { return &pulseBackend{connect: connectPulse} }

func (b *pulseBackend) Name() string { return "PulseAudio / PipeWire" }

func (b *pulseBackend) Available() bool {
	s, err := b.connect()
	if err != nil {
		return false
	}
	s.Close()
	return true
}

func (b *pulseBackend) Open(ctx context.Context) (io.ReadCloser, error) {
	s, err := b.connect()
	if err != nil {
		return nil, fmt.Errorf("no PulseAudio or PipeWire server: %w", err)
	}
	var stopRec func()
	ps := newPipeStream(func() {
		if stopRec != nil {
			stopRec()
		}
		s.Close()
	})
	stopRec, err = s.Record(ps.Push)
	if err != nil {
		s.Close()
		return nil, fmt.Errorf("cannot record from the default source: %w", err)
	}
	ps.watchContext(ctx)
	return ps, nil
}

type realPulse struct{ c *pulse.Client }

func connectPulse() (pulseSession, error) {
	c, err := pulse.NewClient(pulse.ClientApplicationName("akam"), pulse.ClientTimeout(3*time.Second))
	if err != nil {
		return nil, err
	}
	return &realPulse{c: c}, nil
}

func (r *realPulse) Record(push func([]int16) error) (func(), error) {
	w := pulse.Int16Writer(func(s []int16) (int, error) {
		if err := push(s); err != nil {
			return 0, err
		}
		return len(s), nil
	})
	// Default source (the microphone), mono, 8 kHz: the server resamples and downmixes. A short
	// latency keeps the voice close to real time.
	rs, err := r.c.NewRecord(w, pulse.RecordSampleRate(Rate), pulse.RecordLatency(0.1))
	if err != nil {
		return nil, err
	}
	if rs.SampleRate() != Rate || rs.Channels() != 1 {
		rs.Close()
		return nil, errors.New("the sound server did not give 8 kHz mono")
	}
	rs.Start()
	return func() { rs.Stop(); rs.Close() }, nil
}

func (r *realPulse) Close() { r.c.Close() }

// defaultBackend is the built-in microphone of Linux: PulseAudio / PipeWire.
func defaultBackend() MicBackend { return newPulseBackend() }
