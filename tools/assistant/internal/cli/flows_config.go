package cli

import (
	"errors"
	"fmt"
	"strings"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The Configs screen of the home menu, the form that edits the global config, and the two places where
// the first install uses a config (the choice before the card step, and the apply after the verify step).

// flowConfigs is the Configs screen: global, local, copy. Every entry runs the same function as the
// matching `akam config` command.
func (a *App) flowConfigs() error {
	a.banner("Configs - global, local, copy")
	g := "none yet"
	if profile.Exists(a.globalPlace(), profile.Settings) {
		g = "saved"
	}
	a.UI.Say("  Global config (" + g + "): the default template, offered when you set up a new camera. A camera keeps its own settings; nothing is synchronised.")
	if s := a.St.Get(keyConfigSource); s != "" {
		a.UI.Say("  Camera " + a.Cams.Name + ": settings: " + sourceText(s))
	}
	items := []string{
		"Show the global config",
		"Save the global config from this camera (asks first)",
		"Edit the global config (no camera needed)",
		"Apply the global config to this camera (shows the differences, asks first)",
		"Copy the settings of another camera to this one (shows the differences, asks first)",
		"Show this camera's local config",
		"Record this camera's settings as its local config",
		"Forget the saved passwords and Wi-Fi...",
		"Back",
	}
	i, err := a.UI.Pick("Configs:", items)
	if err != nil || i == len(items)-1 {
		return nil
	}
	cam := func() bool { return a.needCamera() }
	switch i {
	case 0:
		a.reportFlowError(a.cfgShow(true))
	case 1:
		if cam() {
			a.reportFlowError(a.cfgSaveGlobal(""))
		}
	case 2:
		a.reportFlowError(a.cfgEditGlobal())
	case 3:
		if cam() {
			a.reportFlowError(a.cfgApplyCmd(true, "", nil, false))
		}
	case 4:
		if cam() {
			if from, ok := a.pickOtherCamera("Copy the settings of which camera?"); ok {
				a.reportFlowError(a.cfgApplyCmd(false, from, nil, false))
			}
		}
	case 5:
		a.reportFlowError(a.cfgShow(false))
	case 6:
		if cam() {
			a.reportFlowError(a.cfgSaveLocal())
		}
	case 7:
		j, err := a.UI.Pick("Forget the passwords and Wi-Fi of:", []string{"the global config", "camera " + a.Cams.Name, "Back"})
		if err == nil && j < 2 {
			a.reportFlowError(a.cfgForget(j == 0))
		}
	}
	return nil
}

// pickOtherCamera lists the other cameras of this computer.
func (a *App) pickOtherCamera(title string) (string, bool) {
	var names []string
	for _, n := range a.Cams.Known() {
		if n != a.Cams.Name {
			names = append(names, n)
		}
	}
	if len(names) == 0 {
		a.UI.Say("  No other camera is known on this computer.")
		return "", false
	}
	items := make([]string, 0, len(names)+1)
	for _, n := range names {
		note := "no config saved on this computer"
		if profile.Exists(a.cameraPlace(n), profile.Settings) {
			note = "config saved on this computer"
		}
		items = append(items, fmt.Sprintf("%s  (%s)", n, note))
	}
	items = append(items, "Back")
	i, err := a.UI.Pick(title, items)
	if err != nil || i >= len(names) {
		return "", false
	}
	return names[i], true
}

// ---------------------------------------------------------------- edit the global config

// formKeyChoices are the keys of the global form that are not free text.
func templateRow(k string, f *profile.File) ui.FormRow {
	g := map[string]string{"led": "LED", "picture": "Picture", "video": "Video", "audio": "Audio", "onvif": "Network", "ntp": "Network", "mqtt": "Network", "auth": "Security", "key": "Security"}[profile.GroupOf(k)]
	row := ui.FormRow{ID: k, Group: g, Label: keyLabel(k), Value: f.Get(k)}
	if o, ok := camopt.Find(k); ok {
		row.Note = camopt.Describe(o)
		switch o.Kind {
		case camopt.Enum:
			row.Kind = ui.FormEnum
			row.Choices = append([]string{"not set"}, enumLabels(o)...)
			row.Value = "not set"
			if f.Has(k) {
				row.Value = valueShown(k, f.Get(k))
			}
		default:
			row.Kind = ui.FormText
			row.Note = fmt.Sprintf("%d to %d; empty: not part of the template", o.Min, o.Max)
		}
		return row
	}
	switch k {
	case profile.OnvifEnabled, profile.NTPEnabled, profile.MQTTEnabled, profile.AuthEnabled:
		row.Kind, row.Choices, row.Value = ui.FormEnum, []string{"not set", "off", "on"}, "not set"
		if f.Has(k) {
			row.Value = valueShown(k, f.Get(k))
		}
	case profile.MQTTDiscovery:
		row.Kind, row.Choices, row.Value = ui.FormEnum, []string{"not set", "on", "off"}, "not set"
		if f.Has(k) {
			row.Value = f.Get(k)
		}
	case profile.KeyKind:
		row.Kind, row.Choices, row.Value = ui.FormEnum, []string{"not set", "shared", "own", "custom"}, "not set"
		if f.Has(k) {
			row.Value = f.Get(k)
		}
		row.Note = "shared ~/.ssh/akam, a key of its own ~/.ssh/akam-NAME, or a key file"
	default:
		row.Kind = ui.FormText
		row.Note = "empty: not part of the template"
		if k == profile.NTPServers {
			row.Value = valueShown(k, f.Get(k))
			row.Note = "1 to 3 servers separated by spaces; empty: not part of the template"
		}
	}
	return row
}

// templateKeys are the keys the form lists.
func templateKeys() []string { return cfgKeys() }

// templateSet changes one key of the global template: "not set" or empty removes it; the value is checked
// with the same validators as the options. It returns the new file.
func templateSet(f *profile.File, k, value string) (*profile.File, error) {
	n := f.Clone()
	value = strings.TrimSpace(value)
	if value == "" || value == "not set" {
		n.Unset(k)
		return n, nil
	}
	if o, ok := camopt.Find(k); ok && o.Kind == camopt.Enum {
		value = enumValue(o, value)
	}
	switch k {
	case profile.OnvifEnabled, profile.NTPEnabled, profile.MQTTEnabled, profile.AuthEnabled:
		value = map[string]string{"on": "1", "off": "0"}[value]
	case profile.NTPServers:
		value = strings.Join(strings.Fields(strings.ReplaceAll(value, ",", " ")), ",")
	}
	if err := n.Set(k, value); err != nil {
		return f, err
	}
	return n, nil
}

func (a *App) saveTemplate(f *profile.File) error {
	g := f.Clone()
	g.Unset(profile.SavedAt)
	_ = g.Set(profile.SavedAt, cfgNow())
	return profile.Save(a.globalPlace(), profile.Settings, g)
}

// cfgEditGlobal edits the template without any camera: a form in the session, a list in plain.
func (a *App) cfgEditGlobal() error {
	a.banner("Edit the global config")
	if a.UI.Dry() {
		a.UI.Say("  WOULD show the settings of the template (video, picture, LED, audio, ONVIF, time sync, MQTT, video password, SSH key),")
		a.UI.Say("  save each change at once in " + a.globalPlace().Settings + " (mode 600), and offer to keep passwords and Wi-Fi (default No). No camera is contacted.")
		return nil
	}
	f, err := profile.LoadOrNew(a.globalPlace(), profile.Settings)
	if err != nil {
		return a.UI.Fail(err.Error(), "fix or delete that file.")
	}
	if fk, ok := a.UI.(ui.FormKeyer); ok {
		return a.editGlobalForm(f, fk)
	}
	for {
		keys := templateKeys()
		items := make([]string, 0, len(keys)+2)
		for _, k := range keys {
			items = append(items, fmt.Sprintf("%-26s %s", keyLabel(k), valueShown(k, f.Get(k))))
		}
		items = append(items, "Passwords and Wi-Fi...", "Done")
		i, err := a.UI.Pick("Global config (a '-' means: not part of the template):", items)
		if err != nil || i == len(items)-1 {
			return nil
		}
		if i == len(items)-2 {
			a.templateSecrets()
			continue
		}
		k := keys[i]
		row := templateRow(k, f)
		var v string
		if row.Kind == ui.FormEnum {
			j, err := a.UI.Pick(keyLabel(k)+":", append(append([]string(nil), row.Choices...), "Cancel"))
			if err != nil || j >= len(row.Choices) {
				continue
			}
			v = row.Choices[j]
		} else {
			s, err := a.UI.Prompt(fmt.Sprintf("  %s (%s; '-' removes it; empty cancels):", keyLabel(k), row.Note), "")
			if err != nil || s == "" {
				continue
			}
			if v = s; s == "-" {
				v = ""
			}
		}
		n, err := templateSet(f, k, v)
		if err != nil {
			a.UI.Err(err.Error() + ".")
			continue
		}
		if err := a.saveTemplate(n); err != nil {
			a.UI.Err("could not save: " + err.Error())
			continue
		}
		f = n
		a.UI.Good("  Saved: " + keyLabel(k) + " " + valueShown(k, f.Get(k)))
	}
}

// editGlobalForm is cfgEditGlobal on the session: the Options form on the template. Each change is
// saved at once; s opens the passwords and Wi-Fi questions (their own yes/no), c copies from a camera.
func (a *App) editGlobalForm(f *profile.File, fk ui.FormKeyer) error {
	group := ""
	for {
		var rows []ui.FormRow
		for _, k := range templateKeys() {
			rows = append(rows, templateRow(k, f))
		}
		res, err := fk.FormKeys(ui.FormSpec{
			Title: "Global config - the default template for new cameras (no camera is contacted)",
			Rows:  rows,
			Apply: func(id, value string) (ui.FormRow, error) {
				n, err := templateSet(f, id, value)
				if err != nil {
					a.UI.Err(err.Error() + ".")
					return ui.FormRow{}, err
				}
				if err := a.saveTemplate(n); err != nil {
					a.UI.Err("could not save: " + err.Error())
					return ui.FormRow{}, err
				}
				f = n
				return templateRow(id, f), nil
			},
			Keys:  []ui.FormKey{{Key: "s", Label: "passwords, Wi-Fi"}, {Key: "c", Label: "copy from a camera"}},
			Group: group,
		})
		if err != nil {
			return nil
		}
		group = res.Group
		switch res.Key {
		case "s":
			a.templateSecrets()
		case "c":
			if from, ok := a.pickOtherCamera("Take the settings of which camera as the global config?"); ok {
				a.reportFlowError(a.cfgSaveGlobal(from))
				if n, err := profile.LoadOrNew(a.globalPlace(), profile.Settings); err == nil {
					f = n
				}
			}
		default:
			return nil
		}
	}
}

// templateSecrets is the passwords and Wi-Fi of the template: each one its own opt-in (default No).
func (a *App) templateSecrets() {
	p := a.globalPlace()
	sec, err := profile.LoadOrNew(p, profile.Secrets)
	if err != nil {
		a.UI.Err(err.Error())
		return
	}
	a.UI.Say("  Saved now: " + secretsSummary(sec))
	items := []string{"Keep a video password in the template", "Keep an MQTT password in the template", "Keep the Wi-Fi name and password in the template", "Forget all of them", "Back"}
	i, err := a.UI.Pick("Passwords and Wi-Fi of the global config:", items)
	if err != nil || i == 4 {
		return
	}
	if i == 3 {
		a.reportFlowError(a.cfgForget(true))
		return
	}
	if err := a.UI.RequireTTY("keeping a password"); err != nil {
		return
	}
	what := []string{"the video password", "the MQTT password", "the Wi-Fi name and password"}[i]
	if ok, _ := a.UI.AskYN("  Keep " + what + " in the global config? " + walletWarning); !ok {
		a.UI.Say("  Nothing was saved.")
		return
	}
	switch i {
	case 0:
		if pw, ok := a.walletPassword("video password", "", rtspMinPass); ok {
			sec.Vals[profile.AuthPassword] = pw
		}
	case 1:
		if pw, ok := a.walletPassword("MQTT password", "", 1); ok {
			sec.Vals[profile.MQTTPassword] = pw
		}
	case 2:
		ssid, err := a.UI.Prompt("  Wi-Fi network name (SSID, 2.4 GHz):", sec.Get(profile.WifiSSID))
		if err != nil || strings.TrimSpace(ssid) == "" {
			return
		}
		pw, err := a.UI.Secret("  Wi-Fi password (not shown):")
		if err != nil {
			return
		}
		a.Secrets.Add(pw)
		n := sec.Clone()
		if e := n.Set(profile.WifiSSID, strings.TrimSpace(ssid)); e != nil {
			a.UI.Err(e.Error() + ".")
			return
		}
		if e := n.Set(profile.WifiPassword, pw); e != nil {
			a.UI.Err(e.Error() + ".")
			return
		}
		sec = n
	}
	if err := profile.Save(p, profile.Secrets, sec); err != nil {
		a.UI.Err("could not save: " + err.Error())
		return
	}
	a.UI.Good("  Saved in a private file: " + secretsSummary(sec))
}

// ---------------------------------------------------------------- the first install

// cfgInstallChoice asks, once per camera and before the card step, where the settings of a new camera
// come from: the global config, another camera's, or the camera's own defaults. It asks nothing when
// there is nothing to choose from, or when the choice was already made. The choice is only recorded
// (config_source, config_pending); the Wi-Fi and the key feed the card step, the settings are applied
// after the verify step (cfgApplyPending), with the differences shown and a yes/no like any apply.
func (a *App) cfgInstallChoice() {
	if a.UI.Dry() || a.St.Get(keyConfigSource) != "" {
		return
	}
	type opt struct{ label, src string }
	var opts []opt
	if profile.Exists(a.globalPlace(), profile.Settings) {
		opts = append(opts, opt{"Global config (your default template)", "global"})
	}
	for _, n := range a.Cams.Known() {
		if n != a.Cams.Name && profile.Exists(a.cameraPlace(n), profile.Settings) {
			opts = append(opts, opt{"Copy from camera " + n, "camera:" + n})
		}
	}
	if len(opts) == 0 {
		return
	}
	items := make([]string, 0, len(opts)+1)
	for _, o := range opts {
		items = append(items, o.label)
	}
	items = append(items, "Keep camera defaults (no config)")
	a.UI.Blank()
	a.UI.Say("  You have saved configs. The Wi-Fi and the SSH key of the one you pick are offered for the card, and its settings are applied")
	a.UI.Say("  after the camera is verified (you see the differences first and confirm; nothing is applied silently).")
	i, err := a.UI.Pick("Use a config for this camera?", items)
	if err != nil {
		return
	}
	if i >= len(opts) {
		a.setState(keyConfigSource, "local")
		return
	}
	a.setState(keyConfigSource, opts[i].src)
	a.setState(keyConfigPending, "1")
	a.UI.Say("  Noted: " + opts[i].label + ".")
}

// pendingFiles are the files of the config chosen at the first install (nil when none or already applied).
func (a *App) pendingFiles() (set, sec *profile.File) {
	if a.St.Get(keyConfigPending) != "1" {
		return nil, nil
	}
	src := a.St.Get(keyConfigSource)
	var p profile.Place
	switch {
	case src == "global":
		p = a.globalPlace()
	case strings.HasPrefix(src, "camera:"):
		p = a.cameraPlace(strings.TrimPrefix(src, "camera:"))
	default:
		return nil, nil
	}
	set, sec, err := loadPlace(p)
	if err != nil {
		return nil, nil
	}
	return set, sec
}

// cfgWifiOffer offers the Wi-Fi saved in the chosen config for the card (one yes/no, default No).
func (a *App) cfgWifiOffer() (ssid, pass string, ok bool) {
	_, sec := a.pendingFiles()
	if sec == nil || !sec.Has(profile.WifiPassword) || !sec.Has(profile.WifiSSID) {
		return "", "", false
	}
	ssid, pass = sec.Get(profile.WifiSSID), sec.Get(profile.WifiPassword)
	a.Secrets.Add(pass)
	if yes, _ := a.UI.AskYN(fmt.Sprintf("  Use the Wi-Fi saved in your config (network \"%s\") for this card?", ui.Sanitize(ssid))); !yes {
		return "", "", false
	}
	a.UI.Say("  Using the saved Wi-Fi: the password goes on the card only (never on a command line).")
	return ssid, pass, true
}

// cfgKeyOffer offers the SSH key choice of the chosen config for the card (one yes/no, default No).
func (a *App) cfgKeyOffer() (string, bool) {
	set, _ := a.pendingFiles()
	if set == nil || !set.Has(profile.KeyKind) {
		return "", false
	}
	var pub, what string
	switch k := set.Get(profile.KeyKind); k {
	case camssh.KindShared:
		pub, what = camssh.SharedKeyPath()+".pub", "the shared key "+tilde(camssh.SharedKeyPath())
	case camssh.KindOwn:
		pub, what = camssh.OwnKeyPath(a.Cams.Name)+".pub", "a key for this camera only ("+tilde(camssh.OwnKeyPath(a.Cams.Name))+")"
	default:
		pub = set.Get(profile.KeyPath)
		if pub == "" || !exists(pub) {
			a.UI.Say("  Your config names a custom key file that is not on this computer: choose a key below.")
			return "", false
		}
		what = "the key " + tilde(strings.TrimSuffix(pub, ".pub"))
	}
	if yes, _ := a.UI.AskYN("  Your config says this camera uses " + what + ". Use it?"); !yes {
		return "", false
	}
	if set.Get(profile.KeyKind) != camssh.KindCustom {
		if _, err := a.keyEnsure(pub, a.keyCommentFor(set.Get(profile.KeyKind)), true); err != nil {
			return "", false
		}
	}
	return pub, true
}

// cfgApplyPending applies the config chosen at the first install, after the verify step. It never fails
// the step: the differences are shown, the user confirms (default No), and a refusal leaves it pending.
func (a *App) cfgApplyPending() {
	if a.St.Get(keyConfigPending) != "1" || a.UI.Dry() {
		return
	}
	src := a.St.Get(keyConfigSource)
	a.UI.Blank()
	a.UI.Bold("The config you chose for this camera")
	var cs *cfgSrc
	var err error
	if src == "global" {
		cs, err = a.globalSource()
	} else if n, ok := strings.CutPrefix(src, "camera:"); ok {
		cs, err = a.cameraSource(n)
	}
	if err != nil || cs == nil {
		a.UI.Say("  The saved config cannot be read now; apply it later: " + Prog + " config apply --global | --from CAMERA")
		return
	}
	if err := a.cfgApplySource(cs, nil, false); err != nil && !errors.Is(err, ui.ErrFailed) {
		a.reportFlowError(err)
	}
	if a.St.Get(keyConfigPending) == "1" {
		a.UI.Say("  Not applied yet. When you want it: " + Prog + " config apply " + map[bool]string{true: "--global", false: "--from " + strings.TrimPrefix(src, "camera:")}[src == "global"])
	}
}
