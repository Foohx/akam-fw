package cli_test

import (
	"context"
	"errors"
	"net"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// boardOuter is a plain UI with a session, like the TUI; the UI the flow gets inside it (boardSession)
// is not a Sessioner, has a header and a board, and takes its keys from a script.
type boardOuter struct {
	*ui.Plain
	s *boardSession
}

func (o boardOuter) Session(fn func(ui.UI) error) error { return fn(o.s) }

type boardSession struct {
	*ui.Plain
	mu         sync.Mutex
	keys       []string
	escOnWait  bool        // a search listens: answer Esc (the user stops it)
	endAfter   func() bool // when set, a search that listens finds the session ended (ErrNoInput) once it returns true
	escAfter   func() bool // when set, Esc is answered only once it returns true (the search really started)
	boards     []ui.Board
	headers    []ui.Header
	closed     int
	waitedStop int
	clears     int
}

func (s *boardSession) ClearOutput() { s.mu.Lock(); s.clears++; s.mu.Unlock() }

func (s *boardSession) SetHeader(h ui.Header) {
	s.mu.Lock()
	s.headers = append(s.headers, h)
	s.mu.Unlock()
}
func (s *boardSession) SetBoard(b ui.Board) {
	s.mu.Lock()
	s.boards = append(s.boards, b)
	s.mu.Unlock()
}
func (s *boardSession) CloseBoard() { s.mu.Lock(); s.closed++; s.mu.Unlock() }

func (s *boardSession) BoardKey() (string, int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.keys) == 0 {
		return "", -1, ui.ErrNoInput
	}
	k := s.keys[0]
	s.keys = s.keys[1:]
	return k, 0, nil
}

func (s *boardSession) WaitBoardKey(stop <-chan struct{}) (string, int, error) {
	if stop == nil {
		return s.BoardKey()
	}
	s.mu.Lock()
	s.waitedStop++
	esc, after, end := s.escOnWait, s.escAfter, s.endAfter
	s.mu.Unlock()
	if end != nil {
		for !end() {
			select {
			case <-stop:
				return "", -1, nil
			case <-time.After(time.Millisecond):
			}
		}
		return "", -1, ui.ErrNoInput
	}
	if esc {
		for after != nil && !after() { // the user presses Esc once the search is under way
			select {
			case <-stop:
				return "", -1, nil
			case <-time.After(time.Millisecond):
			}
		}
		return "esc", 0, nil
	}
	<-stop
	return "", -1, nil
}

func (s *boardSession) last() ui.Board {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.boards[len(s.boards)-1]
}

func (s *boardSession) hasStatus(prefix string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, b := range s.boards {
		if strings.HasPrefix(b.Status, prefix) {
			return true
		}
	}
	return false
}

func (s *boardSession) hasStatusMatch(re *regexp.Regexp) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, b := range s.boards {
		if re.MatchString(b.Status) {
			return true
		}
	}
	return false
}

// runBoard runs the home with a board session and the scripted keys; stdin answers the questions.
func runBoard(t *testing.T, e *env, bs *boardSession, stdin string) result {
	t.Helper()
	t.Setenv("AKAM_FORCE_TTY", "1")
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		p := cli.NewPlainUI(o, st, log, secrets, dry)
		bs.Plain = p
		return boardOuter{p, bs}
	}
	// the first line answers "register this camera?" (No)
	return runWith(e, h, "\n"+stdin, append(e.base(), "--camera", "salon")...)
}

// countDial wraps the discovery dialer: it counts every connection attempt and refuses them all
// except the documentation address of the fake camera.
func (e *env) countDial(n *atomic.Int32, block bool) {
	inner := e.disc.Dial
	e.disc.Dial = func(ctx context.Context, network, addr string) (net.Conn, error) {
		n.Add(1)
		if block {
			<-ctx.Done()
			return nil, errors.New("stopped (test)")
		}
		return inner(ctx, network, addr)
	}
}

// TestNoNetworkBeforeScanYes (specs/installer.md, section 12): the home, the chooser and the scan key send nothing; the
// scan question defaults to No and only a yes dials.
func TestNoNetworkBeforeScanYes(t *testing.T) {
	for _, tc := range []struct {
		name, stdin string
		dials       bool
	}{{"default no", "\n", false}, {"explicit no", "n\n", false}, {"yes", "y\n", true}} {
		t.Run(tc.name, func(t *testing.T) {
			e := newEnv(t)
			e.fakeLAN("192.0.2.99")
			var n atomic.Int32
			e.countDial(&n, false)
			bs := &boardSession{keys: []string{"c", "s", "esc", "esc"}}
			r := runBoard(t, e, bs, tc.stdin)
			expectRC(t, r, 0)
			if got := n.Load() > 0; got != tc.dials {
				t.Fatalf("dials=%d, want dials=%v", n.Load(), tc.dials)
			}
			if tc.dials {
				found := false
				for _, row := range bs.last().Rows {
					found = found || row.Detail == "192.0.2.99"
				}
				// the last board is the home; the chooser showed the camera before Esc
				for _, b := range bs.boards {
					for _, row := range b.Rows {
						found = found || row.Detail == "192.0.2.99"
					}
				}
				if !found {
					t.Error("the camera found by the scan never appeared as a row")
				}
				if !bs.hasStatus("Scanning 192.0.2.0/24") {
					t.Error("no scanning status line was shown")
				}
				if !bs.hasStatusMatch(regexp.MustCompile(`^Scanning 192\.0\.2\.0/24 on 4 ports  [0-9]+ %$`)) {
					t.Error("the scanning status line never showed a percentage")
				}
			}
		})
	}
}

// Opening the home and the chooser (and rechecking nothing) sends nothing at all.
func TestHomeAndChooserSendNothing(t *testing.T) {
	e := newEnv(t)
	e.fakeLAN("192.0.2.99")
	var n atomic.Int32
	e.countDial(&n, false)
	bs := &boardSession{keys: []string{"c", "esc", "esc"}}
	r := runBoard(t, e, bs, "")
	expectRC(t, r, 0)
	if n.Load() != 0 {
		t.Fatalf("%d dials from the home and the chooser", n.Load())
	}
	chooser := bs.boards[1]
	if len(chooser.Rows) != 0 || !strings.Contains(chooser.Empty, "Press s to scan, or t to type an address") {
		t.Errorf("empty state: %+v", chooser)
	}
	var keys []string
	for _, k := range chooser.Keys {
		keys = append(keys, k.Key)
	}
	if strings.Join(keys, " ") != "Enter s o t r f Esc" {
		t.Errorf("chooser keys: %v", keys)
	}
	h := bs.headers[0]
	if !strings.HasPrefix(h.Product, "Akam test") || h.IP != "127.0.0.1" || len(h.Badges) != 1 || h.Badges[0].Text != "not checked" {
		t.Errorf("header: %+v", h)
	}
}

// Esc during a scan stops it: nothing is waited for, the board comes back with its keys.
func TestEscStopsScan(t *testing.T) {
	e := newEnv(t)
	e.fakeLAN("192.0.2.99")
	var n atomic.Int32
	e.countDial(&n, true) // every connection hangs until the context is cancelled
	bs := &boardSession{keys: []string{"c", "s", "esc", "esc"}, escOnWait: true, escAfter: func() bool { return n.Load() > 0 }}
	r := runBoard(t, e, bs, "y\n")
	expectRC(t, r, 0)
	if bs.waitedStop == 0 {
		t.Error("Esc was not listened for during the scan")
	}
	if n.Load() == 0 {
		t.Error("the scan never started")
	}
	if got := bs.last(); len(got.Rows) != 8 {
		t.Errorf("the home did not come back: %d rows", len(got.Rows))
	}
}

// L7: the chooser help says that r connects to the remembered addresses at once, without asking.
func TestChooserHelpSaysRecheckProbesAtOnce(t *testing.T) {
	e := newEnv(t)
	bs := &boardSession{keys: []string{"c", "esc", "esc"}}
	r := runBoard(t, e, bs, "y\n")
	expectRC(t, r, 0)
	help := strings.Join(bs.last().Help, "\n")
	bs.mu.Lock()
	for _, b := range bs.boards {
		if b.Title == "Choose the camera" {
			help = strings.Join(b.Help, "\n")
		}
	}
	bs.mu.Unlock()
	if !strings.Contains(help, "r checks the remembered cameras again at once, without asking") {
		t.Errorf("chooser help:\n%s", help)
	}
	if strings.Contains(help, "r checks the remembered cameras again, f forgets one. Each of them asks") {
		t.Errorf("the help still says that r asks first:\n%s", help)
	}
}

// The session ends while a search runs: the search is cancelled, nothing keeps dialling for nobody.
func TestSessionEndCancelsTheSearch(t *testing.T) {
	e := newEnv(t)
	e.fakeLAN("192.0.2.99")
	var n atomic.Int32
	e.countDial(&n, true) // every connection hangs until the context is cancelled
	bs := &boardSession{keys: []string{"c", "s"}, endAfter: func() bool { return n.Load() > 0 }}
	done := make(chan result, 1)
	start := time.Now()
	go func() { done <- runBoard(t, e, bs, "y\n") }()
	select {
	case <-done:
	case <-time.After(20 * time.Second):
		t.Fatal("the search kept running after the session ended")
	}
	// a probe that is not cancelled waits its own 2 s timeout
	if d := time.Since(start); d > 1500*time.Millisecond {
		t.Errorf("the search took %v to notice that the session ended", d)
	}
	if n.Load() == 0 {
		t.Error("the scan never started")
	}
}

// The home rows are the seven lines of the mockup plus Configs, with the digits as hidden shortcuts (8 is
// the camera chooser, so Configs is 9) and Enter opening the row under the cursor.
func TestHomeBoardRows(t *testing.T) {
	e := newEnv(t)
	bs := &boardSession{keys: []string{"esc"}}
	r := runBoard(t, e, bs, "")
	expectRC(t, r, 0)
	b := bs.boards[0]
	var labels []string
	for i, row := range b.Rows {
		labels = append(labels, row.Label)
		hot := string(rune('1' + i))
		if row.Label == "Configs" {
			hot = "9"
		}
		if row.Hot != hot {
			t.Errorf("row %d hot key %q", i, row.Hot)
		}
	}
	want := "Set up a new camera|Update this camera|Camera options|Back up / restore|Find cameras|Settings|Check this computer|Configs"
	if strings.Join(labels, "|") != want {
		t.Errorf("rows: %v", labels)
	}
}

// Every action of the home starts on a clean panel (the session transcript is cleared), once per
// action, not by the home itself.
func TestHomeClearsOutputAtEachAction(t *testing.T) {
	e := newEnv(t)
	bs := &boardSession{keys: []string{"c", "esc", "c", "esc", "esc"}}
	r := runBoard(t, e, bs, "")
	expectRC(t, r, 0)
	if bs.clears != 2 {
		t.Errorf("the output was cleared %d times, want 2 (two actions)", bs.clears)
	}
}
