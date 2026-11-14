package audio

import (
	"context"
	"sync"
	"testing"
)

// Close may be called by the caller and by the context watcher at the same time: no panic, and
// both return only once the process is gone.
func TestConcurrentCloseIsSafe(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	p, err := Start(ctx, []string{"sleep", "30"})
	if err != nil {
		t.Skip("no sleep command")
	}
	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); p.Close() }()
	}
	cancel()
	wg.Wait()
	if p.cmd.ProcessState == nil {
		t.Error("the process was not waited for")
	}
}
