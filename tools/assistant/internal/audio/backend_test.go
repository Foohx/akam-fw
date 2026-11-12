package audio

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"testing"
	"time"
)

// fakeBackend is a microphone that never touches a sound system.
type fakeBackend struct {
	name      string
	available bool
	opened    int
	openErr   error
	data      []byte
	closed    chan struct{}
}

func (f *fakeBackend) Name() string    { return f.name }
func (f *fakeBackend) Available() bool { return f.available }
func (f *fakeBackend) Open(ctx context.Context) (io.ReadCloser, error) {
	f.opened++
	if f.openErr != nil {
		return nil, f.openErr
	}
	return &fakeStream{f: f, r: bytesReader(f.data)}, nil
}

type fakeStream struct {
	f *fakeBackend
	r io.Reader
}

func (s *fakeStream) Read(b []byte) (int, error) { return s.r.Read(b) }
func (s *fakeStream) Close() error {
	if s.f.closed != nil {
		close(s.f.closed)
	}
	return nil
}

func bytesReader(b []byte) io.Reader { return &sliceReader{b: b} }

type sliceReader struct{ b []byte }

func (r *sliceReader) Read(p []byte) (int, error) {
	if len(r.b) == 0 {
		return 0, io.EOF
	}
	n := copy(p, r.b)
	r.b = r.b[n:]
	return n, nil
}

func TestChooseOrder(t *testing.T) {
	both := finder("ffmpeg", "arecord", "pactl")
	b := &fakeBackend{name: "Fake Audio", available: true}
	// 1. the explicit override beats everything
	c, err := (&Exec{Shell: "cat x", Builtin: b, LookPath: both, GOOS: "linux"}).Choose(0)
	if err != nil || c.Name != "AKAM_TALK_CAPTURE_CMD" || c.Builtin != nil || b.opened != 0 {
		t.Fatalf("override: %+v %v", c, err)
	}
	// 2. the built-in backend beats ffmpeg and arecord
	c, err = (&Exec{Builtin: b, LookPath: both, GOOS: "linux"}).Choose(0)
	if err != nil || c.Builtin != b || c.Name != "Fake Audio" {
		t.Fatalf("builtin: %+v %v", c, err)
	}
	// 3. built-in unreachable: arecord, or ffmpeg alone
	b.available = false
	c, err = (&Exec{Builtin: b, LookPath: finder("arecord", "ffmpeg"), GOOS: "linux"}).Choose(0)
	if err != nil || c.Name != "arecord" || c.Builtin != nil {
		t.Fatalf("fallback: %+v %v", c, err)
	}
	c, err = (&Exec{Builtin: b, LookPath: finder("ffmpeg"), GOOS: "windows"}).Choose(0)
	if err != nil || c.Name != "ffmpeg" {
		t.Fatalf("windows fallback: %+v %v", c, err)
	}
	// 4. nothing at all; no built-in backend (macOS) with and without ffmpeg
	if _, err = (&Exec{Builtin: b, LookPath: finder(), GOOS: "linux"}).Choose(0); !errors.Is(err, ErrNoTool) {
		t.Errorf("nothing: %v", err)
	}
	if _, err = (&Exec{LookPath: finder(), GOOS: "darwin"}).Choose(0); !errors.Is(err, ErrNoTool) {
		t.Errorf("darwin without ffmpeg: %v", err)
	}
	if c, err = (&Exec{LookPath: finder("ffmpeg"), GOOS: "darwin"}).Choose(0); err != nil || c.Name != "ffmpeg" {
		t.Errorf("darwin: %+v %v", c, err)
	}
}

func TestMicUsesBuiltinAndTimeLimit(t *testing.T) {
	b := &fakeBackend{name: "Fake", available: true, data: make([]byte, 5*Rate*2), closed: make(chan struct{})}
	r, err := (&Exec{Builtin: b, LookPath: finder(), GOOS: "linux"}).Mic(context.Background(), 2)
	if err != nil {
		t.Fatal(err)
	}
	all, _ := io.ReadAll(r)
	if len(all) != 2*Rate*2 {
		t.Errorf("%d bytes, want exactly 2 seconds (%d)", len(all), 2*Rate*2)
	}
	r.Close()
	select {
	case <-b.closed:
	default:
		t.Error("the backend stream was not closed")
	}
}

func TestPipeStream(t *testing.T) {
	stopped := 0
	ps := newPipeStream(func() { stopped++ })
	go func() {
		_ = ps.Push([]int16{1, -2, 300})
		ps.Fail(io.EOF)
	}()
	b := make([]byte, 6)
	if _, err := io.ReadFull(ps, b); err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint16(b[2:]) != 0xFFFE || b[4] != 0x2C || b[5] != 0x01 {
		t.Errorf("little endian s16: %v", b)
	}
	if _, err := ps.Read(b); err != io.EOF {
		t.Errorf("after Fail: %v", err)
	}
	ps.Close()
	ps.Close()
	if stopped != 1 {
		t.Errorf("stop called %d times", stopped)
	}
	if err := ps.Push([]int16{1}); err == nil {
		t.Error("Push after Close succeeded")
	}
}

func TestPipeStreamCloseUnblocksProducerAndReader(t *testing.T) {
	ps := newPipeStream(nil)
	pushed := make(chan error, 1)
	go func() { pushed <- ps.Push(make([]int16, 100)) }() // nobody reads: blocks
	time.Sleep(20 * time.Millisecond)
	ps.Close()
	select {
	case err := <-pushed:
		if err == nil {
			t.Error("blocked Push returned nil after Close")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Close did not unblock the producer")
	}
	ctx, cancel := context.WithCancel(context.Background())
	ps2 := newPipeStream(nil)
	ps2.watchContext(ctx)
	read := make(chan error, 1)
	go func() { _, err := ps2.Read(make([]byte, 4)); read <- err }()
	cancel()
	select {
	case err := <-read:
		if err == nil {
			t.Error("read returned nil after the context ended")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("the end of the context did not unblock the reader")
	}
}

func f32le(v float32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, math.Float32bits(v))
	return b
}

func TestPCMConverter(t *testing.T) {
	// float stereo: left 0.5, right -0.25 -> mean 0.125 * 32768 = 4096
	c := NewPCMConverter(8000, 2, 32, true)
	var in []byte
	for i := 0; i < 4; i++ {
		in = append(in, f32le(0.5)...)
		in = append(in, f32le(-0.25)...)
	}
	out := c.Convert(in, false, nil)
	if len(out) != 4 || out[0] != 4096 || out[3] != 4096 {
		t.Errorf("float: %v", out)
	}
	// int16 mono at 8 kHz is copied; a partial frame is ignored
	c = NewPCMConverter(8000, 1, 16, false)
	out = c.Convert([]byte{0x01, 0x00, 0xFF, 0xFF, 0x7F}, false, nil)
	if len(out) != 2 || out[0] != 1 || out[1] != -1 {
		t.Errorf("int16: %v", out)
	}
	// 24-bit: 0x400000 is a half of full scale (16384 in 16 bit)
	c = NewPCMConverter(8000, 1, 24, false)
	if out = c.Convert([]byte{0, 0, 0x40}, false, nil); len(out) != 1 || out[0] != 16384 {
		t.Errorf("int24: %v", out)
	}
	// 32-bit integer, and a silent buffer is zeros whatever it holds
	c = NewPCMConverter(8000, 1, 32, false)
	if out = c.Convert([]byte{0, 0, 0, 0x40}, false, nil); len(out) != 1 || out[0] != 16384 {
		t.Errorf("int32: %v", out)
	}
	if out = c.Convert([]byte{1, 2, 3, 4}, true, nil); len(out) != 1 || out[0] != 0 {
		t.Errorf("silent: %v", out)
	}
	// 48 kHz stereo 16-bit: 6 frames -> 1 sample (the value is filtered, only the count is checked)
	c = NewPCMConverter(48000, 2, 16, false)
	var s []byte
	for i := 0; i < 600; i++ {
		s = append(s, 0x10, 0x00, 0x30, 0x00)
	}
	if out = c.Convert(s, false, nil); len(out) != 100 {
		t.Errorf("48k: %d samples", len(out))
	}
}

// A reachable server that cannot record is an error when there is no other tool (the ffmpeg and
// arecord fallback itself starts a process, so it is not run here).
func TestMicBuiltinOpenFailureWithoutFallbackIsAnError(t *testing.T) {
	b := &fakeBackend{name: "Fake", available: true, openErr: errors.New("no source")}
	_, err := (&Exec{Builtin: b, LookPath: finder(), GOOS: "linux"}).Mic(context.Background(), 0)
	if err == nil || err.Error() != "no source" || b.opened != 1 {
		t.Errorf("%v (opened %d)", err, b.opened)
	}
}
