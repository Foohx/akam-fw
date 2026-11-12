package tui

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/muesli/termenv"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func boardModel(w, h int, hdr ui.Header, b ui.Board) *sessModel {
	st := newSessState()
	m := newSessModel(NewTheme(Env{Profile: termenv.Ascii, ASCII: true}), st)
	m.Update(tea.WindowSizeMsg{Width: w, Height: h})
	m.Update(msgHeader{hdr})
	m.Update(msgBoardSet{b})
	return m
}

var testHeader = ui.Header{Product: "Akam 0.9  linux/amd64", Name: "salon", IP: "192.0.2.41", Firmware: "Akam 1.4.2",
	Badges: []ui.HeaderBadge{{Level: "ok", Text: "ready"}, {Level: "warn", Text: "clock off"}, {Level: "ok", Text: "-58 dBm"}}}

var chooserKeys = []ui.BoardKey{{Key: "Enter", Word: "use"}, {Key: "s", Word: "scan"}, {Key: "o", Word: "ONVIF search"},
	{Key: "t", Word: "type"}, {Key: "r", Word: "recheck"}, {Key: "f", Word: "forget"}, {Key: "Esc", Word: "back"}}

func chooserBoard() ui.Board {
	return ui.Board{Title: "Choose the camera", Keys: chooserKeys, Help: []string{"help line"},
		Empty: "No camera known yet. Press s to scan, or t to type an address.",
		Rows: []ui.BoardRow{
			{ID: "192.0.2.41", Label: "salon", Detail: "192.0.2.41", Badge: ui.HeaderBadge{Level: "ok", Text: "Akam 1.4.2"}, Note: "remembered"},
			{ID: "192.0.2.58", Label: "garage", Detail: "192.0.2.58", Badge: ui.HeaderBadge{Level: "info", Text: "checking..."}, Note: "remembered"},
			{ID: "192.0.2.77", Label: "porch", Detail: "192.0.2.77", Badge: ui.HeaderBadge{Level: "muted", Text: "not seen"}, Note: "seen 2 min ago"},
			{ID: "192.0.2.90", Label: "(new)", Detail: "192.0.2.90", Badge: ui.HeaderBadge{Level: "ok", Text: "stock"}, Note: "found by scan"},
		}}
}

func TestBoardGoldens(t *testing.T) {
	hdr := testHeader
	empty := chooserBoard()
	empty.Rows = nil
	scanning := chooserBoard()
	scanning.Keys = []ui.BoardKey{{Key: "Esc", Word: "stops"}}
	scanning.Status, scanning.StatusLevel = "Scanning 192.0.2.0/24 on 4 ports", "info"
	home := ui.Board{DetailMuted: true, Keys: []ui.BoardKey{{Key: "Enter", Word: "open"}, {Key: "c", Word: "camera"}, {Key: "8"}, {Key: "q", Word: "quit"}},
		Help: []string{"help"},
		Rows: []ui.BoardRow{
			{Label: "Set up a new camera", Detail: "first install from the stock firmware", Hot: "1"},
			{Label: "Update this camera", Detail: "Akam to Akam, over SSH", Hot: "2"},
			{Label: "Camera options", Detail: "LED, picture, video, audio, network", Hot: "3"},
			{Label: "Back up / restore", Detail: "dump the flash, restore the original", Hot: "4"},
			{Label: "Find cameras", Detail: "remembered, scan, typed address", Hot: "5"},
			{Label: "Settings", Detail: "backup folder, state folder, licences", Hot: "6"},
			{Label: "Check this computer", Detail: "what works here, what is missing", Hot: "7"},
		}}
	for _, tc := range []struct {
		name string
		b    ui.Board
		w, h int
	}{
		{"board_chooser_empty", empty, 80, 24},
		{"board_chooser_rows", chooserBoard(), 80, 24},
		{"board_chooser_scanning", scanning, 80, 24},
		{"board_chooser_60x16", chooserBoard(), 60, 16},
		{"board_home", home, 80, 24},
		{"board_home_120x40", home, 120, 40},
	} {
		golden(t, tc.name, boardModel(tc.w, tc.h, hdr, tc.b).View())
	}
}

func press(m *sessModel, keys ...string) {
	for _, k := range keys {
		var msg tea.KeyMsg
		switch k {
		case "enter":
			msg = tea.KeyMsg{Type: tea.KeyEnter}
		case "esc":
			msg = tea.KeyMsg{Type: tea.KeyEsc}
		case "down":
			msg = tea.KeyMsg{Type: tea.KeyDown}
		case "ctrl+c":
			msg = tea.KeyMsg{Type: tea.KeyCtrlC}
		default:
			msg = tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(k)}
		}
		m.Update(msg)
	}
}

func drain(m *sessModel) []boardEv {
	var out []boardEv
	for {
		select {
		case ev := <-m.st.boardKeys:
			out = append(out, ev)
		default:
			return out
		}
	}
}

// Only the reported keys reach the flow; the digits are hidden shortcuts; Ctrl-C is Esc.
func TestBoardKeys(t *testing.T) {
	m := boardModel(80, 24, testHeader, chooserBoard())
	press(m, "x", "down", "down", "enter", "s", "ctrl+c", "esc")
	got := drain(m)
	want := []boardEv{{"enter", 2}, {"s", 2}, {"esc", 2}, {"esc", 2}}
	if len(got) != len(want) {
		t.Fatalf("events %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("event %d: %v, want %v", i, got[i], want[i])
		}
	}
	home := ui.Board{Keys: []ui.BoardKey{{Key: "8"}}, Rows: []ui.BoardRow{{Label: "a", Hot: "1"}, {Label: "b", Hot: "2"}}}
	m = boardModel(80, 24, testHeader, home)
	press(m, "2", "8", "3")
	got = drain(m)
	if len(got) != 2 || got[0] != (boardEv{"enter", 1}) || got[1].key != "8" {
		t.Errorf("home events %v", got)
	}
}

// Rows replaced while the user moves keep the cursor on the same camera (live updates in place).
func TestBoardCursorFollowsRow(t *testing.T) {
	m := boardModel(80, 24, testHeader, chooserBoard())
	press(m, "down", "down") // porch
	b := chooserBoard()
	b.Rows = append([]ui.BoardRow{{ID: "192.0.2.10", Label: "(new)", Detail: "192.0.2.10"}}, b.Rows...)
	m.Update(msgBoardSet{b})
	press(m, "enter")
	ev := drain(m)
	if len(ev) != 1 || ev[0].row != 3 {
		t.Fatalf("cursor did not follow the row: %v", ev)
	}
}

// The footer never shows a hidden key and the board closes without leaving a trace.
func TestBoardFooterAndClose(t *testing.T) {
	m := boardModel(80, 24, testHeader, ui.Board{Keys: []ui.BoardKey{{Key: "8"}, {Key: "q", Word: "quit"}}, Rows: []ui.BoardRow{{Label: "a"}}})
	lines := strings.Split(m.View(), "\n")
	if f := lines[len(lines)-1]; strings.Contains(f, "8") || !strings.Contains(f, "q quit") {
		t.Errorf("footer: %q", f)
	}
	m.Update(msgBoardClose{})
	if m.board != nil {
		t.Error("board still set")
	}
	press(m, "esc")
	if len(drain(m)) != 0 {
		t.Error("a key reached the flow without a board")
	}
}
