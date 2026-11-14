package cli_test

import (
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

var updateGolden = flag.Bool("update-flows", false, "rewrite the golden files of the full-screen flows")

func init() {
	lipgloss.SetColorProfile(termenv.Ascii)
	tui.SetTheme(tui.NewTheme(tui.Env{Profile: termenv.Ascii, ASCII: true})) // fixed glyphs and no colour, whatever the host
}

func flowGolden(t *testing.T, name, got string) {
	t.Helper()
	lines := strings.Split(got, "\n")
	for i := range lines {
		lines[i] = strings.TrimRight(lines[i], " ")
	}
	got = strings.Join(lines, "\n")
	p := filepath.Join("testdata", name+".golden")
	if *updateGolden {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("missing %s (run: go test ./internal/cli -run Golden -update-flows): %v", p, err)
	}
	if string(want) != got {
		t.Errorf("%s differs\n--- want ---\n%s\n--- got ---\n%s", name, want, got)
	}
}

func sizedMenu(w, h int, title string, m *tui.Menu) *tui.Menu {
	m.Update(tea.WindowSizeMsg{Width: w, Height: h})
	return m
}

// The home screen in the full-screen menu: 80x24 with a camera chosen, a small window with none, and
// the one-letter shortcut that changes the camera.
func TestHomeScreenGolden(t *testing.T) {
	title, items := cli.HomeMenu("1.3.0", "salon", "192.0.2.21", "akam", "v1.2.0-3-gab12", true)
	m := sizedMenu(80, 24, title, tui.NewMenu(title, items))
	flowGolden(t, "home_80x24", m.View())

	title, items = cli.HomeMenu("1.3.0", "default", "", "", "", true)
	small := sizedMenu(40, 10, title, tui.NewMenu(title, items))
	small.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("G")})
	if n := strings.Count(small.View(), "\n"); n > 10 {
		t.Errorf("%d lines in a 10 line window", n)
	}
	flowGolden(t, "home_small_none", small.View())

	m = tui.NewMenu(title, items)
	m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("c")})
	if !m.Done || m.Quit || items[m.Cursor].Key != "c" {
		t.Errorf("the c shortcut: done=%v quit=%v cursor=%d", m.Done, m.Quit, m.Cursor)
	}
}
