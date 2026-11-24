package testfake

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
)

// The signed API of the fake 1080p (port 8092). It opens no socket: APIClient returns an akamapi.Client wired to it. It speaks the
// rules of the camera (docs/porting/lsc-1080p/control-api.md and specs/control-api.md): closed while no key exists, every request
// signed in challenge mode (the body hash included), scopes, PATCH all or nothing with one result per key, the registry names. The
// values live in the same map as the old text port's (Camera.ctl, the 8091 names); the table below is the only place the two name
// spaces meet, written here from the camera's own table, NOT from the assistant's (internal/camera/ctl): a wrong name on either side
// fails a test.

type row1080 struct {
	reg, legacy string
	write       bool
}

var rows1080 = []row1080{
	{"led.mode", "led.status", true}, {"night.ir_led", "night.ir_led", true}, {"night.mode", "night.mode", true},
	{"image.brightness", "isp.brightness", true}, {"image.contrast", "isp.contrast", true}, {"image.saturation", "isp.saturation", true},
	{"image.sharpness", "isp.sharpness", true}, {"image.denoise", "isp.denoise", true}, {"img.wb", "img.wb", true},
	{"image.flip", "image.flip", true}, {"image.mirror", "image.mirror", true}, {"image.ceiling", "image.ceiling", true},
	{"audio.out_volume", "audio.out_volume", true}, {"audio.out_enable", "audio.out_enable", true},
	{"audio.talk_enable", "audio.talk_enable", true}, {"audio.mic_enable", "audio.mic_enable", true},
	{"video0.kbps", "video.bitrate_cfg", true}, {"video0.fps", "video.fps_cfg", true}, {"video0.gop", "video.gop_cfg", true},
	{"video0.min_qp", "video.minqp_cfg", true}, {"video0.max_qp", "video.maxqp_cfg", true}, {"control.lan", "control.lan", true},
	// state (read only)
	{"sys.version", "fw.version", false}, {"net.hostname", "sys.hostname", false}, {"night.active", "night.state", false},
	{"net.wifi_rssi", "net.wifi_rssi", false}, {"net.wifi_rate", "net.wifi_rate", false}, {"sys.time_set", "sys.time_set", false},
	{"video0.fps_x10", "video.fps_actual", false}, {"video0.bitrate_kbps", "video.bitrate", false}, {"video0.cfg_pending", "video.cfg_pending", false},
	{"led.state", "led.state", false}, {"audio.out_state", "audio.out_state", false}, {"audio.out_underruns", "audio.out_underruns", false},
	{"audio.out_drops", "audio.out_drops", false}, {"net.ssid", "net.ssid", false},
}

func regValue(r row1080, legacyVal string) string {
	switch r.reg {
	case "night.active":
		if legacyVal == "night" {
			return "1"
		}
		return "0"
	case "net.wifi_rssi", "net.wifi_rate", "video0.fps_x10":
		if legacyVal == "unknown" {
			return "-1000000"
		}
	}
	return legacyVal
}

type apiKey1080 struct {
	key         akamapi.Key
	name, scope string
}

// NewAPIKey makes a key like `akamd --api-key new` does and opens the API (nothing listened while no key existed).
func (c *Camera) NewAPIKey(name, scope string) (akamapi.Key, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.newAPIKeyLocked(name, scope)
}

func (c *Camera) newAPIKeyLocked(name, scope string) (akamapi.Key, error) {
	for _, k := range c.apiKeys {
		if k.name == name {
			return akamapi.Key{}, errors.New("a key named " + name + " exists")
		}
	}
	var id [4]byte
	var sec [32]byte
	_, _ = rand.Read(id[:])
	_, _ = rand.Read(sec[:])
	k, err := akamapi.ParseKey(hex.EncodeToString(id[:]) + ":" + hex.EncodeToString(sec[:]))
	if err != nil {
		return akamapi.Key{}, err
	}
	c.apiKeys = append(c.apiKeys, apiKey1080{key: k, name: name, scope: scope})
	return k, nil
}

// RevokeAPIKeys removes every key: the API closes (a camera from before the API key existed).
func (c *Camera) RevokeAPIKeys() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.apiKeys = nil
}

// APIKeys is the number of API keys on the camera.
func (c *Camera) APIKeys() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.apiKeys)
}

// APIRequests are the requests the fake API received (signed ones record whether the signature was right).
func (c *Camera) APIRequests() []Req2K {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]Req2K(nil), c.apiReqs...)
}

// APIPatches are the bodies of the PATCH requests received.
func (c *Camera) APIPatches() []string {
	var out []string
	for _, r := range c.APIRequests() {
		if r.Method == "PATCH" {
			out = append(out, r.Body)
		}
	}
	return out
}

// APIClient is a client of the fake API signing with k.
func (c *Camera) APIClient(k akamapi.Key) *akamapi.Client {
	cl := &akamapi.Client{HostPort: "192.0.2.10:8092", Key: k}
	cl.Do = func(ctx context.Context, _ string, path string, h akamapi.Header) (int, []byte, error) {
		return c.apiServe(ctx, "GET", path, nil, h)
	}
	cl.DoReq = func(ctx context.Context, _ string, method, path string, body []byte, h akamapi.Header) (int, []byte, error) {
		return c.apiServe(ctx, method, path, body, h)
	}
	return cl
}

func (c *Camera) apiServe(ctx context.Context, method, path string, body []byte, h akamapi.Header) (int, []byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.opts.NoCtl || c.ctlDown || len(c.apiKeys) == 0 {
		return 0, nil, errors.New("connection refused")
	}
	c.apiN++
	if c.opts.CtlAnswerLimit > 0 && c.apiN > c.opts.CtlAnswerLimit {
		c.mu.Unlock() // a hung camera: nothing comes back until the caller gives up
		<-ctx.Done()
		c.mu.Lock()
		return 0, nil, ctx.Err()
	}
	if path == "/akam/v1/hello" {
		c.apiReqs = append(c.apiReqs, Req2K{Method: method, Path: path})
		b, _ := json.Marshal(map[string]any{"api": 1, "clock_set": false, "challenge": fmt.Sprintf("%030x%02x", 0x0123456789abcdef, c.apiN%256)})
		return 200, b, nil
	}
	var key *apiKey1080
	for i := range c.apiKeys {
		if c.apiKeys[i].key.ID == h.Get("X-Akam-Key") {
			key = &c.apiKeys[i]
		}
	}
	ch := h.Get("X-Akam-Challenge")
	ok := false
	if key != nil && ch != "" {
		want, err := akamapi.Sign(key.key, method, path, "", body, "challenge:"+ch, "challenge:"+ch)
		ok = err == nil && hmac.Equal([]byte(want.Get("X-Akam-Signature")), []byte(h.Get("X-Akam-Signature")))
	}
	c.apiReqs = append(c.apiReqs, Req2K{Method: method, Path: path, Body: string(body), SignedOK: ok})
	if !ok {
		return jerr(401, "bad_signature", "-")
	}
	write := key.scope == "write" || key.scope == "admin"
	switch {
	case method == "GET" && path == "/akam/v1/info":
		b, _ := json.Marshal(map[string]any{"model": "lsc-smart-indoor-1080p", "app": c.ctl["fw.version"], "key_id": key.key.ID, "scope": key.scope})
		return 200, b, nil
	case method == "GET" && (path == "/akam/v1/settings" || path == "/akam/v1/state"):
		m := map[string]string{}
		for _, r := range rows1080 {
			if r.write != (path == "/akam/v1/settings") {
				continue
			}
			if v, has := c.ctl[r.legacy]; has {
				m[r.reg] = regValue(r, v)
			}
		}
		b, _ := json.Marshal(m)
		return 200, b, nil
	case method == "PATCH" && path == "/akam/v1/settings":
		if !write {
			return jerr(403, "scope", "write scope needed")
		}
		return c.apiPatchLocked(body)
	case method == "POST" && path == "/akam/v1/actions/audio.beep":
		if !write {
			return jerr(403, "scope", "write scope needed")
		}
		c.ctlSets = append(c.ctlSets, "SET audio.out_test 1")
		if c.ctl["audio.out_enable"] != "1" {
			return jerr(422, "audio", "audio out is not running")
		}
		return 200, []byte(`{"ok":true}`), nil
	}
	return jerr(404, "unknown_path", "-")
}

func (c *Camera) apiPatchLocked(body []byte) (int, []byte, error) {
	var in map[string]string
	if json.Unmarshal(body, &in) != nil || len(in) == 0 {
		return jerr(400, "bad_json", "-")
	}
	byReg := map[string]row1080{}
	for _, r := range rows1080 {
		byReg[r.reg] = r
	}
	keys := make([]string, 0, len(in))
	for k := range in {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys { // every line sent is a line received, whatever the camera makes of it (tests count them)
		if r, known := byReg[k]; known && r.write {
			c.ctlSets = append(c.ctlSets, "SET "+r.legacy+" "+in[k])
		}
	}
	snap := map[string]string{}
	for k, v := range c.ctl {
		snap[k] = v
	}
	res := map[string]map[string]any{}
	bad := false
	for _, k := range keys {
		r, known := byReg[k]
		if !known || !r.write {
			res[k] = map[string]any{"ok": false, "error": "unknown_key"}
			bad = true
			continue
		}
		if _, has := c.ctl[r.legacy]; !has { // an image without this row
			res[k] = map[string]any{"ok": false, "error": "unknown_key"}
			bad = true
			continue
		}
		if bad {
			continue
		}
		reply := c.setLocked(r.legacy, in[k])
		switch {
		case strings.HasPrefix(reply, "OK "):
			ap := "applied"
			if strings.Contains(reply, "(restart to apply)") || strings.HasPrefix(r.reg, "video0.") {
				ap = "restart_component"
			}
			res[k] = map[string]any{"ok": true, "apply": ap, "value": in[k]}
			if c.opts.FlashSaveFails {
				res[k]["saved"] = false
			}
		default:
			code := "range"
			if strings.Contains(reply, "refused") {
				code = "check"
			}
			res[k] = map[string]any{"ok": false, "error": code, "detail": strings.TrimPrefix(reply, "ERR ")}
			bad = true
		}
	}
	if bad {
		c.ctl = snap // all or nothing
		for _, k := range keys {
			if ok, _ := res[k]["ok"].(bool); ok || res[k] == nil {
				res[k] = map[string]any{"ok": false, "error": "not_applied"}
			}
		}
		b, _ := json.Marshal(map[string]any{"results": res})
		return 422, b, nil
	}
	b, _ := json.Marshal(map[string]any{"results": res})
	return 200, b, nil
}

// execKey serves the two key commands of the camera (`akamd --api-key new ...` and `revoke NAME --force`), over SSH.
func (c *Camera) execKey(cmd string, stdout, stderr io.Writer) (int, bool) {
	bin := "/usr/bin/akamd --api-key "
	if c.opts.LegacyNames { // firmware 0.1.x: the program is /usr/bin/ak_rtsp, /usr/bin/akamd does not exist
		if strings.HasPrefix(cmd, bin) {
			fmt.Fprintln(stderr, "sh: /usr/bin/akamd: not found")
			return 127, true
		}
		bin = "/usr/bin/ak_rtsp --api-key "
	}
	if !strings.HasPrefix(cmd, bin) {
		return 0, false
	}
	switch cmd {
	case bin + "new --scope admin --name assistant":
		c.mu.Lock()
		k, err := c.newAPIKeyLocked("assistant", "admin")
		c.mu.Unlock()
		if err != nil {
			fmt.Fprintln(stderr, "akam_api_key: "+err.Error())
			return 1, true
		}
		fmt.Fprintf(stdout, "key_id:   %s\nsecret:   %s\nscope:    admin\nname:     assistant\n# the secret is shown ONCE; the API (port 8092) picks the key up within seconds\n", k.ID, hex.EncodeToString(k.Secret))
		return 0, true
	case bin + "revoke assistant --force":
		c.mu.Lock()
		defer c.mu.Unlock()
		for i := range c.apiKeys {
			if c.apiKeys[i].name == "assistant" {
				c.apiKeys = append(c.apiKeys[:i], c.apiKeys[i+1:]...)
				fmt.Fprintln(stdout, "revoked assistant")
				return 0, true
			}
		}
		fmt.Fprintln(stderr, "akam_api_key: no such key")
		return 1, true
	}
	return 2, true
}
