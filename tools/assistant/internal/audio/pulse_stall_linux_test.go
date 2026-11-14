//go:build linux

package audio

import (
	"context"
	"errors"
	"io"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func fastWatchdog(t *testing.T, every, stall time.Duration) {
	oldEvery, oldStall := pulseWatchEvery, pulseStallAfter
	pulseWatchEvery, pulseStallAfter = every, stall
	t.Cleanup(func() { pulseWatchEvery, pulseStallAfter = oldEvery, oldStall })
}

// A server that stays silent without closing anything (killed, suspended) ends the stream after
// the stall delay instead of leaving the reader blocked.
func TestPulseWatchdogEndsSilentStream(t *testing.T) {
	fastWatchdog(t, 5*time.Millisecond, 60*time.Millisecond)
	fp := &fakePulse{}
	b := &pulseBackend{connect: func() (pulseSession, error) { return fp, nil }}
	r, err := b.Open(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	done := make(chan error, 1)
	go func() { _, err := r.Read(make([]byte, 4)); done <- err }()
	select {
	case err := <-done:
		if err == nil || !strings.Contains(err.Error(), "lost the connection") {
			t.Errorf("read error: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("a silent server never ended the stream")
	}
}

// A stream that keeps delivering is not cut, and neither is one whose chunk waits for a slow reader.
func TestPulseWatchdogLeavesHealthyAndBackpressuredStreamsAlone(t *testing.T) {
	fastWatchdog(t, 5*time.Millisecond, 60*time.Millisecond)
	fp := &fakePulse{}
	b := &pulseBackend{connect: func() (pulseSession, error) { return fp, nil }}
	r, err := b.Open(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	// a chunk is pushed now and nobody reads for longer than the stall delay
	pushed := make(chan error, 1)
	go func() { pushed <- fp.push([]int16{7, 8}) }()
	time.Sleep(200 * time.Millisecond)
	buf := make([]byte, 4)
	if _, err := io.ReadFull(r, buf); err != nil || buf[0] != 7 {
		t.Fatalf("a slow reader got %v %v", buf, err)
	}
	if err := <-pushed; err != nil {
		t.Fatal(err)
	}
	// then a steady feed for several stall delays
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		for {
			select {
			case <-stop:
				return
			case <-time.After(10 * time.Millisecond):
				if fp.push([]int16{1}) != nil {
					return
				}
			}
		}
	}()
	end := time.Now().Add(250 * time.Millisecond)
	for time.Now().Before(end) {
		if _, err := io.ReadFull(r, buf[:2]); err != nil {
			t.Fatalf("a healthy stream was cut: %v", err)
		}
	}
}

// The connection attempt has a deadline; a session that comes late is closed, not leaked.
func TestConnectWithinDeadline(t *testing.T) {
	release := make(chan struct{})
	late := &countingPulse{}
	fp := &fakePulse{}
	start := time.Now()
	_, err := connectWithin(50*time.Millisecond, func() (pulseSession, error) { <-release; return late, nil })
	if err == nil || time.Since(start) > 2*time.Second {
		t.Fatalf("no timeout: %v after %v", err, time.Since(start))
	}
	close(release)
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if late.closed.Load() == 1 {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if late.closed.Load() != 1 {
		t.Error("the late session was not closed")
	}
	if s, err := connectWithin(time.Second, func() (pulseSession, error) { return fp, nil }); err != nil || s != fp {
		t.Errorf("a quick connection failed: %v", err)
	}
	var calls atomic.Int32
	if _, err := connectWithin(time.Second, func() (pulseSession, error) { calls.Add(1); return nil, errors.New("refused") }); err == nil || calls.Load() != 1 {
		t.Errorf("error not passed on: %v", err)
	}
}

type countingPulse struct {
	fakePulse
	closed atomic.Int32
}

func (c *countingPulse) Close() { c.closed.Add(1) }
