package cli_test

import (
	"bytes"
	"context"
	"errors"
	"net"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// scriptMenuUI is a plain UI that also has the full-screen menu: it picks the scripted lines (by a
// substring of the label), records every menu it shows with its result strip, and every prompt.
type scriptMenuUI struct {
	*ui.Plain
	picks   []string
	views   []ui.MenuView
	prompts []string
}

func (s *scriptMenuUI) Menu(title string, items []ui.MenuItem) (int, error) {
	return s.MenuWithResult(ui.MenuView{Title: title, Items: items})
}

func (s *scriptMenuUI) MenuWithResult(v ui.MenuView) (int, error) {
	s.views = append(s.views, v)
	if len(s.picks) == 0 {
		return -1, nil
	}
	p := s.picks[0]
	s.picks = s.picks[1:]
	for i, it := range v.Items {
		if strings.Contains(it.Label, p) {
			return i, nil
		}
	}
	return -1, nil
}

func (s *scriptMenuUI) Prompt(p, def string) (string, error) {
	s.prompts = append(s.prompts, p)
	return s.Plain.Prompt(p, def)
}

func runWithMenu(t *testing.T, e *env, picks []string, stdin string, args ...string) (*scriptMenuUI, result) {
	t.Helper()
	t.Setenv("AKAM_FORCE_TTY", "1")
	var su *scriptMenuUI
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, sec *state.Secrets, dry bool) ui.UI {
		su = &scriptMenuUI{Plain: cli.NewPlainUI(o, st, log, sec, dry), picks: picks}
		return su
	}
	var out, errb bytes.Buffer
	rc := cli.Run(append(e.base(), args...), cli.Streams{In: strings.NewReader(stdin), Out: &out, Err: &errb}, "test", h)
	return su, result{out.String(), errb.String(), rc}
}

func noPressEnter(t *testing.T, s *scriptMenuUI) {
	t.Helper()
	for _, p := range s.prompts {
		if strings.Contains(p, "Press Enter") {
			t.Errorf("a Press Enter prompt was issued: %q", p)
		}
	}
}

// Home: an action returns to the home screen with its one-line result and no prompt.
func TestHomeNoPressEnter(t *testing.T) {
	e := newEnv(t)
	s, r := runWithMenu(t, e, []string{"Check this computer"}, "", "--camera", "salon")
	expectRC(t, r, 0)
	noPressEnter(t, s)
	if len(s.views) != 2 {
		t.Fatalf("want 2 home screens, got %d", len(s.views))
	}
	if s.views[0].Result.Text != "" {
		t.Errorf("first screen must have no result: %+v", s.views[0].Result)
	}
	res := s.views[1].Result
	if res.Text == "" || strings.ContainsAny(res.Text, "\n") {
		t.Errorf("second screen needs a one-line result, got %+v", res)
	}
	if res.Level != "ok" && res.Level != "warn" && res.Level != "error" {
		t.Errorf("bad level %q", res.Level)
	}
}

// Step menu: same, with read-only camera tools.
func TestStepMenuNoPressEnter(t *testing.T) {
	e := newEnv(t)
	s, r := runWithMenu(t, e, []string{"Camera: show LEDs", "Cameras"}, "", "--camera", "salon", "steps")
	expectRC(t, r, 0)
	noPressEnter(t, s)
	if len(s.views) != 3 {
		t.Fatalf("want 3 menus, got %d", len(s.views))
	}
	if s.views[0].Result.Text != "" {
		t.Errorf("first menu must have no result")
	}
}

// A declined or failed action leaves its message on the next screen; nothing waits for Enter.
func TestHomeFailureShownOnStrip(t *testing.T) {
	e := newEnv(t)
	e.http = &kit.Client{DialContext: noNet} // no network: every download is refused
	s, _ := runWithMenu(t, e, []string{"Update this camera"}, "n\n", "--camera", "salon", "--ip", "127.0.0.1")
	noPressEnter(t, s)
	if len(s.views) < 2 {
		t.Fatalf("want a second home screen, got %d", len(s.views))
	}
	if r := s.views[1].Result; r.Level != "error" || r.Text == "" {
		t.Errorf("a failed update must show an error on the strip, got %+v", r)
	}
}

func noNet(context.Context, string, string) (net.Conn, error) {
	return nil, errors.New("network disabled in tests")
}
