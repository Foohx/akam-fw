//go:build akamtest

package cli_test

import (
	"sync"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// lockProbeUI is an Operator that runs probe when the flow marks the write.
type lockProbeUI struct {
	*ui.Plain
	probe func(on bool)
}

func (lockProbeUI) OpPlan([]string)      {}
func (lockProbeUI) OpStart(int)          {}
func (lockProbeUI) OpDone(int, string)   {}
func (lockProbeUI) OpStatus(string)      {}
func (lockProbeUI) OpBytes(int64, int64) {}
func (u lockProbeUI) OpWriting(on bool)  { u.probe(on) }
func (lockProbeUI) OpEnd()               {}

// M1: the write lock goes on BEFORE the "YES" that starts the write leaves for the camera's flash
// script, so a Ctrl-C can never land between the confirmation and the lock.
func TestTelnetFlashLocksBeforeSendingYES(t *testing.T) {
	fw := newCardFlashWorld(t, testfake.StockOptions{}, nil)
	var mu sync.Mutex
	var confirmsWhenLocked []int
	h := fw.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, sec *state.Secrets, dry bool) ui.UI {
		return lockProbeUI{cli.NewPlainUI(o, st, log, sec, dry), func(on bool) {
			if !on {
				return
			}
			time.Sleep(200 * time.Millisecond) // a YES already sent would be read by the script meanwhile
			mu.Lock()
			confirmsWhenLocked = append(confirmsWhenLocked, len(fw.st.Confirms()))
			mu.Unlock()
		}}
	}
	r := runWith(fw.env, h, "y\ny\n", append(fw.base(), "flash")...)
	expectRC(t, r, 0)
	mu.Lock()
	defer mu.Unlock()
	if len(confirmsWhenLocked) != 1 || confirmsWhenLocked[0] != 0 {
		t.Errorf("the script had seen %v confirmations when the write lock went on; want [0]", confirmsWhenLocked)
	}
	if got := fw.st.Confirms(); len(got) != 1 || got[0] != "YES" {
		t.Errorf("the script saw %v", got)
	}
}

// stopUI is an Operator with a stop channel the test closes when a given item starts.
type stopUI struct {
	*ui.Plain
	stop   chan struct{}
	onItem int
	once   sync.Once
}

func (u *stopUI) OpPlan([]string)           {}
func (u *stopUI) OpDone(int, string)        {}
func (u *stopUI) OpStatus(string)           {}
func (u *stopUI) OpBytes(_, _ int64)        {}
func (u *stopUI) OpWriting(bool)            {}
func (u *stopUI) OpEnd()                    {}
func (u *stopUI) StopChan() <-chan struct{} { return u.stop }
func (u *stopUI) OpStart(i int) {
	if i == u.onItem {
		u.once.Do(func() { close(u.stop) })
	}
}

// M3: a stop reaches the read-only phases. An SSH backup stopped in the middle ends at once, says it
// was stopped (not that the connection was lost), keeps nothing and never reads the other partitions.
func TestStopCancelsTheSSHBackup(t *testing.T) {
	e, root := sshBackupWorld(t)
	su := &stopUI{stop: make(chan struct{}), onItem: 4} // item 4 = the read of mtdblock2 (items 2 to 9 are the partitions)
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, sec *state.Secrets, dry bool) ui.UI {
		su.Plain = cli.NewPlainUI(o, st, log, sec, dry)
		return su
	}
	r := runWith(e, h, "", append(e.base(), "backup")...)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "Stopped. Nothing was changed on the camera and nothing was kept here.")
	notContains(t, r.out+r.err, "connection to the camera was lost", "no input")
	if len(sshBackupDirs(root)) != 0 {
		t.Errorf("a backup folder was kept: %v", sshBackupDirs(root))
	}
	if n := ddCommands(e); n > 5 {
		t.Errorf("%d partition reads: the stop did not end the backup", n)
	}
}
