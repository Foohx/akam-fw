//go:build linux

package audio

import (
	"context"
	"io"
	"os/exec"
	"syscall"
	"testing"
	"time"
)

// gone reports whether no process of the group (and not the leader) exists any more.
func gone(pid int) bool {
	return syscall.Kill(-pid, 0) == syscall.ESRCH && syscall.Kill(pid, 0) == syscall.ESRCH
}

// A capture is stopped by Close and by the context, with its whole process group. The processes
// here are plain sh and sleep: this checks the cleanup, not any audio tool.
func TestProcessIsStoppedOnEveryExit(t *testing.T) {
	if _, err := exec.LookPath("sh"); err != nil {
		t.Skip("skip: no sh")
	}
	script := "sleep 60 & echo ready; wait" // a grandchild that would outlive its parent
	for name, stop := range map[string]func(*Proc, context.CancelFunc){
		"Close":  func(p *Proc, _ context.CancelFunc) { p.Close() },
		"cancel": func(p *Proc, c context.CancelFunc) { c() },
	} {
		ctx, cancel := context.WithCancel(context.Background())
		p, err := Start(ctx, []string{"sh", "-c", script})
		if err != nil {
			t.Fatal(err)
		}
		pid := p.cmd.Process.Pid
		buf := make([]byte, 6)
		if _, err := io.ReadFull(p, buf); err != nil || string(buf) != "ready\n" {
			t.Fatalf("%s: %q %v", name, buf, err)
		}
		start := time.Now()
		stop(p, cancel)
		for !gone(pid) && time.Since(start) < 4*time.Second {
			time.Sleep(10 * time.Millisecond)
		}
		if !gone(pid) {
			syscall.Kill(-pid, syscall.SIGKILL)
			t.Fatalf("%s: the process group is still alive after %v", name, time.Since(start))
		}
		// the output pipe ends instead of blocking, and a second Close is harmless
		done := make(chan struct{})
		go func() { io.Copy(io.Discard, p); close(done) }()
		select {
		case <-done:
		case <-time.After(3 * time.Second):
			t.Errorf("%s: the output pipe never ended", name)
		}
		p.Close()
		cancel()
	}
}

// A process that ends by itself leaves nothing behind either, and its output is read in full.
func TestShortProcessKeepsItsOutput(t *testing.T) {
	p, err := Start(context.Background(), []string{"sh", "-c", "printf abcdef"})
	if err != nil {
		t.Skip("skip: no sh")
	}
	b, err := io.ReadAll(p)
	if err != nil || string(b) != "abcdef" {
		t.Errorf("%q %v", b, err)
	}
	pid := p.cmd.Process.Pid
	p.Close()
	if !gone(pid) {
		t.Error("a finished process was not reaped")
	}
}
