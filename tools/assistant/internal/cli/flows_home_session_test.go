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
	escOnWait  bool // a search listens: answer Esc at once (the user stops it)
	boards     []ui.Board
	headers    []ui.Header
	closed     int
	waitedStop int
}

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
	esc := s.escOnWait
	s.mu.Unlock()
	if esc {
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

// TestNoNetworkBeforeScanYes (spec 6.4): the home, the chooser and the scan key send nothing; the
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
	bs := &boardSession{keys: []string{"c", "s", "esc", "esc"}, escOnWait: true}
	r := runBoard(t, e, bs, "y\n")
	expectRC(t, r, 0)
	if bs.waitedStop == 0 {
		t.Error("Esc was not listened for during the scan")
	}
	if n.Load() == 0 {
		t.Error("the scan never started")
	}
	if got := bs.last(); len(got.Rows) != 7 {
		t.Errorf("the home did not come back: %d rows", len(got.Rows))
	}
}

// The home rows are the seven lines of the mockup, with the digits as hidden shortcuts and Enter
// opening the row under the cursor.
func TestHomeBoardRows(t *testing.T) {
	e := newEnv(t)
	bs := &boardSession{keys: []string{"esc"}}
	r := runBoard(t, e, bs, "")
	expectRC(t, r, 0)
	b := bs.boards[0]
	var labels []string
	for i, row := range b.Rows {
		labels = append(labels, row.Label)
		if row.Hot != string(rune('1'+i)) {
			t.Errorf("row %d hot key %q", i, row.Hot)
		}
	}
	want := "Set up a new camera|Update this camera|Camera options|Back up / restore|Find cameras|Settings|Check this computer"
	if strings.Join(labels, "|") != want {
		t.Errorf("rows: %v", labels)
	}
}
