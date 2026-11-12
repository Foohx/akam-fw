//go:build akamtest

package cli_test

import (
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// opLog records what a flow tells the session about its long operation (ui.Operator, ui.Checklister).
type opLog struct {
	mu      sync.Mutex
	ev      []string
	stopAll bool // the checklist answers "the user stopped"
}

func (l *opLog) add(f string, a ...any) {
	l.mu.Lock()
	l.ev = append(l.ev, fmt.Sprintf(f, a...))
	l.mu.Unlock()
}

func (l *opLog) events() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.ev...)
}

func (l *opLog) index(prefix string, from int) int {
	for i, e := range l.events() {
		if i >= from && strings.HasPrefix(e, prefix) {
			return i
		}
	}
	return -1
}

type opUI struct {
	*ui.Plain
	l *opLog
}

func (u opUI) OpPlan(items []string)     { u.l.add("plan:%s", strings.Join(items, "|")) }
func (u opUI) OpStart(i int)             { u.l.add("start:%d", i) }
func (u opUI) OpDone(i int, note string) { u.l.add("done:%d:%s", i, note) }
func (u opUI) OpStatus(line string)      { u.l.add("status:%s", line) }
func (u opUI) OpBytes(done, total int64) { u.l.add("bytes:%d/%d", done, total) }
func (u opUI) OpWriting(on bool)         { u.l.add("writing:%v", on) }
func (u opUI) OpEnd()                    { u.l.add("end") }
func (u opUI) Checklist(t string, s []string) error {
	u.l.add("checklist:%s:%d", t, len(s))
	if u.l.stopAll {
		return ui.ErrNoInput
	}
	return nil
}

func (e *env) runOps(l *opLog, stdin string, argv ...string) result {
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		return opUI{cli.NewPlainUI(o, st, log, secrets, dry), l}
	}
	return runWith(e, h, stdin, append(e.base(), argv...)...)
}

// The SSH backup describes eight partitions read twice, reports bytes against twice the partition
// size, and never marks a write to the camera.
func TestBackupSSHOperationEvents(t *testing.T) {
	e, root := sshBackupWorld(t)
	l := &opLog{}
	r := e.runOps(l, "", "backup")
	expectRC(t, r, 0)
	if len(sshBackupDirs(root)) != 1 {
		t.Fatalf("backup folders: %v", sshBackupDirs(root))
	}
	ev := l.events()
	if len(ev) == 0 || !strings.HasPrefix(ev[0], "plan:Connect over SSH|Check the partition table|Read mtdblock0 twice|") || !strings.HasSuffix(ev[0], "|Save the backup on this PC") {
		t.Fatalf("plan: %v", ev)
	}
	if ev[len(ev)-1] != "end" {
		t.Errorf("the operation did not end: %v", ev[len(ev)-1])
	}
	next := 0
	for i := 0; i <= 10; i++ {
		j := l.index(fmt.Sprintf("start:%d", i), next)
		if j < 0 {
			t.Fatalf("item %d never started (or out of order): %v", i, ev)
		}
		next = j
	}
	for n := 0; n < 8; n++ {
		if l.index(fmt.Sprintf("done:%d:read twice, identical", 2+n), 0) < 0 {
			t.Errorf("mtdblock%d: no note", n)
		}
		size := int64(len(testfake.MtdData(n)))
		if size == 0 {
			continue
		}
		if l.index(fmt.Sprintf("bytes:%d/%d", 2*size, 2*size), 0) < 0 {
			t.Errorf("mtdblock%d: no final byte count of %d", n, 2*size)
		}
	}
	for _, x := range ev {
		if x == "writing:true" {
			t.Error("a backup is read only: no write may be announced")
		}
		if strings.HasPrefix(x, "status:dd if=/dev/mtdblock") && !strings.Contains(x, "read only") {
			t.Errorf("the command must say it is read only: %q", x)
		}
	}
	if l.index("status:dd if=/dev/mtdblock2 bs=64k", 0) < 0 {
		t.Error("the command that runs is not shown")
	}
}

// The update announces the write after the red box answer and withdraws it when the write ends; before the
// yes (no answer) it never announces one.
func TestUpdateOperationWriting(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	l := &opLog{}
	w.update("") // prepares the backup state; refused without a yes
	l = &opLog{}
	h := w.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		return opUI{cli.NewPlainUI(o, st, log, secrets, dry), l}
	}
	r := runWith(w.env, h, "y\n", append(w.base(), "update", "--image", w.img)...)
	expectRC(t, r, 0)
	ev := l.events()
	t.Log(ev)
	if len(ev) == 0 || !strings.HasPrefix(ev[0], "plan:Check the camera") {
		t.Fatalf("plan: %v", ev)
	}
	s2 := l.index("start:2", 0)
	on := l.index("writing:true", 0)
	off := l.index("writing:false", on)
	done := l.index("done:2:", 0)
	if !(s2 >= 0 && on > s2 && off > on && done > off) {
		t.Errorf("order start:2=%d writing:true=%d writing:false=%d done:2=%d: %v", s2, on, off, done, ev)
	}
	if l.index("writing:true", on+1) >= 0 {
		t.Error("writing announced twice")
	}
	if l.index("done:1:confirmed", 0) < 0 || l.index("done:1:confirmed", 0) > on {
		t.Error("the confirmation must precede the write")
	}
	if ev[len(ev)-1] != "end" {
		t.Errorf("last event %q", ev[len(ev)-1])
	}

	// not confirmed: never a write
	w2 := newFlashWorld(t, testfake.FlashOptions{})
	w2.update("")
	l2 := &opLog{}
	h2 := w2.hooks()
	h2.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		return opUI{cli.NewPlainUI(o, st, log, secrets, dry), l2}
	}
	r = runWith(w2.env, h2, "n\n", append(w2.base(), "update", "--image", w2.img)...)
	expectRC(t, r, 1)
	if l2.index("writing:true", 0) >= 0 || l2.index("start:2", 0) >= 0 {
		t.Errorf("a write was announced without a yes: %v", l2.events())
	}
	w2.unchanged(t)
}

// With a checklist (the session), the power-cycle wait is one key and no line prompt is asked; stopping
// it stops the step; without one the line prompt is as before (TestVerifyEndToEnd).
func TestVerifyUsesTheChecklist(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	l := &opLog{}
	r := w.runOps(l, "n\nn\n", "verify")
	expectRC(t, r, 0)
	if l.index("checklist:Power-cycle the camera by hand (the assistant never does it):3", 0) < 0 {
		t.Errorf("no checklist: %v", l.events())
	}
	if strings.Contains(r.out+r.err, "Press Enter when") {
		t.Error("a text prompt was asked next to the checklist")
	}
	if w.readState("step_verify_done") == "" {
		t.Error("verify not marked done")
	}

	w = newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	l = &opLog{stopAll: true}
	r = w.runOps(l, "", "verify")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "stopped.")
	if w.readState("step_verify_done") != "" {
		t.Error("verify marked done after a stop")
	}
}
