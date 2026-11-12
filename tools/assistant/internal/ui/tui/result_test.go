package tui

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func TestResultMenuStrip(t *testing.T) {
	items := []ui.MenuItem{{Label: "One", Status: "plain"}}
	for level, tag := range map[string]string{"ok": "[ok] ", "warn": "[!] ", "error": "[x] "} {
		v := resultMenu{NewMenu("T", items), ui.MenuResult{Level: level, Text: "it happened"}}.View()
		lines := strings.Split(strings.TrimRight(v, "\n"), "\n")
		if n := len(lines); n < 2 || !strings.Contains(lines[n-2], tag+"it happened") || !strings.Contains(lines[n-1], "q quit") {
			t.Errorf("%s: strip not above the footer:\n%s", level, v)
		}
	}
	plain := NewMenu("T", items).View()
	if got := (resultMenu{NewMenu("T", items), ui.MenuResult{}}).View(); got != plain {
		t.Error("an empty result must not change the menu")
	}
}
