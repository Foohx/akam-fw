package cli

import (
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// HomeMenu exposes the home screen to the golden tests of the full-screen menu (package cli_test).
func HomeMenu(version, name, ip, kind, fw string, hotkey bool) (string, []ui.MenuItem) {
	title, items := homeScreen(homeInfo{Version: version, Name: name, IP: ip, Kind: kind, FW: fw,
		Seen: time.Date(2026, 11, 11, 11, 57, 0, 0, time.UTC), Now: time.Date(2026, 11, 11, 12, 0, 0, 0, time.UTC), Hotkey: hotkey, Platform: "linux/amd64"})
	out := make([]ui.MenuItem, len(items))
	for i, it := range items {
		out[i] = it.MenuItem
	}
	return title, out
}
