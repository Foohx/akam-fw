package tui

import (
	"fmt"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

var t0 = time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)

// opModel is a session with a fixed clock; advance moves it.
func opModel(w, h int) (*sessModel, func(d time.Duration)) {
	m := layoutModel(w, h)
	now := t0
	m.now = func() time.Time { return now }
	return m, func(d time.Duration) { now = now.Add(d) }
}

// op feeds an Operator call to the model; "writing" also sets the session atomic like OpWriting does.
func op(m *sessModel, v msgOp) {
	if v.kind == "writing" {
		if v.on {
			m.st.wrote.Store(true)
		}
		m.st.writing.Store(v.on)
	}
	m.Update(v)
}

func backupOp(m *sessModel, adv func(time.Duration)) {
	m.Update(msgBanner{"Back up the camera - salon"})
	op(m, msgOp{kind: "plan", items: []string{"Connect over SSH", "Check free space", "Dump the flash", "Verify checksums"}})
	op(m, msgOp{kind: "start", i: 0})
	op(m, msgOp{kind: "done", i: 0, text: "192.0.2.41"})
	op(m, msgOp{kind: "start", i: 1})
	op(m, msgOp{kind: "done", i: 1, text: "12.4 GB free"})
	op(m, msgOp{kind: "start", i: 2})
	op(m, msgOp{kind: "status", text: "dd if=/dev/mtdblock2 bs=64k   (runs on the camera, read only)"})
	adv(8 * time.Second)
	op(m, msgOp{kind: "bytes", a: 8_700_000, b: 16 << 20})
}

func TestOperationPanelGoldens(t *testing.T) {
	for _, sz := range [][2]int{{80, 24}, {60, 16}, {120, 40}} {
		w, h := sz[0], sz[1]
		tag := fmt.Sprintf("%dx%d", w, h)
		m, adv := opModel(w, h)
		backupOp(m, adv)
		v := m.View()
		checkFits(t, "op "+tag, v, w, h)
		golden(t, "op_running_"+tag, v)
		for _, want := range []string{"Esc", "stop (nothing on the camera changes)"} {
			if h >= 14 && !strings.Contains(v, want) && w >= 80 {
				t.Errorf("%s: footer lacks %q:\n%s", tag, want, v)
			}
		}

		// a write: no stop offered, the footer says so
		m, adv = opModel(w, h)
		backupOp(m, adv)
		op(m, msgOp{kind: "writing", on: true})
		v = m.View()
		checkFits(t, "write "+tag, v, w, h)
		if strings.Contains(v, "Esc") || strings.Contains(v, "Ctrl-C") {
			t.Errorf("%s: a stop is offered during a write:\n%s", tag, v)
		}
		if w >= 80 && !strings.Contains(v, "cannot be interrupted safely") {
			t.Errorf("%s: the footer does not say the write cannot be interrupted:\n%s", tag, v)
		}
		golden(t, "op_writing_"+tag, v)

		// after: the list is gone, the result is on the strip
		m, adv = opModel(w, h)
		backupOp(m, adv)
		op(m, msgOp{kind: "end"})
		m.Update(msgLine{entry{kGood, "Backup saved: 8 files, 16.0 MB"}})
		v = m.View()
		checkFits(t, "after "+tag, v, w, h)
		if strings.Contains(v, "Dump the flash") || !strings.Contains(v, "Backup saved: 8 files") {
			t.Errorf("%s: after the operation:\n%s", tag, v)
		}
		golden(t, "op_after_"+tag, v)
	}
}

func TestOperationTimingAndSpeed(t *testing.T) {
	m, adv := opModel(100, 30)
	backupOp(m, adv)
	v := m.View()
	for _, want := range []string{"51 %", "8.3 / 16.0 MB", "1.0 MB/s", "8 s, about 8 s left", "[ok] Check free space", "12.4 GB free", "| Dump the flash", "[-] Verify checksums"} {
		if !strings.Contains(v, want) && !strings.Contains(strings.ReplaceAll(v, "|", "~"), want) {
			t.Errorf("missing %q in:\n%s", want, v)
		}
	}
	if !strings.Contains(v, "step 3 of 4") {
		t.Errorf("step counter:\n%s", v)
	}
	if !strings.Contains(v, "> dd if=/dev/mtdblock2") {
		t.Errorf("the command is not shown:\n%s", v)
	}
}

func TestOperationNoPercentWithoutSize(t *testing.T) {
	m, _ := opModel(80, 24)
	op(m, msgOp{kind: "plan", items: []string{"Download the kit"}})
	op(m, msgOp{kind: "start", i: 0})
	v := m.View()
	if strings.Contains(v, "%") || strings.Contains(v, "MB") {
		t.Errorf("a bar without a size:\n%s", v)
	}
}

func TestProgressShowsElapsedAndLeft(t *testing.T) {
	m, adv := opModel(100, 24)
	m.Update(msgBanner{"Flash"})
	m.Update(msgProgress{"APP", 10})
	adv(10 * time.Second)
	m.Update(msgProgress{"APP", 20})
	v := m.View()
	if !strings.Contains(v, "10 s, about 40 s left") {
		t.Errorf("elapsed and time left:\n%s", v)
	}
}

// Esc stops only where it is safe; a write ignores Esc and Ctrl-C and says why.
func TestOperationStopKeys(t *testing.T) {
	m, adv := opModel(80, 24)
	backupOp(m, adv)
	m.Update(key("esc"))
	if !m.stopping || !m.st.stopped() {
		t.Fatal("Esc did not ask the flow to stop")
	}
	if m.strip == nil || !strings.Contains(m.strip.text, "Nothing on the camera changes") {
		t.Errorf("strip: %+v", m.strip)
	}

	m, adv = opModel(80, 24)
	backupOp(m, adv)
	op(m, msgOp{kind: "writing", on: true})
	for _, k := range []string{"esc", "ctrl+c", "ctrl+c", "esc"} {
		_, cmd := m.Update(key(k))
		if cmd != nil {
			if msg := cmd(); msg != nil {
				if _, quit := msg.(tea.QuitMsg); quit {
					t.Fatalf("%s quit the program during a write", k)
				}
			}
		}
		if m.stopping || m.st.stopped() {
			t.Fatalf("%s asked the flow to stop during a write", k)
		}
		if m.strip == nil || !strings.Contains(m.strip.text, "cannot be interrupted safely") {
			t.Errorf("%s: strip %+v", k, m.strip)
		}
	}
	// once the write is over the keys work again
	op(m, msgOp{kind: "writing", on: false})
	m.Update(key("esc"))
	if !m.stopping {
		t.Error("Esc is still blocked after the write")
	}
}

// The "?" pane still opens during an operation and Esc closes it without stopping.
func TestOperationDetailsPane(t *testing.T) {
	m, adv := opModel(80, 24)
	backupOp(m, adv)
	m.Update(msgLine{entry{kSay, "  Reading mtdblock2 (read 1 of 2) ..."}})
	m.Update(key("?"))
	if m.pane == nil {
		t.Fatal("no details pane")
	}
	m.Update(key("esc"))
	if m.pane != nil || m.stopping {
		t.Errorf("pane=%v stopping=%v", m.pane, m.stopping)
	}
}

// The board replaces the list (the flow went back home); a flow's end releases a stuck write mark.
func TestOperationClearedByBoardAndFlowEnd(t *testing.T) {
	m, adv := opModel(80, 24)
	backupOp(m, adv)
	m.Update(msgBoardSet{ui.Board{Title: "Home"}})
	if m.op != nil {
		t.Error("the list survived the board")
	}
	m, adv = opModel(80, 24)
	backupOp(m, adv)
	op(m, msgOp{kind: "writing", on: true})
	m.Update(msgFlowDone{})
	if m.writing() {
		t.Error("a write mark outlived the flow")
	}
}

// Through the real sessionUI: Operator calls reach the model, a checklist is one key.
func TestSessionOperatorAndChecklist(t *testing.T) {
	h := newHarness(t, 80, 24, nil)
	h.run(func(u ui.UI) error {
		o := u.(ui.Operator)
		o.OpPlan([]string{"Check", "Write"})
		o.OpStart(0)
		o.OpDone(0, "ok")
		o.OpStart(1)
		o.OpWriting(true)
		o.OpWriting(false)
		o.OpEnd()
		return u.(ui.Checklister).Checklist("Power-cycle by hand", []string{"Unplug", "Plug"})
	})
	q := h.ask()
	if _, ok := q.model.(*Checklist); !ok {
		t.Fatalf("overlay %T", q.model)
	}
	if !strings.Contains(h.m.View(), "[ ] 1. Unplug") || !strings.Contains(h.m.View(), "Space/Enter") {
		t.Errorf("checklist:\n%s", h.m.View())
	}
	h.press(" ")
	h.finish()
	if h.err != nil {
		t.Errorf("flow error %v", h.err)
	}
	if h.m.op != nil {
		t.Error("the list was not removed")
	}
}
