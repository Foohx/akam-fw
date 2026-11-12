//go:build linux

package audio

import (
	"context"
	"errors"
	"io"
	"testing"
)

// fakePulse stands for a PulseAudio connection: the adapter is tested without any sound server.
type fakePulse struct {
	push      func([]int16) error
	stopped   int
	closed    int
	recordErr error
}

func (f *fakePulse) Record(push func([]int16) error) (func(), error) {
	if f.recordErr != nil {
		return nil, f.recordErr
	}
	f.push = push
	return func() { f.stopped++ }, nil
}
func (f *fakePulse) Close() { f.closed++ }

func TestPulseBackendAdapter(t *testing.T) {
	fp := &fakePulse{}
	b := &pulseBackend{connect: func() (pulseSession, error) { return fp, nil }}
	if b.Name() == "" || !b.Available() || fp.closed != 1 {
		t.Fatalf("available: closed %d", fp.closed)
	}
	r, err := b.Open(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	go func() { _ = fp.push([]int16{1, 2}) }()
	buf := make([]byte, 4)
	if _, err := io.ReadFull(r, buf); err != nil || buf[0] != 1 || buf[2] != 2 {
		t.Fatalf("%v %v", buf, err)
	}
	r.Close()
	if fp.stopped != 1 || fp.closed != 2 {
		t.Errorf("stopped %d closed %d: the recording and the connection must both end", fp.stopped, fp.closed)
	}
	if err := fp.push([]int16{3}); err == nil {
		t.Error("the callback still accepts data after Close")
	}
}

func TestPulseBackendUnreachable(t *testing.T) {
	b := &pulseBackend{connect: func() (pulseSession, error) { return nil, errors.New("no server") }}
	if b.Available() {
		t.Error("available without a server")
	}
	if _, err := b.Open(context.Background()); err == nil {
		t.Error("opened without a server")
	}
	fp := &fakePulse{recordErr: errors.New("no source")}
	b = &pulseBackend{connect: func() (pulseSession, error) { return fp, nil }}
	if _, err := b.Open(context.Background()); err == nil || fp.closed != 1 {
		t.Errorf("record failure: %v, connection closed %d times", err, fp.closed)
	}
}

func TestDefaultBackendIsNilInTestBuilds(t *testing.T) {
	// Run with -tags akamtest (the project's tests always do): nothing may reach the real sound server.
	if DefaultBackend() != nil {
		t.Skip("built without the akamtest tag: the real backend is selected")
	}
}
