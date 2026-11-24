package ctl

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
)

// The 1080p used to be driven through the text control port (8091); that port is now closed to the network, and
// the assistant uses the signed API of the camera (port 8092) for both models. The commands and the option engine
// still speak in the old key names, one GET or SET line each, and this file is the one place that turns such a line
// into an API call and the camera's answer back into the old answer lines, so Get, ParseSet and ParseLedSet read them
// unchanged. docs/porting/lsc-1080p/control-api.md section 1 is the table the names come from.

// legacyToRegistry maps an 8091 key to the row of the registry that holds the same value.
var legacyToRegistry = map[string]string{
	"led.status": "led.mode", "night.ir_led": "night.ir_led", "night.mode": "night.mode", "led.state": "led.state",
	"isp.brightness": "image.brightness", "isp.contrast": "image.contrast", "isp.saturation": "image.saturation",
	"isp.sharpness": "image.sharpness", "isp.denoise": "image.denoise", "isp.wdr": "isp.wdr", "isp.nr2": "isp.nr2", "img.wb": "img.wb",
	"image.flip": "image.flip", "image.mirror": "image.mirror", "image.ceiling": "image.ceiling",
	"audio.out_volume": "audio.out_volume", "audio.out_enable": "audio.out_enable", "audio.talk_enable": "audio.talk_enable",
	"audio.mic_enable": "audio.mic_enable", "audio.out_state": "audio.out_state", "audio.out_underruns": "audio.out_underruns",
	"audio.out_drops":   "audio.out_drops",
	"video.bitrate_cfg": "video0.kbps", "video.fps_cfg": "video0.fps", "video.gop_cfg": "video0.gop", "video.minqp_cfg": "video0.min_qp",
	"video.maxqp_cfg": "video0.max_qp", "video.cfg_pending": "video0.cfg_pending", "video.bitrate": "video0.bitrate_kbps",
	"video.fps_actual": "video0.fps_x10", "night.state": "night.active", "sys.time_set": "sys.time_set",
	"net.wifi_rssi": "net.wifi_rssi", "net.wifi_rate": "net.wifi_rate", "fw.version": "sys.version", "sys.hostname": "net.hostname",
	"control.lan": "control.lan", "net.ssid": "net.ssid",
}

// naValue is the registry's "not known" number (hal_sys): the old port said "unknown".
const naValue = "-1000000"

// RegistryKey is the registry row of an 8091 key, ok false for a key the signed API has no row for (net.mac and
// the debug flag): the answer to a GET of it is then missing, as from an older image.
func RegistryKey(legacy string) (string, bool) {
	k, ok := legacyToRegistry[legacy]
	return k, ok
}

// legacyValue turns the registry value of a row into what the old port printed.
func legacyValue(legacy, v string) string {
	switch legacy {
	case "night.state":
		if v == "1" {
			return "night"
		}
		return "day"
	case "net.wifi_rssi", "net.wifi_rate", "video.fps_actual":
		if v == naValue {
			return "unknown"
		}
	}
	return v
}

// Client is the part of the signed API an exchange needs (akamapi.Client implements it).
type Client interface {
	Settings(ctx context.Context) (map[string]string, error)
	State(ctx context.Context) (map[string]string, error)
	Patch(ctx context.Context, kv map[string]string) (akamapi.PatchResult, error)
	Action(ctx context.Context, name string, args map[string]string) error
}

var _ Client = (*akamapi.Client)(nil)

type setLine struct{ legacy, reg, val string }

// Exchange runs GET and SET lines ("GET key", "SET key value") the way the old port did and returns the answer lines.
// GETs read /settings and /state at most once each, the SETs go in ONE PATCH (the camera validates all of them or none
// and spends one flash token), then one answer line per SET, in registry key order. "SET audio.out_test 1" is the action
// audio.beep. QUIT lines are ignored. A transport or authentication error is returned together with the answers that
// could be produced (none for a read that failed).
func Exchange(ctx context.Context, cl Client, lines []string) ([]string, error) {
	var settings, state map[string]string
	var sets []setLine
	var out []string
	var firstErr error
	fail := func(err error) {
		if firstErr == nil {
			firstErr = err
		}
	}
	load := func(m *map[string]string, get func(context.Context) (map[string]string, error)) {
		if *m != nil {
			return
		}
		v, err := get(ctx)
		if err != nil {
			fail(err)
			v = map[string]string{}
		}
		*m = v
	}
	read := func(reg string) (string, bool) {
		load(&settings, cl.Settings)
		if v, ok := settings[reg]; ok {
			return v, true
		}
		load(&state, cl.State)
		v, ok := state[reg]
		return v, ok
	}
	beeps := 0
	for _, l := range lines {
		f := strings.SplitN(strings.TrimSpace(l), " ", 3)
		switch {
		case f[0] == "GET" && len(f) == 2:
			reg, ok := RegistryKey(f[1])
			if !ok {
				continue
			}
			if v, ok := read(reg); ok {
				out = append(out, f[1]+"="+legacyValue(f[1], v))
			}
		case f[0] == "SET" && len(f) == 3 && f[1] == "audio.out_test":
			beeps++
		case f[0] == "SET" && len(f) == 3:
			reg, ok := RegistryKey(f[1])
			if !ok {
				out = append(out, "ERR unknown param "+f[1])
				continue
			}
			sets = append(sets, setLine{f[1], reg, f[2]})
		}
	}
	if len(sets) > 0 {
		out = append(out, patch(ctx, cl, sets, fail)...)
	}
	for ; beeps > 0; beeps-- {
		err := cl.Action(ctx, "audio.beep", nil)
		var ae *akamapi.APIError
		switch {
		case err == nil:
			out = append(out, "OK audio.out_test=1")
		case errors.As(err, &ae):
			out = append(out, "ERR audio.out_test: "+ae.Error())
		default:
			fail(err)
		}
	}
	return out, firstErr
}

func patch(ctx context.Context, cl Client, sets []setLine, fail func(error)) []string {
	kv := map[string]string{}
	byReg := map[string]string{}
	for _, s := range sets {
		kv[s.reg] = s.val
		byReg[s.reg] = s.legacy
	}
	res, err := cl.Patch(ctx, kv)
	var ae *akamapi.APIError
	var out []string
	switch {
	case err == nil:
		names := make([]string, 0, len(kv))
		for reg := range kv {
			names = append(names, reg)
		}
		sort.Strings(names)
		for _, reg := range names {
			out = append(out, setAnswer(byReg[reg], kv[reg], res.Results[reg]))
		}
	case errors.As(err, &ae):
		for _, s := range sets {
			out = append(out, "ERR "+s.legacy+": the camera answered "+ae.Error())
		}
	default:
		fail(err)
	}
	return out
}

// setAnswer is the old answer line of one SET from the camera's result for its row.
func setAnswer(legacy, val string, r akamapi.KeyResult) string {
	switch {
	case r.OK:
		s := "OK " + legacy + "=" + val
		if r.Apply == "restart_component" || r.Apply == "pending_device_restart" {
			s += " (restart to apply)"
		}
		if r.NotSaved {
			s += " (not saved: the camera could not write it to flash)"
		}
		return s
	case r.Error == "unknown_key":
		return "ERR unknown param " + legacy
	case r.Error == "not_applied":
		return "ERR " + legacy + " not applied: another setting of the same change was refused"
	case r.Error == "":
		return "ERR " + legacy + " was not answered"
	}
	d := ""
	if r.Detail != "" && r.Detail != "-" {
		d = " " + r.Detail
	}
	return fmt.Sprintf("ERR invalid %s value %s (%s%s)", legacy, val, r.Error, d)
}
