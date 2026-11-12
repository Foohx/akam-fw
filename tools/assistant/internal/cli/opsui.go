package cli

import (
	"errors"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The long operations (backup, flash, update, restore, verify, download) describe their items to the
// full-screen session through ui.Operator. Plain does not implement it: every helper below is then a
// no-op, so the line output is exactly what it was.

// opr is the operation display of a command; the zero value (no Operator) does nothing.
type opr struct{ o ui.Operator }

// op returns the operation display of the current UI.
func (a *App) op() opr {
	o, _ := a.UI.(ui.Operator)
	return opr{o}
}

func (p opr) on() bool { return p.o != nil }

func (p opr) plan(items ...string) {
	if p.o != nil {
		p.o.OpPlan(items)
	}
}

func (p opr) start(i int) {
	if p.o != nil {
		p.o.OpStart(i)
	}
}

func (p opr) done(i int, note string) {
	if p.o != nil {
		p.o.OpDone(i, note)
	}
}

func (p opr) status(line string) {
	if p.o != nil {
		p.o.OpStatus(line)
	}
}

// writing marks a write to the camera: the screen offers no stop until writing(false).
func (p opr) writing(on bool) {
	if p.o != nil {
		p.o.OpWriting(on)
	}
}

func (p opr) end() {
	if p.o != nil {
		p.o.OpWriting(false)
		p.o.OpEnd()
	}
}

// bytes returns a callback for a stream of total bytes that reports at most ten times a second (and
// the last byte), or nil without an Operator. base is what earlier streams of the same item moved.
func (p opr) bytes(base, total int64) func(n int64) {
	if p.o == nil {
		return nil
	}
	var last time.Time
	return func(n int64) {
		if now := time.Now(); now.Sub(last) >= 100*time.Millisecond || base+n >= total {
			last = now
			p.o.OpBytes(base+n, total)
		}
	}
}

// handsDone waits for the user to do physical steps by hand (power-cycle, card out). In the session it
// is a checklist acknowledged by one key; everywhere else it is the line prompt, unchanged. stopped is
// true when the user chose to stop (q, Esc, Ctrl-C or "q" typed); err is set when no answer could be
// read at all.
func (a *App) handsDone(title string, steps []string, prompt string) (stopped bool, err error) {
	if cl, ok := a.UI.(ui.Checklister); ok {
		if err := cl.Checklist(title, steps); err != nil {
			if errors.Is(err, ui.ErrNoInput) {
				return true, nil
			}
			return false, err
		}
		return false, nil
	}
	ans, err := a.UI.Prompt(prompt, "")
	if err != nil {
		return false, err
	}
	return ans == "q", nil
}
