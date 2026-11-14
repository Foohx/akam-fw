package cli

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The home screen and the camera chooser inside the full-screen session (specs/installer.md). They
// need a UI with a header (ui.HeaderSetter) and persistent lists (ui.Boarder); every other UI keeps
// the Pick based screens of flows_home.go and flows_camera.go, byte for byte.
//
// Nothing here sends anything to the network: the header shows what is remembered about the camera
// (its state folder) and, once a flow has read the camera (the options screen), what that flow read.

// liveHealth is what a flow read of the chosen camera, kept for the badges of the header.
type liveHealth struct {
	name, ip string
	h        camHealth
}

// noteHealth remembers a snapshot a flow has just read (SSH answered) and refreshes the header.
func (a *App) noteHealth(h camHealth) {
	a.live = &liveHealth{name: a.Cams.Name, ip: a.camAddr(), h: h}
	a.refreshHeader()
}

// camAddr is the remembered address of the chosen camera (no network).
func (a *App) camAddr() string {
	if a.Opt.IP != "" {
		return a.Opt.IP
	}
	return a.St.Get("camera_ip")
}

// healthBadges are the badges of a snapshot: ready, clock, Wi-Fi (specs/installer.md, section 10).
func healthBadges(h camHealth) []ui.HeaderBadge {
	b := []ui.HeaderBadge{{Level: "ok", Text: "ready"}}
	switch {
	case clockUnset(h):
		b = append(b, ui.HeaderBadge{Level: "warn", Text: "clock unset"})
	case h.camEpoch > 0 && h.pcEpoch > 0:
		if d := absInt64(h.camEpoch - h.pcEpoch); d > clockToleranceSec {
			b = append(b, ui.HeaderBadge{Level: "warn", Text: "clock off"})
		} else if h.ntp == "on" && h.ntpSync == "ok" {
			b = append(b, ui.HeaderBadge{Level: "ok", Text: "synced"})
		} else {
			b = append(b, ui.HeaderBadge{Level: "ok", Text: "clock ok"})
		}
	}
	switch wifiLevel(h.rssi) {
	case "good", "fair":
		b = append(b, ui.HeaderBadge{Level: "ok", Text: fmt.Sprintf("%d dBm", h.rssi)})
	case "weak":
		b = append(b, ui.HeaderBadge{Level: "warn", Text: "weak wifi"})
	case "poor":
		b = append(b, ui.HeaderBadge{Level: "error", Text: "poor wifi"})
	}
	return b
}

// sessionHeader is the header of the session: the product line, the chosen camera and its badges.
func (a *App) sessionHeader() ui.Header {
	h := a.homeInfo(true)
	hd := ui.Header{Product: fmt.Sprintf("Akam %s  %s", h.Version, h.Platform)}
	if h.IP == "" {
		return hd
	}
	hd.Name, hd.IP = h.Name, h.IP
	switch h.Kind {
	case "akam":
		hd.Firmware = "Akam"
		if h.FW != "" {
			hd.Firmware += " " + ui.Sanitize(h.FW)
		}
	case "stock":
		hd.Firmware = "stock firmware"
	}
	if l := a.live; l != nil && l.name == a.Cams.Name && l.ip == h.IP {
		hd.Badges = healthBadges(l.h)
		return hd
	}
	switch {
	case h.Kind == "stock":
		hd.Badges = []ui.HeaderBadge{{Level: "info", Text: "stock"}}
	case !h.Seen.IsZero():
		hd.Badges = []ui.HeaderBadge{{Level: "muted", Text: "seen " + shortAgo(h.Seen, h.Now)}}
	default:
		hd.Badges = []ui.HeaderBadge{{Level: "muted", Text: "not checked"}}
	}
	return hd
}

// shortAgo is discover.Ago without the trailing "ago", short enough for a badge.
func shortAgo(t, now time.Time) string {
	s := strings.TrimSuffix(discover.Ago(t, now), " ago")
	s = strings.NewReplacer(" minutes", " min", " hours", " h", " hour", " h", " days", " d", " day", " d").Replace(s)
	return s
}

// refreshHeader sends the header when the UI has one (the camera or its health changed).
func (a *App) refreshHeader() {
	if hs, ok := a.UI.(ui.HeaderSetter); ok {
		hs.SetHeader(a.sessionHeader())
	}
}

// homeBoard is the home screen of the session: short details, the digits stay as hidden shortcuts.
func (a *App) homeBoard() (ui.Board, []string) {
	type row struct{ act, label, detail string }
	rs := []row{
		{"setup", "Set up a new camera", "first install from the stock firmware"},
		{"update", "Update this camera", "Akam to Akam, over SSH"},
		{"options", "Camera options", "LED, picture, video, audio, network"},
		{"backup", "Back up / restore", "dump the flash, restore the original"},
		{"find", "Find cameras", "remembered, scan, typed address"},
		{"settings", "Settings", "backup folder, state folder, licences"},
		{"doctor", "Check this computer", "what works here, what is missing"},
		{"configs", "Configs", "global, local, copy"},
	}
	b := ui.Board{DetailMuted: true,
		Keys: []ui.BoardKey{{Key: "Enter", Word: "open"}, {Key: "c", Word: "camera"}, {Key: "8"}, {Key: "q", Word: "quit"}},
		Help: []string{
			"Enter opens the highlighted line; the digits 1 to 7 and 9 pick a line directly, c chooses another camera.",
			"Every action shows what it will do first and asks one yes/no (default No). The home screen itself sends nothing to the network.",
		},
	}
	acts := make([]string, len(rs))
	for i, r := range rs {
		hot := fmt.Sprint(i + 1)
		if r.act == "configs" {
			hot = "9" // 8 is the camera chooser
		}
		b.Rows = append(b.Rows, ui.BoardRow{ID: r.act, Label: r.label, Detail: r.detail, Hot: hot})
		acts[i] = r.act
	}
	return b, acts
}

// cmdHomeSession is the home loop on a board. The outcome of an action is on the result strip of the
// screen the user returns to; nothing waits for Enter.
func (a *App) cmdHomeSession(bd ui.Boarder) error {
	b, acts := a.homeBoard()
	for {
		a.refreshHeader()
		bd.SetBoard(b)
		key, row, err := bd.BoardKey()
		if err != nil {
			return nil
		}
		act := ""
		switch key {
		case "esc", "q":
			bd.CloseBoard()
			return nil
		case "c", "8":
			act = "camera"
		case "enter":
			if row >= 0 && row < len(acts) {
				act = acts[row]
			}
		}
		if act == "" {
			continue
		}
		label := "Camera"
		if row >= 0 && row < len(b.Rows) && key == "enter" {
			label = b.Rows[row].Label
		} else if act == "camera" {
			label = "Choose the camera"
		}
		bd.CloseBoard()
		a.clearOutput() // the panel of this action starts empty; the last output stays until here
		a.runScreen(label, func() error { return a.homeAction(act) })
	}
}

// ---------------------------------------------------------------- chooser

// boardCtl is the chooser on a board: it draws the rows again whenever one changes, and runs a search
// with Esc listened for.
type boardCtl struct {
	a      *App
	bd     ui.Boarder
	ch     *chooser
	status string // the search in progress, "" when idle
}

func (c *boardCtl) rows() []discover.Row {
	known, found, _ := c.ch.snapshot()
	return discover.Merge(known, found, time.Now())
}

func chooserKeys(searching bool) []ui.BoardKey {
	if searching {
		return []ui.BoardKey{{Key: "Esc", Word: "stops"}}
	}
	return []ui.BoardKey{{Key: "Enter", Word: "use"}, {Key: "s", Word: "scan"}, {Key: "o", Word: "ONVIF search"},
		{Key: "t", Word: "type"}, {Key: "r", Word: "recheck"}, {Key: "f", Word: "forget"}, {Key: "Esc", Word: "back"}}
}

// show draws the chooser; status is the search in progress ("" when idle).
func (c *boardCtl) show(status string) {
	rows := c.rows()
	_, _, checking := c.ch.snapshot()
	b := ui.Board{Title: "Choose the camera", Keys: chooserKeys(status != ""),
		Empty: "No camera known yet. Press s to scan, or t to type an address.",
		Help: []string{
			"A stock camera only answers while the toolkit card is in it and it has had 2 minutes to start; your router's client list (maker 'AltoBeam') shows its address.",
			"s scans your network, o searches with ONVIF, t takes an address or a name, f forgets one: each of them asks before sending anything.",
			"r checks the remembered cameras again at once, without asking: it only connects to the addresses already in the list.",
		}}
	now := time.Now()
	for _, r := range rows {
		row := chooserRow(r, checking, now)
		if r.Name != "" {
			if k := c.a.keyBrief(r.Name); k != "" {
				row.Note = strings.TrimSpace(row.Note + "  SSH key: " + k)
			}
			if t := sourceText(c.a.Cams.ReadKey(r.Name, keyConfigSource)); t != "" {
				row.Note = strings.TrimSpace(row.Note + "  settings: " + t)
			}
		}
		b.Rows = append(b.Rows, row)
	}
	if status != "" {
		b.Status, b.StatusLevel = status, "info"
	} else if n := strings.TrimSpace(c.ch.getNote()); n != "" {
		b.Status, b.StatusLevel = strings.Trim(n, "()"), "muted"
	}
	c.bd.SetBoard(b)
}

// chooserRow is one row of the chooser (every network-supplied field passes through ui.Sanitize).
func chooserRow(r discover.Row, checking map[string]bool, now time.Time) ui.BoardRow {
	row := ui.BoardRow{Label: "(new)"}
	switch {
	case r.Name != "":
		row.Label = r.Name
	case r.Host != "":
		row.Label = "(" + ui.Sanitize(r.Host) + ")"
	}
	if !r.Found {
		row.ID, row.Detail = r.LastIP, r.LastIP
		row.Badge = ui.HeaderBadge{Level: "muted", Text: "not seen"}
		row.Note = "seen " + discover.Ago(r.LastSeen, now)
		if checking[r.LastIP] {
			row.Badge = ui.HeaderBadge{Level: "info", Text: "checking..."}
			row.Note = "remembered"
		}
		return row
	}
	row.ID, row.Detail = r.IP, r.IP
	switch r.Kind {
	case discover.Akam:
		t := "Akam"
		if r.FW != "" {
			t += " " + ui.Sanitize(r.FW)
		}
		row.Badge = ui.HeaderBadge{Level: "ok", Text: t}
	case discover.Stock:
		row.Badge = ui.HeaderBadge{Level: "ok", Text: "stock"}
	case discover.Partial:
		row.Badge = ui.HeaderBadge{Level: "warn", Text: "starting"}
	}
	switch {
	case r.OldIP != "":
		row.Note = "was " + r.OldIP
	case r.Name == "":
		row.Note = "found by scan"
	default:
		row.Note = "remembered"
	}
	return row
}

func (c *boardCtl) setStatus(s string) {
	c.ch.mu.Lock()
	c.status = s
	c.ch.mu.Unlock()
}

func (c *boardCtl) getStatus() string {
	c.ch.mu.Lock()
	defer c.ch.mu.Unlock()
	return c.status
}

// scanProgress puts the percentage of the running scan on the status line (the calls come from the
// scan's goroutines; the caller only reports when the percentage changes).
func (c *boardCtl) scanProgress(label string, pct int) {
	s := fmt.Sprintf("%s  %d %%", label, pct)
	c.setStatus(s)
	c.show(s)
}

// run is chooser.run on a board: the status line, Esc listened for (it cancels the search through the
// context every probe and the scan already honour), the board drawn again as rows arrive.
func (c *boardCtl) run(status string, fn func()) {
	a := c.a
	c.setStatus(status)
	c.show(status)
	outer := a.Ctx
	if outer == nil {
		outer = context.Background()
	}
	ctx, cancel := context.WithCancel(outer)
	a.Ctx = ctx
	stop, joined := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(joined)
		for {
			k, _, err := c.bd.WaitBoardKey(stop)
			if err != nil { // the session ended: the search must not run on for nobody
				cancel()
				return
			}
			if k == "" {
				return
			}
			if k == "esc" {
				cancel()
				return
			}
		}
	}()
	fn()
	close(stop)
	<-joined
	stopped := ctx.Err() != nil
	cancel()
	a.Ctx = outer
	c.setStatus("")
	if stopped {
		c.ch.setNote("  (stopped)")
	}
	c.show("")
}

// chooseCameraBoard is chooseCamera on a persistent list.
func (a *App) chooseCameraBoard(bd ui.Boarder) (bool, error) {
	if err := a.UI.RequireTTY("choosing a camera"); err != nil {
		return false, err
	}
	ch := &chooser{known: a.knownCameras()}
	c := &boardCtl{a: a, bd: bd, ch: ch}
	ch.run = c.run
	ch.onChange = func() { c.show(c.getStatus()) }
	ch.progress = c.scanProgress
	defer bd.CloseBoard()
	for {
		c.show("")
		key, row, err := bd.BoardKey()
		if err != nil {
			return false, nil
		}
		rows := c.rows()
		switch key {
		case "esc":
			return false, nil
		case "enter":
			if row >= 0 && row < len(rows) {
				ok, err := a.selectRow(ch, rows[row])
				if err != nil || ok {
					return ok, err
				}
			}
		case "s":
			a.chooserScan(ch)
		case "o":
			a.chooserWSD(ch)
		case "t":
			a.chooserType(ch)
		case "r":
			a.recheckKnown(ch)
		case "f":
			if row >= 0 && row < len(rows) && rows[row].Name != "" {
				for _, k := range ch.known {
					if k.Name == rows[row].Name {
						a.forgetKnown(k)
					}
				}
			} else {
				a.forgetCameraScreen(ch)
			}
			ch.known = a.knownCameras()
		}
	}
}
