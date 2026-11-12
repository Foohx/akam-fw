package cli

import (
	"fmt"
	"runtime"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The home screen (spec 11, 3.1): which camera is chosen, what is known about it, and the main
// actions. Everything it starts is a flow written once against ui.UI.

// homeInfo is what the header shows. It is read from the state of the chosen camera; the home screen
// itself sends nothing to the network.
type homeInfo struct {
	Version  string
	Name     string
	IP       string
	Kind     string // akam | stock | ""
	FW       string
	Seen     time.Time
	Now      time.Time
	Platform string // windows/amd64
	Hotkey   bool   // the interface has one-letter shortcuts (the full-screen menu)
}

type homeItem struct {
	ui.MenuItem
	Act string
}

// homeHeader are the two lines above the actions.
func homeHeader(h homeInfo) []string {
	l1 := fmt.Sprintf("Akam %s  (%s)", h.Version, h.Platform)
	change := "item 8 chooses another camera"
	if h.Hotkey {
		change = "c: change camera"
	}
	if h.IP == "" {
		none := "item 8 chooses one"
		if h.Hotkey {
			none = "press c"
		}
		return []string{l1, "Camera: none (" + none + ")"}
	}
	what := "address known, not checked yet"
	switch h.Kind {
	case "akam":
		what = "Akam"
		if h.FW != "" {
			what += " " + ui.Sanitize(h.FW)
		}
	case "stock":
		what = "stock firmware (toolkit card)"
	}
	if !h.Seen.IsZero() {
		what += ", seen " + discover.Ago(h.Seen, h.Now)
	}
	return []string{l1, fmt.Sprintf("Camera: %s  %s  %s  (%s)", h.Name, h.IP, what, change)}
}

// homeScreen is the title (header lines) and the entries of the home screen.
func homeScreen(h homeInfo) (string, []homeItem) {
	it := func(act, label, detail, key string) homeItem {
		return homeItem{ui.MenuItem{Label: label, Detail: detail, Status: "plain", Key: key}, act}
	}
	items := []homeItem{
		it("setup", "1  Set up a new camera", "First install from the stock firmware.", ""),
		it("update", "2  Update this camera", "Akam to Akam, over SSH, no card.", ""),
		it("options", "3  Camera options", "LED, picture, video, audio, network, security.", ""),
		it("backup", "4  Back up / restore", "Dump the flash, restore the original.", ""),
		it("find", "5  Find cameras on my network", "Remembered cameras, a scan, a typed address.", ""),
		it("settings", "6  Settings", "Backup folder, state folder, licences.", ""),
		it("doctor", "7  Check this computer", "What works here, what is missing.", ""),
		it("camera", "8  Change camera", "Choose another camera."+map[bool]string{true: " (shortcut: c)"}[h.Hotkey], "c"),
	}
	return strings.Join(homeHeader(h), "\n"), items
}

func (a *App) homeInfo(hotkey bool) homeInfo {
	h := homeInfo{Version: a.Version, Name: a.Cams.Name, Now: time.Now(), Hotkey: hotkey, Platform: runtime.GOOS + "/" + runtime.GOARCH}
	h.IP = a.Opt.IP
	if h.IP == "" {
		h.IP = a.St.Get("camera_ip")
	}
	h.Kind, h.FW = a.St.Get("camera_kind"), a.St.Get("camera_fw")
	if s := a.Cams.ReadSeen(a.Cams.Name); !s.At.IsZero() {
		h.Seen = s.At
	}
	return h
}

func (a *App) haveCamera() bool { return a.Opt.IP != "" || a.St.Get("camera_ip") != "" }

// needCamera runs the chooser when no camera is chosen yet (menu items 1 to 4).
func (a *App) needCamera() bool {
	if a.haveCamera() {
		return true
	}
	a.UI.Say("  No camera is chosen yet.")
	ok, err := a.chooseCamera()
	return err == nil && ok && a.haveCamera()
}

// cmdHome shows the home screen until the user quits.
func (a *App) cmdHome() error {
	if done, err := a.inSession(a.cmdHome); done { // the session UI is not a Sessioner: no recursion
		return err
	}
	if bd, ok := a.UI.(ui.Boarder); ok {
		return a.cmdHomeSession(bd)
	}
	var res ui.MenuResult
	for {
		_, hot := a.UI.(Menuer)
		title, items := homeScreen(a.homeInfo(hot))
		mi := make([]ui.MenuItem, len(items))
		for i, it := range items {
			mi[i] = it.MenuItem
		}
		var idx int
		var err error
		if m, ok := a.UI.(Menuer); ok {
			idx, err = a.showMenu(m, title, mi, res)
		} else {
			a.UI.Blank()
			for _, l := range strings.Split(title, "\n") {
				a.UI.Bold(l)
			}
			labels := make([]string, 0, len(mi)+1)
			for _, x := range items {
				labels = append(labels, x.Label[3:]+"  - "+x.Detail)
			}
			labels = append(labels, "Quit")
			idx, err = a.UI.Pick("What do you want to do?", labels)
			if err == nil && idx == len(items) {
				idx = -1
			}
		}
		if err != nil || idx < 0 {
			return nil
		}
		_, hot = a.UI.(Menuer)
		if !hot {
			a.reportFlowError(a.homeAction(items[idx].Act))
			continue
		}
		// The outcome is shown on the home screen the user returns to, not behind a "Press Enter".
		res = a.runAction(func() error { return a.homeAction(items[idx].Act) })
	}
}

// reportFlowError shows what a flow returned (ErrFailed was already shown).
func (a *App) reportFlowError(e error) {
	if e == nil || e == ui.ErrFailed {
		return
	}
	var ue *ui.UsageError
	if asUsage(e, &ue) {
		a.UI.Err(ue.Msg)
		return
	}
	a.UI.Err(e.Error())
}

func (a *App) homeAction(act string) error {
	switch act {
	case "setup":
		return a.flowInstall()
	case "update":
		if !a.needCamera() {
			return nil
		}
		return a.flowUpdate()
	case "options":
		if !a.needCamera() {
			return nil
		}
		return a.flowOptions()
	case "backup":
		return a.flowBackupMenu()
	case "find", "camera":
		_, err := a.chooseCamera()
		return err
	case "settings":
		return a.flowSettings()
	case "doctor":
		return a.cmdDoctor()
	}
	return nil
}
