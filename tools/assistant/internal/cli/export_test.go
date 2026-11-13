package cli

import (
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
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

// FirmwareLabels exposes the lines of the "Which firmware?" list (update or install, the version the
// camera runs, the releases newest first, the kits already downloaded, the recorded image, a verified stock
// backup) to the tests, including the full-screen list.
func FirmwareLabels(update bool, cur string, versions, downloaded []string, built string, stockOK bool, flags ...bool) []string {
	mode := fwInstall
	if update {
		mode = fwUpdate
	}
	var rels []kit.Release
	for _, v := range versions {
		rels = append(rels, kit.Release{Version: v, Tag: "v" + v})
	}
	var out []string
	l := fwList{Mode: mode, Cur: cur, Rels: rels, Downloaded: downloaded, Built: built, StockOK: stockOK}
	if len(flags) > 0 { // builtFirst, retry, imported, onCamera
		l.BuiltFirst = flags[0]
	}
	if len(flags) > 1 {
		l.Retry = flags[1]
	}
	if len(flags) > 2 {
		l.BuiltImported = flags[2]
	}
	if len(flags) > 3 {
		l.BuiltOnCamera = flags[3]
	}
	for _, c := range fwChoices(l) {
		out = append(out, c.label)
	}
	return out
}

// SetCtlTimeout shortens the control-port exchange timeout and returns the function that restores it.
func SetCtlTimeout(d time.Duration) func() {
	old := ctlTimeout
	ctlTimeout = d
	return func() { ctlTimeout = old }
}
