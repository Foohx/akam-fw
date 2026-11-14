//go:build linux

package audio

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync/atomic"
	"time"

	"github.com/jfreymuth/pulse"
)

// pulseSession is the part of a PulseAudio connection the microphone needs (a fake in tests).
type pulseSession interface {
	// Record starts recording the default source as 8 kHz mono s16 (the server converts) and calls
	// push for every chunk; stop ends it; lost reports that the server went away (or the stream
	// died) while recording.
	Record(push func([]int16) error) (stop func(), lost func() bool, err error)
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
	// last is the time of the last chunk (or of the start), busy is true while a chunk waits for a
	// slow reader (no data is expected from the server then).
	var last atomic.Int64
	var busy atomic.Bool
	last.Store(time.Now().UnixNano())
	push := func(b []int16) error {
		busy.Store(true)
		err := ps.Push(b)
		busy.Store(false)
		last.Store(time.Now().UnixNano())
		return err
	}
	var lost func() bool
	stopRec, lost, err = s.Record(push)
	if err != nil {
		s.Close()
		return nil, fmt.Errorf("cannot record from the default source: %w", err)
	}
	ps.watchContext(ctx)
	// A server that vanishes mid-talk would leave a reader blocked for ever: poll the stream and
	// end the pipe with a clear error.
	every, stallAfter := pulseWatchEvery, pulseStallAfter // read once: tests change them
	go func() {
		t := time.NewTicker(every)
		defer t.Stop()
		for {
			select {
			case <-ps.done:
				return
			case <-t.C:
				stalled := !busy.Load() && time.Since(time.Unix(0, last.Load())) > stallAfter
				if lost() || stalled {
					ps.Fail(errors.New("lost the connection to the PulseAudio / PipeWire server"))
					return
				}
			}
		}
	}()
	return ps, nil
}

// pulseWatchEvery is how often the watchdog looks at the stream (a variable for tests).
var pulseWatchEvery = 500 * time.Millisecond

// pulseStallAfter is how long a recording may deliver nothing before it is declared lost (a
// server that dies without closing the socket never gives an end of stream). A running source
// delivers a chunk every 100 ms.
var pulseStallAfter = 3 * time.Second

// pulseConnectTimeout bounds the connection to the server: the library has no dial timeout, so
// a PULSE_SERVER that does not answer (tcp) would block for minutes.
var pulseConnectTimeout = 5 * time.Second

// connectWithin runs connect and gives up after d: a late success is closed in the background.
func connectWithin(d time.Duration, connect func() (pulseSession, error)) (pulseSession, error) {
	type result struct {
		s   pulseSession
		err error
	}
	ch := make(chan result, 1)
	go func() {
		s, err := connect()
		ch <- result{s, err}
	}()
	select {
	case r := <-ch:
		return r.s, r.err
	case <-time.After(d):
		go func() {
			if r := <-ch; r.err == nil && r.s != nil {
				r.s.Close()
			}
		}()
		return nil, errors.New("the sound server did not answer in time")
	}
}

type realPulse struct{ c *pulse.Client }

func connectPulse() (pulseSession, error) {
	return connectWithin(pulseConnectTimeout, dialPulse)
}

func dialPulse() (pulseSession, error) {
	c, err := pulse.NewClient(pulse.ClientApplicationName("akam"), pulse.ClientTimeout(3*time.Second))
	if err != nil {
		return nil, err
	}
	return &realPulse{c: c}, nil
}

func (r *realPulse) Record(push func([]int16) error) (func(), func() bool, error) {
	var dead atomic.Bool // our own state: rs.Closed() is read by another goroutine than the one that writes it
	w := pulse.Int16Writer(func(s []int16) (int, error) {
		if err := push(s); err != nil {
			dead.Store(true)
			return 0, err
		}
		return len(s), nil
	})
	// Default source (the microphone), mono, 8 kHz: the server resamples and downmixes. A short
	// latency keeps the voice close to real time.
	rs, err := r.c.NewRecord(w, pulse.RecordSampleRate(Rate), pulse.RecordLatency(0.1))
	if err != nil {
		return nil, nil, err
	}
	if rs.SampleRate() != Rate || rs.Channels() != 1 {
		rs.Close()
		return nil, nil, errors.New("the sound server did not give 8 kHz mono")
	}
	rs.Start()
	return func() { dead.Store(true); rs.Stop(); rs.Close() }, dead.Load, nil
}

func (r *realPulse) Close() { r.c.Close() }

// defaultBackend is the built-in microphone of Linux: PulseAudio / PipeWire.
func defaultBackend() MicBackend { return newPulseBackend() }
