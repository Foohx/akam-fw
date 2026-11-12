package cli

import (
	"errors"
	"fmt"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The option screen of the session (spec 12, 4.4): one form, the groups side by side, every change sent
// at once through the one audited path (camopt.Apply of one Change, the same validation and log lines
// as the plain screens). Plain keeps groupScreen and its "N pending, Apply, Discard".

// formGroups are the groups of the form, in order, with the camopt group they show ("" for the two
// that go through helper scripts of the camera).
var formGroups = []struct{ name, camopt string }{
	{"Video", "Video"}, {"Picture", "Picture"}, {"LED", "LED and night"}, {"Audio", "Audio"},
	{"Network", ""}, {"Security", ""},
}

const restartNote = "Saved. Applies when the video service restarts: r restarts it (about 20 s)."
const powerNote = "Saved. Applies at the next power cycle: unplug the USB-C power, wait 5 s, plug it back (never done for you)."

// optionRow is the form row of one control-port option as the camera reports it.
func optionRow(o camopt.Option, st camopt.OptionState, group string) ui.FormRow {
	r := ui.FormRow{ID: o.ID, Group: group, Label: o.Label, Value: valueText(st), Note: camopt.Describe(o), Detail: o.Help + "\n" + camopt.Describe(o)}
	switch o.Kind {
	case camopt.Enum:
		r.Kind, r.Choices = ui.FormEnum, enumLabels(o)
	case camopt.Int:
		r.Kind, r.Min, r.Max, r.Value = ui.FormInt, o.Min, o.Max, st.Value
	default:
		r.Kind = ui.FormReadonly
	}
	if st.Unavailable {
		r.Unavailable, r.Value = true, "n/a"
		return r
	}
	r.NotSaved = st.Persisted == camopt.PersistRAM
	return r
}

// optionRows are all the rows of the form for one snapshot.
func (a *App) optionRows(snap optSnap) []ui.FormRow {
	var rows []ui.FormRow
	pending := snap.vals["video.cfg_pending"].Value == "1"
	for _, g := range formGroups {
		if g.camopt != "" {
			for _, o := range optionGroupOptions(g.camopt) {
				st := snap.vals[o.ID]
				st.Option = o
				r := optionRow(o, st, g.name)
				if o.Applies == camopt.VideoRestart && pending && !r.Unavailable {
					r.Note = restartNote
				}
				rows = append(rows, r)
			}
			continue
		}
		ro := func(label, val, note string) {
			rows = append(rows, ui.FormRow{ID: "ro." + g.name + "." + label, Group: g.name, Label: label, Kind: ui.FormReadonly, Value: val, Note: note})
		}
		if g.name == "Network" {
			ssid := "n/a"
			if st, ok := snap.vals["net.ssid"]; ok && !st.Unavailable {
				ssid = ui.Sanitize(st.Value)
			}
			ro("ONVIF", onOff(snap.onvifOn), "n changes the network settings")
			ro("Time sync (NTP)", snap.ntp, "n changes the network settings")
			ro("Name", orDash(snap.host), "n changes the network settings (applies at the next power cycle)")
			ro("Wi-Fi", ssid, "read-only: changing it needs the flash card")
			ro("MQTT / Home Assistant", snap.mqtt, "n changes the network settings")
		} else {
			t := map[string]string{"enabled": "on", "disabled": "off"}[snap.rtsp]
			if t == "" {
				t = "unknown"
			}
			ro("Video password", t, "p sets, changes or removes it (removing asks first)")
		}
	}
	return rows
}

// applyOne is one change from the form: the SET line is shown before it is sent, and the answer is
// the new row or the refusal (the form snaps the row back and shows it).
func (a *App) applyOne(c *camssh.Client, id, value string) (ui.FormRow, error) {
	o, ok := camopt.Find(id)
	if !ok {
		return ui.FormRow{}, errors.New("unknown option")
	}
	if o.Kind == camopt.Enum {
		value = enumValue(o, value)
	}
	v, err := camopt.Validate(id, value)
	if err != nil {
		a.UI.Err(err.Error() + ".")
		return ui.FormRow{}, err
	}
	a.UI.Say(fmt.Sprintf("SET %s %s", o.Key, v))
	res, err := camopt.Apply(a.exchanger(c), []camopt.Change{{ID: id, Value: v}})
	if err != nil {
		a.UI.Err(err.Error())
		return ui.FormRow{}, err
	}
	r := res[0]
	switch {
	case r.OK:
	case r.Answered:
		a.UI.Err(fmt.Sprintf("the camera refused %s %s: %s (this image may not have that setting; update it)", o.Key, r.Value, ui.Sanitize(r.Err)))
		return ui.FormRow{}, errors.New("refused: " + ui.Sanitize(r.Err))
	default:
		a.UI.Err(fmt.Sprintf("the camera did not confirm %s %s: its control port may be busy; try again.", o.Key, r.Value))
		return ui.FormRow{}, errors.New("not confirmed: control port busy")
	}
	st := camopt.OptionState{Option: o, Value: r.Value}
	if r.NotSaved {
		a.UI.Warn(fmt.Sprintf("%s is %s now, but the camera could not save it: it will be lost at the next reboot.", o.Label, valueText(st)))
	} else {
		a.UI.Good(fmt.Sprintf("  %s: %s (%s).", o.Label, valueText(st), camopt.Describe(o)))
	}
	row := optionRow(o, st, groupName(o))
	row.NotSaved = r.NotSaved
	switch {
	case r.Restart || o.Applies == camopt.VideoRestart:
		row.Note = restartNote
	case o.Applies == camopt.NextPowerCycle:
		row.Note = powerNote
	}
	return row, nil
}

func groupName(o camopt.Option) string {
	for _, g := range formGroups {
		if g.camopt == o.Group {
			return g.name
		}
	}
	return o.Group
}

// optionsForm is flowOptions on the session: the form, and the one-letter actions (r restarts the video
// service, t sets the clock when it is off, n and p open the network and password screens). Each of
// them asks its own question, default No; none is asked unless the key is pressed.
func (a *App) optionsForm(c *camssh.Client, ip string, fk ui.FormKeyer) error {
	group := ""
	for {
		snap := a.readOptions(c)
		a.noteHealth(snap.health)
		_, offer := clockReport(snap.health)
		keys := []ui.FormKey{{Key: "r", Label: "restart video"}, {Key: "n", Label: "network"}, {Key: "p", Label: "password"}}
		if offer {
			keys = append(keys, ui.FormKey{Key: "t", Label: "set clock"})
		}
		res, err := fk.FormKeys(ui.FormSpec{
			Title: fmt.Sprintf("Camera options - %s (%s)%s", a.Cams.Name, ip, fwSuffix(snap.fw)),
			Rows:  a.optionRows(snap),
			Apply: func(id, value string) (ui.FormRow, error) { return a.applyOne(c, id, value) },
			Keys:  keys,
			Group: group,
		})
		if err != nil {
			return nil
		}
		group = res.Group
		switch res.Key {
		case "r":
			if a.restartVideo(c) {
				a.UI.Good(fmt.Sprintf("  The video service was asked to restart; it is back in about 20 seconds (%s video show).", Prog))
			} else {
				a.UI.Say("  Not restarted: the new values apply at the next restart of the video service or the next reboot.")
			}
		case "t":
			if offer {
				a.clockAsk(c, snap.health)
			}
		case "n":
			a.networkScreen(c, snap)
		case "p":
			a.securityScreen(snap)
		default:
			return nil
		}
	}
}
