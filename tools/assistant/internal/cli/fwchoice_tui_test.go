package cli_test

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

func firmwarePicker(labels []string, title string) *tui.Picker {
	shown := labels
	if n := len(labels); labels[n-1] == "Back" { // the full-screen list hides Back: Esc does it
		shown = labels[:n-1]
	}
	return &tui.Picker{Title: title, Items: shown, W: 80, H: 24}
}

// The full-screen "Which firmware?" list of the update (80x24) and of the first install, and its keys: Enter
// takes the first row (the latest release), a digit jumps to a row, Esc goes back.
func TestFirmwareListGolden(t *testing.T) {
	labels := cli.FirmwareLabels(true, "1.5.0", []string{"2.0.0", "1.5.0", "1.0.0"}, []string{"1.0.0"}, "/home/u/akam-image/ak_rtsp_firmware.squashfs", true)
	m := firmwarePicker(labels, "Which firmware?  (the camera runs: b35-2af81c5; Akam 1.5.0 installed here)")
	flowGolden(t, "firmware_update_80x24", m.View())

	labels = cli.FirmwareLabels(false, "", []string{"2.0.0", "1.5.0"}, nil, "", false)
	flowGolden(t, "firmware_install_80x24", firmwarePicker(labels, "Which firmware?").View())

	m = firmwarePicker(labels, "Which firmware?")
	m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	if !m.Done || m.Cancelled || m.Cursor != 0 || !strings.HasPrefix(labels[m.Cursor], "Akam 2.0.0 (latest)") {
		t.Errorf("Enter: done=%v cancelled=%v cursor=%d", m.Done, m.Cancelled, m.Cursor)
	}
	m = firmwarePicker(labels, "Which firmware?")
	m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	if !m.Cancelled {
		t.Error("Esc does not leave the list")
	}
	m = firmwarePicker(labels, "Which firmware?")
	m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("2")})
	if !m.Done || labels[m.Cursor] != "Akam 1.5.0  - older" {
		t.Errorf("digit 2: done=%v cursor=%d", m.Done, m.Cursor)
	}
}
