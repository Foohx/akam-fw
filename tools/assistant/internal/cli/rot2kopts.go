package cli

import (
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The options of the card-less Rotatable 2K: the same commands as the 1080p (led, picture, video, hostname) plus
// track, wifi and the generic `settings`, all through the signed API of the camera (port 8092,
// docs/porting/lsc-rotatable-2k/control-api.md): GET /settings to read, one PATCH /settings to change. The
// catalogue and its validators are camopt.Catalog2K. Needs the API key made by key-2k; nothing creates a key here.
// Host-tested only (fake API server): not verified against a real 2K yet.

// opts2KCommands are the commands this file serves on a 2K; the other option commands stay refused for it.
var opts2KCommands = map[string]bool{"led": true, "picture": true, "video": true, "hostname": true, "track": true, "wifi": true, "settings": true}

// words2K maps the words of a command to registry keys.
var words2K = map[string]map[string]string{
	"led": {"status": "led.mode", "ir": "night.ir_led"},
	"picture": {"brightness": "image.brightness", "contrast": "image.contrast", "sharpness": "image.sharpness", "mains": "image.mains_hz", "flip": "image.flip", "mirror": "image.mirror", "ceiling": "image.ceiling",
		"saturation": "isp.saturation", "denoise": "isp.denoise", "wb": "img.wb"},
	"video": {"bitrate": "kbps", "maxbitrate": "max_kbps", "fps": "fps", "gop": "gop", "minqp": "min_qp", "maxqp": "max_qp", "rc": "rc"},
	"track": {"return": "track.return", "delay": "track.return_delay_s", "tilt": "track.tilt", "lag": "track.lag_ms"},
	"wifi":  {"country": "net.wifi_country", "band": "net.wifi_band"},
}

// unavailable2K are the words of the 1080p commands the 2K has no equivalent for.
var unavailable2K = map[string]string{
	"night": "the 2K has no night mode setting (night.ir_led is 'led ir')",
	"reset": "there is no reset yet: set the values you want",
}

func (a *App) dispatch2KOpts(cmd string, args []string) error {
	if cmd == "settings" {
		return a.cmdSettings2K(args)
	}
	var changes []camopt.Change
	var show []string
	seen := map[string]bool{}
	doShow := false
	add := func(key, w, v string) error {
		val, err := camopt.Validate2K(key, v)
		if err != nil {
			return audioUsage(fmt.Sprintf("%s %s: %s", cmd, w, err.Error()))
		}
		if seen[key] {
			return audioUsage(fmt.Sprintf("%s %s given twice", cmd, w))
		}
		seen[key] = true
		changes = append(changes, camopt.Change{ID: key, Value: val})
		return nil
	}
	prefix := "video0."
	switch cmd {
	case "hostname":
		show = []string{"net.hostname"}
		if len(args) > 1 {
			return audioUsage("hostname takes one name")
		}
		if len(args) == 1 {
			if err := add("net.hostname", "NAME", args[0]); err != nil {
				return err
			}
		}
	default:
		for i := 0; i < len(args); {
			w := args[i]
			next := ""
			if i+1 < len(args) {
				next = args[i+1]
			}
			key, ok := words2K[cmd][w]
			switch {
			case w == "show":
				doShow = true
				i++
			case cmd == "track" && (w == "on" || w == "off"):
				v := map[string]string{"on": "1", "off": "0"}[w]
				if err := add("track.enabled", w, v); err != nil {
					return err
				}
				i++
			case cmd == "video" && w == "stream":
				if next != "0" && next != "1" {
					return audioUsage("video stream needs 0 (main) or 1 (second)")
				}
				prefix = "video" + next + "."
				i += 2
			case ok:
				if cmd == "video" {
					key = prefix + key
				}
				if next == "" {
					return audioUsage(fmt.Sprintf("%s %s needs a value", cmd, w))
				}
				if err := add(key, w, next); err != nil {
					return err
				}
				i += 2
			case unavailable2K[w] != "":
				return audioUsage(fmt.Sprintf("%s %s is not available on the 2K: %s", cmd, w, unavailable2K[w]))
			default:
				return audioUsage(fmt.Sprintf("%s: unknown word '%s'", cmd, w))
			}
		}
		if doShow && len(changes) > 0 {
			return audioUsage(cmd + ": show takes no other word")
		}
		switch cmd {
		case "led":
			show = keysWithPrefix2K("led.", "night.ir_led")
		case "picture":
			show = keysWithPrefix2K("image.", "isp.", "img.")
		case "video":
			show = keysWithPrefix2K(prefix)
		case "track":
			show = keysWithPrefix2K("track.", "detect.publish_hz")
		case "wifi":
			show = []string{"net.wifi_country", "net.wifi_band"}
		}
	}
	if err := checkOffline2K(changes); err != nil {
		return err
	}
	return a.run2KOpts(map[string]string{"led": "Front LED and IR LEDs", "picture": "Picture", "video": "Video settings", "hostname": "The camera's network name",
		"track": "Person tracking", "wifi": "Wi-Fi country and band"}[cmd], changes, show)
}

// keysWithPrefix2K lists the catalogue keys with one of these prefixes (or exactly these keys), in catalogue order.
func keysWithPrefix2K(p ...string) []string {
	var out []string
	for _, o := range camopt.Catalog2K() {
		for _, x := range p {
			if strings.HasPrefix(o.Key, x) || o.Key == x {
				out = append(out, o.Key)
				break
			}
		}
	}
	return out
}

// checkOffline2K applies the cross-field rules that need no value of the camera.
func checkOffline2K(changes []camopt.Change) error {
	pending := map[string]string{}
	for _, c := range changes {
		pending[c.ID] = c.Value
	}
	if errs := camopt.Check2K(pending, nil); len(errs) > 0 {
		return audioUsage(errs[0].Error())
	}
	return nil
}

// cmdSettings2K is `settings [list | get KEY|PREFIX... | set KEY VALUE [KEY VALUE]...]` for the 2K.
func (a *App) cmdSettings2K(args []string) error {
	action := "list"
	if len(args) > 0 {
		action, args = args[0], args[1:]
	}
	var changes []camopt.Change
	var show []string
	switch action {
	case "list":
		if len(args) > 0 {
			return audioUsage("settings list takes no argument")
		}
		show = keysWithPrefix2K("")
	case "get":
		if len(args) == 0 {
			return audioUsage("settings get needs a key or a prefix (settings list shows them)")
		}
		for _, k := range args {
			ks := keysWithPrefix2K(k)
			if len(ks) == 0 {
				return audioUsage(fmt.Sprintf("settings get: '%s' is not a setting of the 2K that can be read here (settings list shows them)", k))
			}
			show = append(show, ks...)
		}
	case "set":
		if len(args) == 0 || len(args)%2 != 0 {
			return audioUsage("settings set needs pairs: KEY VALUE [KEY VALUE ...]")
		}
		seen := map[string]bool{}
		for i := 0; i < len(args); i += 2 {
			v, err := camopt.Validate2K(args[i], args[i+1])
			if err != nil {
				return audioUsage("settings set: " + err.Error())
			}
			if seen[args[i]] {
				return audioUsage("settings set: " + args[i] + " given twice")
			}
			seen[args[i]] = true
			changes = append(changes, camopt.Change{ID: args[i], Value: v})
			show = append(show, args[i])
		}
	default:
		return audioUsage("settings needs list, get KEY... or set KEY VALUE...")
	}
	if err := checkOffline2K(changes); err != nil {
		return err
	}
	return a.run2KOpts("Settings of the 2K", changes, show)
}

// ---------------------------------------------------------------- shared engine

// maskedBody is the PATCH body as shown on a screen or in a rehearsal: the value of a secret option is never in it.
func maskedBody(changes []camopt.Change) string {
	parts := make([]string, len(changes))
	sorted := append([]camopt.Change(nil), changes...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].ID < sorted[j].ID })
	for i, c := range sorted {
		v := c.Value
		if o, ok := camopt.Find2K(c.ID); !ok || camopt.Display2K(o, "x") != "x" {
			v = "***"
		}
		parts[i] = fmt.Sprintf("%q:%q", c.ID, ui.Sanitize(v))
	}
	return "{" + strings.Join(parts, ",") + "}"
}

// api2K builds the client of the camera, or says there is no key and points to key-2k (it never makes one).
func (a *App) api2K(ip string) (*akamapi.Client, error) {
	k, ok, err := a.apiKey2K()
	if err != nil {
		return nil, a.UI.Fail("the API key cannot be used: "+ui.Sanitize(err.Error())+".", "check the file (--api-key-file, or the api_key file of this camera's state folder).")
	}
	if !ok {
		return nil, a.UI.Fail("this PC has no API key for this camera, and the options of the 2K go through its API.",
			"run: "+Prog+" key-2k (it asks before creating the key), or pass --api-key-file FILE.")
	}
	return a.camAPI(ip, k), nil
}

// api2KFail turns an API error into a plain message.
func (a *App) api2KFail(err error) error {
	var ae *akamapi.APIError
	switch {
	case errors.Is(err, akamapi.ErrAuth):
		return a.UI.Fail("the camera refused the API key (revoked, or not the key of this camera).", "make a new one: "+Prog+" key-2k (revoke the old one on the camera first).")
	case errors.Is(err, akamapi.ErrScope):
		return a.UI.Fail("the API key may only read: changing a setting needs a write or admin key.", "key-2k makes an admin key; or use another key with --api-key-file.")
	case errors.As(err, &ae):
		if ae.Status == 429 {
			return a.UI.Fail("the camera is rate-limiting the API ("+ui.Sanitize(ae.Error())+").", "wait a few seconds and try again.")
		}
		return a.UI.Fail("the camera answered: "+ui.Sanitize(ae.Error())+".", "nothing was changed.")
	case errors.Is(err, akamapi.ErrClosed):
		return a.UI.Fail("the camera does not answer its control API on port 8092 ("+ui.Sanitize(err.Error())+").",
			"check that it is on and that the address is right. The API only listens once an API key exists on the camera ("+Prog+" key-2k).")
	}
	return a.UI.Fail("the camera API failed: "+ui.Sanitize(err.Error())+".", "nothing was changed.")
}

// confirm2K is the yes/no (default No) before a change; --yes answers it, and with no terminal it refuses.
func (a *App) confirm2K(q string) bool {
	if a.Opt.Yes {
		return true
	}
	if !a.UI.Interactive() {
		a.UI.Err("this change needs a confirmation: run it in a terminal, or add --yes.")
		return false
	}
	ok, err := a.UI.AskYN(q)
	return err == nil && ok
}

func (a *App) valueLine2K(o camopt.Option, cur map[string]string) string {
	v, ok := cur[o.Key]
	if !ok && o.Kind == camopt.Secret { // write-only: the camera never reports it
		return fmt.Sprintf("  %-44s write-only (never shown, never read back)", o.Label)
	}
	if !ok {
		return fmt.Sprintf("  %-44s n/a (this camera does not report it)", o.Label)
	}
	return fmt.Sprintf("  %-44s %-10s %s", o.Label, ui.Sanitize(camopt.Display2K(o, v)), rangeText2K(o))
}

func rangeText2K(o camopt.Option) string {
	switch o.Kind {
	case camopt.Int:
		return fmt.Sprintf("[%d to %d]", o.Min, o.Max)
	case camopt.Enum:
		return "[" + strings.Join(o.Values, ", ") + "]"
	case camopt.Bool:
		return "[on, off]"
	}
	return ""
}

// run2KOpts shows the values of showKeys, after sending changes (one PATCH) when there are some.
func (a *App) run2KOpts(title string, changes []camopt.Change, showKeys []string) error {
	a.banner(title)
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD use the signed API of the camera (http://" + ip + ":8092/akam/v1, key from key-2k; read-only until a change is confirmed): GET /settings")
		if len(changes) > 0 {
			a.UI.Say("  WOULD check the values against the camera's own rules, ask for a confirmation (default No) for a Wi-Fi or name change, then send in ONE request:")
			a.UI.Say("  WOULD send: PATCH /settings " + maskedBody(changes))
		}
		return nil
	}
	cl, err := a.api2K(ip)
	if err != nil {
		return err
	}
	cur, err := cl.Settings(a.Ctx)
	if err != nil {
		return a.api2KFail(err)
	}
	if len(changes) > 0 {
		if err := a.send2K(cl, cur, changes); err != nil {
			return err
		}
		if cur, err = cl.Settings(a.Ctx); err != nil {
			return a.api2KFail(err)
		}
	}
	for _, k := range showKeys {
		if o, ok := camopt.Find2K(k); ok {
			a.UI.Say(a.valueLine2K(o, cur))
		}
	}
	return nil
}

// send2K checks, confirms and sends one change set, then reports each key.
func (a *App) send2K(cl *akamapi.Client, cur map[string]string, changes []camopt.Change) error {
	pending := map[string]string{}
	var todo []camopt.Change
	asks := false
	for _, c := range changes {
		pending[c.ID] = c.Value
		if old, ok := cur[c.ID]; ok && old == c.Value {
			o, _ := camopt.Find2K(c.ID)
			a.UI.Say(fmt.Sprintf("  %s is already %s: not sent.", o.Label, camopt.Display2K(o, c.Value)))
			continue
		}
		if o, _ := camopt.Find2K(c.ID); o.Danger >= camopt.DangerAsks {
			asks = true
		}
		todo = append(todo, c)
	}
	if errs := camopt.Check2K(pending, cur); len(errs) > 0 {
		return a.UI.Fail(ui.Sanitize(errs[0].Error())+".", "nothing was sent.")
	}
	if len(todo) == 0 {
		return nil
	}
	if asks {
		a.UI.Say("  A Wi-Fi or name change is applied by the camera within seconds; a Wi-Fi change that loses the network is reverted by the camera after about a minute. A new stream password disconnects the viewers that are logged in.")
		if !a.confirm2K("Send " + maskedBody(todo) + " to the camera?") {
			return a.UI.Fail("not confirmed.", "nothing was changed.")
		}
	}
	a.UI.Say("  WILL send: PATCH /settings " + maskedBody(todo))
	return a.report2K(cl, todo)
}

// report2K sends the change and tells, key by key, what the camera did and when it takes effect.
func (a *App) report2K(cl *akamapi.Client, todo []camopt.Change) error {
	kv := map[string]string{}
	for _, c := range todo {
		kv[c.ID] = c.Value
	}
	res, err := cl.Patch(a.Ctx, kv)
	if err != nil {
		return a.api2KFail(err)
	}
	keys := make([]string, 0, len(res.Results))
	for k := range res.Results {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	failed := 0
	for _, k := range keys {
		r := res.Results[k]
		o, known := camopt.Find2K(k)
		name := k
		if known {
			name = o.Label
		}
		switch {
		case r.OK:
			v := ui.Sanitize(r.Value)
			if known {
				v = camopt.Display2K(o, r.Value)
			}
			a.UI.Good(fmt.Sprintf("  %s: %s (%s).", name, v, when2K(o, r.Apply)))
		case r.Error == "not_applied":
			failed++
			a.UI.Say(fmt.Sprintf("  %s: not applied (another setting of the same change was refused: the camera applies all or none).", name))
		default:
			failed++
			a.UI.Err(fmt.Sprintf("the camera refused %s: %s", k, ui.Sanitize(strings.TrimSpace(r.Error+" "+r.Detail))))
		}
	}
	if failed > 0 {
		if res.Status == 207 {
			return a.UI.Fail("some settings were applied and some failed (see above).", "read them back with: "+Prog+" settings get KEY.")
		}
		return a.UI.Fail("the camera refused the change.", "nothing was applied.")
	}
	return nil
}

// when2K is the "applied / needs a restart" note of one result.
func when2K(o camopt.Option, apply string) string {
	switch apply {
	case "restart_component":
		return "needs a restart of that part of the camera to take effect"
	case "pending_device_restart":
		return "saved; takes effect after the camera restarts: power-cycle it yourself, the assistant never does"
	}
	if o.Applies == camopt.Queued {
		return "queued; the camera applies it within seconds (Wi-Fi: up to a minute, with automatic revert); read it back later"
	}
	if o.Persisted == camopt.PersistRAM {
		return "applied now, not saved: back to the default at the next restart"
	}
	return "applied now, saved on the camera"
}

// ---------------------------------------------------------------- the menu

// flowOptions2K is the Camera options screen of a 2K: groups of the catalogue, current values read through the
// API, local edits, and one confirmed PATCH.
func (a *App) flowOptions2K() error {
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.banner("Camera options - " + a.Cams.Name)
		a.UI.Say("  WOULD use the signed API of the 2K (http://" + ip + ":8092/akam/v1, key from key-2k): GET /settings to show every value (read-only),")
		a.UI.Say("  edit values locally, and only on 'apply' ask a confirmation (default No) and send ONE PATCH /settings.")
		return nil
	}
	cl, err := a.api2K(ip)
	if err != nil {
		return a.reportFlowErr2K(err)
	}
	for {
		cur, err := cl.Settings(a.Ctx)
		if err != nil {
			return a.reportFlowErr2K(a.api2KFail(err))
		}
		a.UI.Blank()
		a.UI.Bold(fmt.Sprintf("Camera options - %s (%s), Rotatable 2K, through its signed API", a.Cams.Name, ip))
		items := make([]string, 0, len(camopt.Groups2K)+2)
		for _, g := range camopt.Groups2K {
			items = append(items, fmt.Sprintf("%-14s %s", g, a.groupSummary2K(g, cur)))
		}
		items = append(items, "Reload", "Back")
		i, err := a.UI.Pick("Which group?", items)
		if err != nil || i == len(camopt.Groups2K)+1 {
			return nil
		}
		if i < len(camopt.Groups2K) {
			a.groupScreen2K(cl, camopt.Groups2K[i], cur)
		}
	}
}

func (a *App) reportFlowErr2K(err error) error {
	a.reportFlowError(err)
	return nil
}

func (a *App) groupSummary2K(g string, cur map[string]string) string {
	var parts []string
	for _, o := range camopt.Catalog2K() {
		if o.Group != g {
			continue
		}
		for _, k := range summaryKeys2K {
			if k == o.Key {
				v, ok := cur[k]
				if !ok {
					v = "n/a"
				}
				parts = append(parts, k[strings.LastIndexByte(k, '.')+1:]+" "+ui.Sanitize(camopt.Display2K(o, v)))
			}
		}
	}
	return strings.Join(parts, "   ")
}

var summaryKeys2K = []string{"led.mode", "night.ir_led", "image.brightness", "image.flip", "image.mirror", "video0.kbps", "video0.fps",
	"track.enabled", "track.return", "ptz.patrol.idle_start_s", "net.hostname", "net.wifi_country", "net.wifi_band", "audio.mic_enable", "mqtt.enabled", "onvif.enabled"}

func (a *App) groupScreen2K(cl *akamapi.Client, group string, cur map[string]string) {
	var opts []camopt.Option
	for _, o := range camopt.Catalog2K() {
		if o.Group == group {
			opts = append(opts, o)
		}
	}
	pending := map[string]string{}
	for {
		a.UI.Blank()
		a.UI.Bold(fmt.Sprintf("%s - %s", group, a.Cams.Name))
		items := make([]string, 0, len(opts)+3)
		for _, o := range opts {
			line := strings.TrimPrefix(a.valueLine2K(o, cur), "  ")
			if p, ok := pending[o.Key]; ok {
				line += "   -> " + ui.Sanitize(camopt.Display2K(o, p)) + " (pending)"
			}
			items = append(items, line)
		}
		n := len(pending)
		items = append(items, fmt.Sprintf("Apply %d pending change(s)", n), "Discard the pending changes", "Back")
		i, err := a.UI.Pick(group+": which option?", items)
		if err != nil || i == len(opts)+2 {
			if n > 0 {
				a.UI.Say("  Nothing was sent: the pending changes are discarded.")
			}
			return
		}
		switch {
		case i < len(opts):
			o := opts[i]
			if _, ok := cur[o.Key]; !ok && o.Kind != camopt.Secret {
				a.UI.Say("  This camera does not report that setting: update it first (Update this camera).")
				continue
			}
			if v, ok := a.askValue2K(o, cur[o.Key]); ok {
				if v == cur[o.Key] {
					delete(pending, o.Key)
				} else {
					pending[o.Key] = v
				}
			}
		case i == len(opts):
			if n == 0 {
				a.UI.Say("  Nothing to apply.")
				continue
			}
			var changes []camopt.Change
			for _, o := range opts {
				if v, ok := pending[o.Key]; ok {
					changes = append(changes, camopt.Change{ID: o.Key, Value: v})
				}
			}
			if !a.confirm2KList(changes, cur) {
				a.UI.Say("  Not confirmed: nothing was sent.")
				continue
			}
			if err := a.send2KConfirmed(cl, cur, changes); err != nil {
				a.reportFlowError(err)
			}
			if fresh, err := cl.Settings(a.Ctx); err == nil {
				cur = fresh
			}
			pending = map[string]string{}
		default:
			pending = map[string]string{}
			a.UI.Say("  Discarded.")
		}
	}
}

// confirm2KList shows what will change (old to new, secrets hidden) and asks yes/no, default No.
func (a *App) confirm2KList(changes []camopt.Change, cur map[string]string) bool {
	a.UI.Say("  About to change:")
	for _, c := range changes {
		o, _ := camopt.Find2K(c.ID)
		a.UI.Say(fmt.Sprintf("    %s: %s -> %s   (%s)", o.Label, ui.Sanitize(camopt.Display2K(o, cur[c.ID])), ui.Sanitize(camopt.Display2K(o, c.Value)), camopt.Describe2K(o)))
	}
	return a.confirm2K(fmt.Sprintf("Send %d change(s) to the camera?", len(changes)))
}

// send2KConfirmed is send2K for a change set the user already confirmed on the screen (no second question).
func (a *App) send2KConfirmed(cl *akamapi.Client, cur map[string]string, changes []camopt.Change) error {
	pending := map[string]string{}
	for _, c := range changes {
		pending[c.ID] = c.Value
	}
	if errs := camopt.Check2K(pending, cur); len(errs) > 0 {
		return a.UI.Fail(ui.Sanitize(errs[0].Error())+".", "nothing was sent.")
	}
	a.UI.Say("  WILL send: PATCH /settings " + maskedBody(changes))
	return a.report2K(cl, changes)
}

// askValue2K asks the new value of one option and validates it before it is kept.
func (a *App) askValue2K(o camopt.Option, cur string) (string, bool) {
	now := camopt.Display2K(o, cur)
	switch o.Kind {
	case camopt.Enum, camopt.Bool:
		labels := o.Values
		if o.Kind == camopt.Bool {
			labels = []string{"off", "on"}
		}
		items := append(append([]string(nil), labels...), "Cancel")
		i, err := a.UI.Pick(fmt.Sprintf("%s (now %s):", o.Label, ui.Sanitize(now)), items)
		if err != nil || i >= len(labels) {
			return "", false
		}
		v, err := camopt.Validate2K(o.Key, labels[i])
		return v, err == nil
	case camopt.Secret:
		a.UI.Say("  " + o.Help + ".")
		s, err := a.UI.Secret(fmt.Sprintf("  %s (not shown; empty cancels):", o.Label))
		if err != nil || s == "" {
			return "", false
		}
		v, verr := camopt.Validate2K(o.Key, s)
		if verr != nil {
			a.UI.Say("  " + verr.Error() + ".")
			return "", false
		}
		a.Secrets.Add(v)
		return v, true
	}
	for {
		if o.Help != "" {
			a.UI.Say("  " + o.Help + ".")
		}
		s, err := a.UI.Prompt(fmt.Sprintf("  %s (now %s; empty cancels):", o.Label, ui.Sanitize(now)), "")
		if err != nil || s == "" {
			return "", false
		}
		v, verr := camopt.Validate2K(o.Key, strings.TrimSpace(s))
		if verr != nil {
			a.UI.Say("  " + verr.Error() + ".")
			continue
		}
		return v, true
	}
}
