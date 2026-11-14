package cli

import (
	"errors"
	"fmt"
	"sort"
	"strings"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// akam config [show | save-global | save-local | apply | edit-global | forget-secrets]: the settings of a
// camera saved on this computer (configcore.go has the model). Every question defaults to No, every list
// masks the secrets, and nothing is sent to a camera except by `apply` after its own yes/no.

const walletWarning = "They are stored on this computer in a private file; anyone with access to your account can read it."

var configUsage = "akam: config needs one of: show [--global] | save-global [--from CAMERA] | save-local | apply (--global | --from CAMERA) [--only GROUPS] [--include-secrets] | edit-global | forget-secrets [--global]"

func (a *App) cmdConfig(args []string) error {
	sub := "show"
	if len(args) > 0 {
		sub = args[0]
	}
	if len(args) > 1 {
		return usageErr(configUsage)
	}
	o := a.Opt
	bad := func(msg string) error {
		return usageErr("akam: config " + sub + ": " + msg + " (see " + Prog + " help)")
	}
	if o.CfgGlobal && o.Camera != "" && (sub == "show" || sub == "forget-secrets") {
		return bad("--global and --camera cannot be used together")
	}
	if o.CfgSecrets && sub != "apply" {
		return bad("--include-secrets is for apply")
	}
	if (o.CfgOnly != "") && sub != "apply" {
		return bad("--only is for apply")
	}
	if o.CfgFrom != "" && sub != "apply" && sub != "save-global" {
		return bad("--from is for apply and save-global")
	}
	if o.CfgGlobal && sub != "show" && sub != "apply" && sub != "forget-secrets" {
		return bad("--global does not apply here")
	}
	switch sub {
	case "show":
		return a.cfgShow(o.CfgGlobal)
	case "save-global":
		return a.cfgSaveGlobal(o.CfgFrom)
	case "save-local":
		return a.cfgSaveLocal()
	case "apply":
		only, err := profile.ParseOnly(o.CfgOnly)
		if err != nil {
			return usageErr("akam: config apply: " + err.Error())
		}
		if o.CfgGlobal == (o.CfgFrom != "") {
			return bad("give exactly one of --global or --from CAMERA")
		}
		return a.cfgApplyCmd(o.CfgGlobal, o.CfgFrom, only, o.CfgSecrets)
	case "edit-global":
		return a.cfgEditGlobal()
	case "forget-secrets":
		return a.cfgForget(o.CfgGlobal)
	}
	return usageErr(configUsage)
}

// ---------------------------------------------------------------- show

// secretsSummary says what the secrets file holds, never a value.
func secretsSummary(sec *profile.File) string {
	yn := func(k string) string {
		if sec.Has(k) {
			return "set"
		}
		return "not set"
	}
	w := "not set"
	if sec.Has(profile.WifiPassword) {
		w = "set" // the network name is never printed or logged either
	}
	return fmt.Sprintf("video password: %s   MQTT password: %s   Wi-Fi: %s", yn(profile.AuthPassword), yn(profile.MQTTPassword), w)
}

// configLines lists a settings file by group, then the secrets summary (the secrets as set / not set).
func configLines(set, sec *profile.File) []string {
	var out []string
	last := ""
	for _, k := range cfgKeys() {
		if !set.Has(k) {
			continue
		}
		g := profile.GroupOf(k)
		if g != last {
			out = append(out, "  "+groupTitle[g])
			last = g
		}
		out = append(out, fmt.Sprintf("    %-26s %s", keyLabel(k), valueShown(k, set.Get(k))))
	}
	if len(out) == 0 {
		out = append(out, "  (no setting in it)")
	}
	return append(out, "  Secrets: "+secretsSummary(sec))
}

func (a *App) cfgShow(global bool) error {
	p, title := a.cameraPlace(a.Cams.Name), "Local config of camera "+a.Cams.Name
	if global {
		p, title = a.globalPlace(), "Global config (the default template for new cameras)"
	}
	a.UI.Bold(title)
	set, err := profile.Load(p, profile.Settings)
	if errors.Is(err, profile.ErrNone) {
		if global {
			a.UI.Say("  None saved yet. Save one from a camera: " + Prog + " config save-global [--from CAMERA]")
		} else {
			a.UI.Say("  Nothing recorded for this camera yet. Record it: " + Prog + " config save-local (or apply a config to it).")
			a.showSource()
		}
		return nil
	}
	if err != nil {
		return a.UI.Fail(err.Error(), "fix or delete that file.")
	}
	sec, err := profile.LoadOrNew(p, profile.Secrets)
	if err != nil {
		return a.UI.Fail(err.Error(), "fix that file, or delete it with "+Prog+" config forget-secrets.")
	}
	a.UI.Say("  File: " + p.Settings + " (private)")
	if s := savedStamp(set); s != "" {
		a.UI.Say("  " + strings.ToUpper(s[:1]) + s[1:])
	}
	if !global {
		a.showSource()
	}
	for _, l := range configLines(set, sec) {
		a.UI.Say(l)
	}
	if len(set.Extra)+len(sec.Extra) > 0 {
		a.UI.Say(fmt.Sprintf("  %d key(s) of a newer akam are kept in the file and ignored.", len(set.Extra)+len(sec.Extra)))
	}
	return nil
}

func (a *App) showSource() {
	if s := a.St.Get(keyConfigSource); s != "" {
		a.UI.Say("  Settings of this camera: " + sourceText(s))
	}
}

// sourceText is "local (from global)" for config_source=global, "local (from camera kitchen)", or "local".
func sourceText(s string) string {
	switch {
	case s == "global":
		return "local (from global)"
	case strings.HasPrefix(s, "camera:"):
		return "local (from camera " + strings.TrimPrefix(s, "camera:") + ")"
	case s == "local":
		return "local"
	}
	return ""
}

// ---------------------------------------------------------------- saving

// askWallet asks the two opt-in questions (passwords, Wi-Fi) and returns the secrets to keep. have holds what the
// source already knows (its saved secrets). A saved password is offered again only while it still belongs to the
// settings being saved (the same video user, the same broker and user); a yes then offers to keep it or to type a
// new one, never reuses it silently. Nothing is asked without a terminal. defSSID is the network the camera
// reports (the default of the Wi-Fi name).
func (a *App) askWallet(set, have *profile.File, defSSID, where string) (*profile.File, bool) {
	out := profile.New(profile.Secrets)
	if a.UI.Dry() || !a.UI.Interactive() {
		a.UI.Say("  No terminal: passwords and Wi-Fi are not saved (they are only ever saved when you answer yes).")
		return out, false
	}
	any := false
	authUser := set.Get(profile.AuthUser)
	mqttHost, mqttUser := set.Get(profile.MQTTHost), set.Get(profile.MQTTUser)
	needAuth := set.Get(profile.AuthEnabled) == "1" && authUser != ""
	needMqtt := set.Get(profile.MQTTEnabled) == "1" && mqttUser != "" && mqttHost != ""
	if needAuth || needMqtt {
		a.UI.Say("  The camera cannot tell its passwords: to keep them in " + where + " you type them here.")
		if ok, _ := a.UI.AskYN("  Also keep the video and MQTT passwords in " + where + "? " + walletWarning); ok {
			if needAuth {
				if pw, ok := a.walletPassword("video password", "user "+authUser, profile.BoundAuth(have, authUser), have.Has(profile.AuthPassword), rtspMinPass); ok {
					out.Vals[profile.AuthPassword], out.Vals[profile.AuthPasswordUser] = pw, authUser
					any = true
				}
			}
			if needMqtt {
				if pw, ok := a.walletPassword("MQTT password", "broker "+mqttHost+", user "+mqttUser, profile.BoundMQTT(have, mqttHost, mqttUser), have.Has(profile.MQTTPassword), 1); ok {
					out.Vals[profile.MQTTPassword], out.Vals[profile.MQTTPasswordHost], out.Vals[profile.MQTTPasswordUser] = pw, mqttHost, mqttUser
					any = true
				}
			}
		}
	}
	a.UI.Say("  The Wi-Fi name and password let you prepare the card of a new camera without typing them again.")
	if ok, _ := a.UI.AskYN("  Also keep the Wi-Fi name and password in " + where + "? " + walletWarning); ok {
		ssid, pw := have.Get(profile.WifiSSID), have.Get(profile.WifiPassword)
		if ssid == "" {
			s, err := a.UI.Prompt("  Wi-Fi network name (SSID, 2.4 GHz):", defSSID)
			if err == nil {
				ssid = strings.TrimSpace(s)
			}
		}
		if pw == "" && ssid != "" {
			p, err := a.UI.Secret("  Wi-Fi password (not shown):")
			if err == nil {
				pw = p
			}
		}
		a.Secrets.Add(pw)
		if err := out.Set(profile.WifiSSID, ssid); err != nil {
			a.UI.Warn("Wi-Fi not kept: " + err.Error() + ".")
		} else if err := out.Set(profile.WifiPassword, pw); err != nil {
			delete(out.Vals, profile.WifiSSID)
			a.UI.Warn("Wi-Fi not kept: " + err.Error() + ".")
		} else {
			any = true
		}
	}
	return out, any
}

// walletPassword returns the password to keep. bound is the saved one when it belongs to these settings (for is
// what it belongs to, in words); present says a saved one exists that does not belong to them. A usable saved
// one is offered (default No); No, or none, means type a new one (twice, not shown).
func (a *App) walletPassword(what, for_, bound string, present bool, min int) (string, bool) {
	title := strings.ToUpper(what[:1]) + what[1:]
	if bound != "" {
		a.Secrets.Add(bound)
		if ok, _ := a.UI.AskYN("  A " + what + " saved for " + for_ + " is available. Keep it? (No: type a new one)"); ok {
			return bound, true
		}
	} else if present {
		a.UI.Say("  The " + what + " saved earlier was for other settings: it is not reused.")
	}
	pw, err := a.UI.Secret(fmt.Sprintf("  %s (not shown, empty keeps none):", title))
	if err != nil || pw == "" {
		return "", false
	}
	if p := pwProblem(pw, min); p != "" {
		a.UI.Warn(what + " not kept: " + p + ".")
		return "", false
	}
	pw2, err := a.UI.Secret("  Type it again:")
	if err != nil || pw != pw2 {
		a.UI.Warn(what + " not kept: the two entries differ.")
		return "", false
	}
	a.Secrets.Add(pw)
	return pw, true
}

// pruneStale drops the saved passwords that no longer belong to the settings next to them (another video
// user, another broker or user). It reports what it dropped.
func pruneStale(sec, set *profile.File) (dropped []string) {
	drop := func(label string, keys ...string) {
		for _, k := range keys {
			sec.Unset(k)
		}
		dropped = append(dropped, label)
	}
	if sec.Has(profile.AuthPassword) && profile.BoundAuth(sec, set.Get(profile.AuthUser)) == "" {
		drop("video password", profile.AuthPassword, profile.AuthPasswordUser)
	}
	if sec.Has(profile.MQTTPassword) && profile.BoundMQTT(sec, set.Get(profile.MQTTHost), set.Get(profile.MQTTUser)) == "" {
		drop("MQTT password", profile.MQTTPassword, profile.MQTTPasswordHost, profile.MQTTPasswordUser)
	}
	return dropped
}

// secretsAfterSet is called when a password was just set on the current camera: a copy saved for this camera
// on this computer is now stale. With ask (a command, outside any panel) the user may keep it up to date
// (default No); otherwise, and on No, the stale copy is removed. pw "" only removes.
func (a *App) secretsAfterSet(key, host, user, pw string, ask bool) {
	if a.UI.Dry() {
		return
	}
	p := a.cameraPlace(a.Cams.Name)
	sec, err := profile.LoadOrNew(p, profile.Secrets)
	if err != nil || !sec.Has(key) {
		return
	}
	what := map[string]string{profile.AuthPassword: "video", profile.MQTTPassword: "MQTT"}[key]
	if pw != "" && sec.Get(key) == pw {
		// the saved copy is the password that was just set: nothing is out of date; only its binding may move
		if key == profile.AuthPassword {
			sec.Vals[profile.AuthPasswordUser] = user
		} else {
			sec.Vals[profile.MQTTPasswordHost], sec.Vals[profile.MQTTPasswordUser] = host, user
		}
		_ = profile.Save(p, profile.Secrets, sec)
		return
	}
	if ask && pw != "" && a.UI.Interactive() {
		if ok, _ := a.UI.AskYN("  A " + what + " password is saved for this camera on this computer. Update it to the new one? " + walletWarning); ok {
			if key == profile.AuthPassword {
				sec.Vals[key], sec.Vals[profile.AuthPasswordUser] = pw, user
			} else {
				sec.Vals[key], sec.Vals[profile.MQTTPasswordHost], sec.Vals[profile.MQTTPasswordUser] = pw, host, user
			}
			if profile.Save(p, profile.Secrets, sec) == nil {
				a.UI.Say("  Updated.")
			}
			return
		}
	}
	for _, k := range map[string][]string{
		profile.AuthPassword: {profile.AuthPassword, profile.AuthPasswordUser},
		profile.MQTTPassword: {profile.MQTTPassword, profile.MQTTPasswordHost, profile.MQTTPasswordUser},
	}[key] {
		sec.Unset(k)
	}
	if profile.Save(p, profile.Secrets, sec) == nil {
		a.UI.Say("  The " + what + " password saved for this camera on this computer was out of date: removed.")
	}
}

// cfgSaveTo writes src as the config of place p (settings; then the secrets the user opted in to).
func (a *App) cfgSaveTo(p profile.Place, src *cfgSrc, ssid, where string) error {
	set := src.set.Clone()
	set.Unset(profile.SavedFrom)
	set.Unset(profile.SavedAt)
	_ = set.Set(profile.SavedAt, cfgNow())
	if name, ok := strings.CutPrefix(src.label, "camera:"); ok {
		_ = set.Set(profile.SavedFrom, "camera "+name)
	}
	if err := profile.Save(p, profile.Settings, set); err != nil {
		return a.UI.Fail("could not save the config: "+err.Error(), "nothing was changed.")
	}
	a.UI.Good(fmt.Sprintf("  Saved %s (%d setting(s)): %s", where, countSettings(set), p.Settings))
	oldSec, err := profile.LoadOrNew(p, profile.Secrets)
	if err != nil {
		a.UI.Warn(err.Error() + " - the secrets file was left alone.")
		return nil
	}
	// a password saved for other settings than the ones just saved must not stay next to them
	pruned := pruneStale(oldSec, set)
	got, any := a.askWallet(set, src.sec, ssid, where)
	for k, v := range got.Vals {
		oldSec.Vals[k] = v
	}
	if !any && len(pruned) == 0 {
		if oldSec.Empty() {
			a.UI.Say("  No password and no Wi-Fi saved.")
		} else {
			a.UI.Say("  The secrets already saved there were left as they were (" + Prog + " config forget-secrets deletes them).")
		}
		return nil
	}
	if err := profile.Save(p, profile.Secrets, oldSec); err != nil {
		return a.UI.Fail("could not save the secrets: "+err.Error(), "they were not saved.")
	}
	if len(pruned) > 0 {
		a.UI.Say("  Removed the saved " + strings.Join(pruned, " and ") + ": it no longer matches these settings.")
	}
	if any {
		a.UI.Good("  Secrets saved in a private file (" + secretsSummary(oldSec) + ").")
	}
	return nil
}

func countSettings(f *profile.File) int {
	n := 0
	for k := range f.Vals {
		if profile.GroupOf(k) != "" {
			n++
		}
	}
	return n
}

// cfgCurrentLive reads the current camera over SSH (one read-only session).
func (a *App) cfgCurrentLive() (*cfgSrc, string, error) {
	ip, err := a.camNeedIP()
	if err != nil {
		return nil, "", err
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return nil, "", err
	}
	defer c.Close()
	r := a.cfgReadLive(c, a.Cams.Name)
	if !r.ctlOK {
		return nil, "", a.UI.Fail("the camera's control port did not answer, so its settings cannot be read.",
			"nothing was recorded; wait for the camera (a video restart takes a few seconds) and try again.")
	}
	a.reportMissing(r)
	sec, _ := profile.LoadOrNew(a.cameraPlace(a.Cams.Name), profile.Secrets)
	if sec == nil {
		sec = profile.New(profile.Secrets)
	}
	return &cfgSrc{label: "camera:" + a.Cams.Name, desc: "camera " + a.Cams.Name, set: r.f, sec: sec, missing: r.missing}, r.ssid, nil
}

func (a *App) reportMissing(r cfgRead) {
	if len(r.missing) > 0 {
		a.UI.Say("  Could not be read from this camera: " + strings.Join(r.missing, ", ") + " (an older image or a helper that did not answer). They are left out, not guessed.")
	}
}

func (a *App) cfgSaveGlobal(from string) error {
	a.banner("Save the global config")
	if from == a.Cams.Name {
		from = ""
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD read the settings of the camera (read-only, over SSH; or camera " + orDash(from) + " when --from is given),")
		a.UI.Say("  ask: Replace the global config? [y/N] (default No) when there is one, write " + a.globalPlace().Settings + " (mode 600),")
		a.UI.Say("  then ask separately (default No) whether to keep the passwords, and whether to keep the Wi-Fi name and password.")
		return nil
	}
	var src *cfgSrc
	var err error
	name := from
	if name == "" {
		name = a.Cams.Name // live when it answers, else what was last saved for it (it says which)
	} else if !state.NameExists(from) || !state.CameraExists(a.Cams.Base, from) {
		return a.UI.Fail("no camera called '"+from+"' on this computer.", "see "+Prog+" cameras.")
	}
	if src, err = a.cameraSource(name); err != nil {
		return err
	}
	ssid := src.ssid
	if profile.Exists(a.globalPlace(), profile.Settings) {
		if err := a.UI.RequireTTY("replacing the global config"); err != nil {
			return err
		}
		q := "  A global config already exists. Replace it with the settings of " + src.desc + "?"
		if len(src.missing) > 0 {
			q = "  A global config already exists. Replace it with the settings of " + src.desc + ", WITHOUT " + strings.Join(src.missing, ", ") + " (not read)?"
		}
		if ok, _ := a.UI.AskYN(q); !ok {
			a.UI.Say("  Left as it was.")
			return nil
		}
	}
	return a.cfgSaveTo(a.globalPlace(), src, ssid, "the global config")
}

func (a *App) cfgSaveLocal() error {
	a.banner("Record the config of camera " + a.Cams.Name)
	if a.UI.Dry() {
		a.UI.Say("  WOULD read the settings of the camera (read-only, over SSH) and write " + a.cameraPlace(a.Cams.Name).Settings + " (mode 600),")
		a.UI.Say("  then ask separately (default No) whether to keep the passwords and the Wi-Fi next to it.")
		return nil
	}
	src, ssid, err := a.cfgCurrentLive()
	if err != nil {
		return err
	}
	if err := a.cfgSaveTo(a.cameraPlace(a.Cams.Name), src, ssid, "the config of camera "+a.Cams.Name); err != nil {
		return err
	}
	if a.St.Get(keyConfigSource) == "" {
		a.setState(keyConfigSource, "local")
	}
	return nil
}

// ---------------------------------------------------------------- forget

func (a *App) cfgForget(global bool) error {
	p, where := a.cameraPlace(a.Cams.Name), "camera "+a.Cams.Name
	if global {
		p, where = a.globalPlace(), "the global config"
	}
	a.banner("Forget the saved secrets of " + where)
	sec, err := profile.LoadOrNew(p, profile.Secrets)
	if err != nil && !profile.Exists(p, profile.Secrets) {
		return a.UI.Fail(err.Error(), "")
	}
	if !profile.Exists(p, profile.Secrets) {
		a.UI.Say("  Nothing to forget: no password or Wi-Fi is saved for " + where + ".")
		return nil
	}
	if err == nil {
		a.UI.Say("  Saved for " + where + ": " + secretsSummary(sec))
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask: Delete them from this computer? [y/N] (default No), then delete " + p.Secrets + ". The camera is not touched; the settings stay.")
		return nil
	}
	if err := a.UI.RequireTTY("deleting the saved secrets"); err != nil {
		return err
	}
	if ok, _ := a.UI.AskYN("  Delete the saved passwords and Wi-Fi of " + where + " from this computer? The settings stay and the camera is not touched."); !ok {
		a.UI.Say("  Left as it was.")
		return nil
	}
	if _, err := profile.Forget(p); err != nil {
		return a.UI.Fail("could not delete "+p.Secrets+": "+err.Error(), "")
	}
	a.UI.Good("  Deleted: no password and no Wi-Fi of " + where + " is kept on this computer any more.")
	return nil
}

// ---------------------------------------------------------------- apply

func (a *App) cfgApplyCmd(global bool, from string, only map[string]bool, withSecrets bool) error {
	a.banner("Apply a config to camera " + a.Cams.Name)
	if from == a.Cams.Name {
		return usageErr("akam: config apply: --from needs ANOTHER camera than the current one (" + a.Cams.Name + ")")
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD read the config (" + map[bool]string{true: "the global one", false: "camera " + from}[global] + "), then read the camera's settings (read-only, over SSH),")
		a.UI.Say("  show the differences (passwords masked), ask: Apply these changes? [y/N] (default No), and only then send them")
		a.UI.Say("  the same way the option screens do (control-port SET lines, the helper scripts); the video restart is asked separately (default No).")
		a.UI.Say("  Wi-Fi is never applied to a running camera: it is only used to prepare a card.")
		return nil
	}
	var src *cfgSrc
	var err error
	switch {
	case global:
		src, err = a.globalSource()
		if errors.Is(err, profile.ErrNone) {
			return a.UI.Fail("there is no global config yet.", "save one from a camera: "+Prog+" config save-global")
		}
		if err != nil {
			return a.UI.Fail(err.Error(), "fix or delete that file.")
		}
	default:
		if !state.NameExists(from) || !state.CameraExists(a.Cams.Base, from) {
			return a.UI.Fail("no camera called '"+from+"' on this computer.", "see "+Prog+" cameras.")
		}
		if src, err = a.cameraSource(from); err != nil {
			return err
		}
	}
	return a.cfgApplySource(src, only, withSecrets)
}

// cfgApplySource is the whole apply: read the camera, show the difference, ask, send, report, record.
func (a *App) cfgApplySource(src *cfgSrc, only map[string]bool, withSecrets bool) error {
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	c, err := a.connectTools(ip)
	if err != nil {
		return err
	}
	defer c.Close()
	r := a.cfgReadLive(c, a.Cams.Name)
	if !r.ctlOK {
		return a.UI.Fail("the camera's control port did not answer, so its settings cannot be compared.",
			"nothing was sent; wait a few seconds (a video restart takes the port down for a moment) and try again.")
	}
	a.reportMissing(r)
	ch, skipped := cfgDiff(r.f, src.set, src.sec, only, withSecrets)
	a.UI.Say(fmt.Sprintf("  Config: %s. Camera: %s (settings read just now).", src.desc, a.Cams.Name))
	for _, s := range skipped {
		a.UI.Say("  Not applied: " + s + ".")
	}
	if len(ch) == 0 {
		a.UI.Good("  Nothing to change: the camera already has these settings" + onlyNote(only) + ".")
		a.cfgSettle(c, src, len(skipped) == 0, only, &r)
		return nil
	}
	a.UI.Say(fmt.Sprintf("  %d change(s) from the camera's current settings:", len(ch)))
	for _, l := range cfgDiffLines(ch) {
		a.UI.Say(l)
	}
	if !withSecrets {
		a.UI.Say("  Passwords are sent only with the setting that needs them (they cannot be read back from the camera: --include-secrets sends them again).")
	}
	if err := a.UI.RequireTTY("applying a config"); err != nil {
		return err
	}
	if ok, _ := a.UI.AskYN(fmt.Sprintf("  Apply these %d change(s) to camera %s?", len(ch), a.Cams.Name)); !ok {
		a.UI.Say("  Nothing was sent.")
		return nil
	}
	if a.stopRequested() {
		return a.stoppedRead()
	}
	// the changes that carry their own warning are confirmed one by one, before anything is sent
	var go2 []cfgChange
	var declined []cfgOutcome
	for _, x := range ch {
		if x.Own != "" {
			ok, _ := a.UI.Danger(x.Label+": "+x.From+" -> "+x.To, "Apply this change to camera "+a.Cams.Name+"?", x.Own)
			if !ok {
				declined = append(declined, cfgOutcome{ch: x, state: "skipped", note: "not confirmed"})
				continue
			}
		}
		go2 = append(go2, x)
	}
	go2, pws, noPw := a.cfgPasswords(go2, src.sec)
	// the options of a broker that could not be saved (no password to send) have nothing to attach to
	for _, x := range noPw {
		if x.ch.Op == opMqttSet && !r.f.Has(profile.MQTTHost) {
			var keep []cfgChange
			for _, y := range go2 {
				if y.Op == opMqttOpt {
					declined = append(declined, cfgOutcome{ch: y, state: "skipped", note: "the broker settings were not applied"})
					continue
				}
				keep = append(keep, y)
			}
			go2 = keep
		}
	}
	out := append(a.cfgApply(c, ip, go2, pws), declined...)
	out = append(out, noPw...)
	for k := range pws {
		pws[k] = ""
	}
	applied, failed, restart, power := a.cfgReport(out)
	a.UI.Say(fmt.Sprintf("  %d applied, %d failed, %d not applied.", applied, failed, len(out)-applied-failed))
	// what the camera has now is recorded BEFORE the restart question: the control port is down for a moment after it
	a.cfgSettle(c, src, failed == 0 && applied == len(out) && len(skipped) == 0, only, nil)
	if power {
		a.UI.Say("  A change above applies at the next power cycle: unplug the USB-C power, wait 5 seconds, plug it back (the assistant never does it for you).")
	}
	if restart {
		a.UI.Say("  Saved on the camera. It applies when the video service restarts.")
		if a.restartVideo(c) {
			a.UI.Good(fmt.Sprintf("  The video service was asked to restart; it is back in about 20 seconds (%s video show).", Prog))
		} else {
			a.UI.Say("  Not restarted: the new values apply at the next restart of the video service or the next reboot.")
		}
	}
	if failed > 0 {
		return ui.ErrFailed
	}
	return nil
}

func onlyNote(only map[string]bool) string {
	if only == nil {
		return ""
	}
	var g []string
	for k := range only {
		g = append(g, k)
	}
	sort.Strings(g)
	return " (" + strings.Join(g, ", ") + " only)"
}

// cfgSettle records what the camera has now as its local config and where it came from. clean says every
// change was applied (none failed, was declined or stopped, and nothing in the config was left out). When it is
// not, or the groups were restricted, the camera does not wholly follow that config: a first-install choice then
// stays what it was (source and pending, so the offers and the next apply still work) and any other source
// becomes "local". r is the read already made by the caller (nil: read again, after changes were sent).
func (a *App) cfgSettle(c *camssh.Client, src *cfgSrc, clean bool, only map[string]bool, r *cfgRead) {
	if a.UI.Dry() {
		return
	}
	switch {
	case clean && only == nil:
		a.setState(keyConfigSource, src.label)
		_ = a.St.Unset(keyConfigPending)
	case a.St.Get(keyConfigPending) == "1":
		a.UI.Say("  The config chosen for this camera is NOT fully applied: it stays pending (" + Prog + " config apply " + map[bool]string{true: "--global", false: "--from " + strings.TrimPrefix(src.label, "camera:")}[src.label == "global"] + " to finish).")
	default:
		a.setState(keyConfigSource, "local")
	}
	a.cfgRecordLocal(c, r)
}

// cfgRecordLocal writes camera.conf from a read of the camera (settings only: a password or the Wi-Fi is kept
// on this computer only when the user said so through save-local or save-global). A read in which the control
// port said nothing never replaces the file: it would erase the LED, picture, video and audio settings. A
// partial read keeps the earlier record's values for what it could not read.
func (a *App) cfgRecordLocal(c *camssh.Client, r *cfgRead) {
	if r == nil {
		n := a.cfgReadLive(c, a.Cams.Name)
		r = &n
	}
	if !r.ctlOK {
		a.UI.Warn("the camera's control port did not answer: its settings were NOT recorded on this computer (the earlier record is kept). Run '" + Prog + " config save-local' when it is back.")
		return
	}
	f := r.f.Clone()
	if len(r.missing) > 0 {
		if old, err := profile.Load(a.cameraPlace(a.Cams.Name), profile.Settings); err == nil {
			kept := 0
			for _, k := range cfgKeys() {
				if !f.Has(k) && old.Has(k) && r.unread(k) {
					_ = f.Set(k, old.Get(k))
					kept++
				}
			}
			a.UI.Say(fmt.Sprintf("  Could not be read: %s. %d value(s) of the earlier record were kept for them.", strings.Join(r.missing, ", "), kept))
		} else {
			a.UI.Say("  Not recorded (could not be read): " + strings.Join(r.missing, ", ") + ".")
		}
	}
	_ = f.Set(profile.SavedAt, cfgNow())
	if err := profile.Save(a.cameraPlace(a.Cams.Name), profile.Settings, f); err != nil {
		a.UI.Warn("could not record this camera's settings on this computer: " + err.Error())
		return
	}
	a.UI.Say("  The camera's settings were recorded on this computer (" + Prog + " config show).")
}
