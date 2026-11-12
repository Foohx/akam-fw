//go:build akamtest && unix

package cli

import (
	"os"
	"os/signal"
	"syscall"
	"testing"
	"time"
)

// L5: ignoreSignals must not undo the handler of the full-screen session (bubbletea listens with
// signal.Notify): after the write, a signal still reaches the earlier subscriber.
func TestIgnoreSignalsKeepsOtherHandlers(t *testing.T) {
	session := make(chan os.Signal, 4)
	signal.Notify(session, syscall.SIGHUP)
	defer signal.Stop(session)

	stop := ignoreSignals()
	if err := syscall.Kill(os.Getpid(), syscall.SIGHUP); err != nil { // harmless: it is caught
		t.Fatal(err)
	}
	select {
	case <-session:
	case <-time.After(2 * time.Second):
		t.Fatal("the session's handler lost the signal during the write")
	}
	stop()
	if err := syscall.Kill(os.Getpid(), syscall.SIGHUP); err != nil {
		t.Fatal(err)
	}
	select {
	case <-session:
	case <-time.After(2 * time.Second):
		t.Fatal("the session's handler was removed by the end of the write")
	}
}
