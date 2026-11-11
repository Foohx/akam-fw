package ssh_test

import (
	"bytes"
	"context"
	"errors"
	"runtime"
	"testing"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

const readCmd = "dd if=/dev/mtdblock7 bs=64k"

type failAfter struct {
	n, max int
	err    error
}

func (w *failAfter) Write(p []byte) (int, error) {
	if w.n+len(p) > w.max {
		return 0, w.err
	}
	w.n += len(p)
	return len(p), nil
}

func TestRunStreamDeliversAll(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	var b bytes.Buffer
	res, err := c.RunStream(context.Background(), readCmd, &b)
	if err != nil || res.Status != 0 || !bytes.Equal(b.Bytes(), testfake.MtdData(7)) {
		t.Fatalf("stream: status=%d err=%v len=%d", res.Status, err, b.Len())
	}
	// a non-zero status is a status, not an error
	res, err = c.RunStream(context.Background(), "frobnicate", &b)
	if err != nil || res.Status != 127 {
		t.Fatalf("unknown command: %+v %v", res, err)
	}
}

// A failing writer aborts the session at once with the writer's error, and nothing is written after
// RunStream has returned.
func TestRunStreamWriterErrorAborts(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	boom := errors.New("over the cap")
	w := &failAfter{max: 100000, err: boom}
	start := time.Now()
	_, err = c.RunStream(context.Background(), readCmd, w)
	if !errors.Is(err, boom) {
		t.Fatalf("err = %v, want the writer's error", err)
	}
	if time.Since(start) > 10*time.Second {
		t.Error("the abort took too long")
	}
	n := w.n
	time.Sleep(100 * time.Millisecond)
	if w.n != n {
		t.Error("the writer was called after RunStream returned")
	}
	// the connection still works for the next command
	if res, err := c.Run(context.Background(), "true", nil); err != nil || res.Status != 0 {
		t.Errorf("after the abort: %+v %v", res, err)
	}
}

// Cancelling the context ends a stalled read and leaves no goroutine behind.
func TestRunStreamContextCancel(t *testing.T) {
	release := make(chan struct{})
	f := newFixtureWith(t, testfake.Options{MtdRead: func(n, read int, data []byte) ([]byte, bool) {
		<-release // the camera stalls before sending anything
		return nil, false
	}})
	defer close(release)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	before := runtime.NumGoroutine()
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	var b bytes.Buffer
	start := time.Now()
	_, err = c.RunStream(ctx, readCmd, &b)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("err = %v, want deadline exceeded", err)
	}
	if time.Since(start) > 8*time.Second {
		t.Error("the cancel took too long")
	}
	time.Sleep(200 * time.Millisecond)
	if after := runtime.NumGoroutine(); after > before+2 {
		t.Errorf("goroutines: %d before, %d after", before, after)
	}
}
