package testfake

import (
	"context"
	"crypto/hmac"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sort"
	"strings"
	"sync"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
)

// API2K is an in-memory fake of the signed local API of the Rotatable 2K (port 8092): GET /hello, GET /info,
// GET /settings and PATCH /settings, with the camera's rules (every request is signed in challenge mode and the
// signature, body hash included, is checked; a read key cannot PATCH; all-or-nothing validation; per-key results).
// It opens no socket: Client returns an akamapi.Client wired to it.
type API2K struct {
	Key    akamapi.Key
	Scope  string            // "admin" (default), "write" or "read"
	Values map[string]string // the writable settings the camera reports
	// Refuse makes a key fail validation: key -> "code|detail" (for example "range|[1,15]").
	Refuse map[string]string
	// Apply overrides the "apply" word of a key (default "applied").
	Apply map[string]string
	// Closed: nothing listens (no key installed on the camera).
	Closed bool
	// WriteOnly are the write-only rows (mqtt.password, rtsp.password): accepted in a PATCH, kept in Secrets, never returned by a GET and never
	// echoed in a PATCH reply (the firmware's ST_SECRET_WO rows).
	WriteOnly map[string]bool
	Secrets   map[string]string
	// Extra holds hypothetical extra values the camera returns in GET /settings (a test that a credential is never shown).
	Extra map[string]string

	// Beeps counts the audio.beep actions the speaker played.
	Beeps int

	mu       sync.Mutex
	Requests []Req2K
	n        int
}

// Req2K is one request the fake received.
type Req2K struct {
	Method, Path string
	Body         string
	SignedOK     bool
}

// Patches returns the bodies of the PATCH requests received.
func (f *API2K) Patches() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []string
	for _, r := range f.Requests {
		if r.Method == "PATCH" {
			out = append(out, r.Body)
		}
	}
	return out
}

// Client is a client of the fake, with the fake's key.
func (f *API2K) Client() *akamapi.Client {
	c := &akamapi.Client{HostPort: "192.0.2.10:8092", Key: f.Key}
	c.Do = func(_ context.Context, _ string, path string, h akamapi.Header) (int, []byte, error) {
		return f.serve("GET", path, nil, h)
	}
	c.DoReq = func(_ context.Context, _ string, method, path string, body []byte, h akamapi.Header) (int, []byte, error) {
		return f.serve(method, path, body, h)
	}
	return c
}

func (f *API2K) challenge() string {
	f.n++
	return strings.Repeat("0123456789abcdef", 2)[:30] + hex.EncodeToString([]byte{byte(f.n)})
}

func jerr(code int, e, d string) (int, []byte, error) {
	b, _ := json.Marshal(map[string]string{"error": e, "detail": d})
	return code, b, nil
}

func (f *API2K) serve(method, path string, body []byte, h akamapi.Header) (int, []byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.Closed {
		return 0, nil, errors.New("connection refused")
	}
	if path == "/akam/v1/hello" {
		f.Requests = append(f.Requests, Req2K{Method: method, Path: path})
		b, _ := json.Marshal(map[string]any{"api": 1, "clock_set": false, "challenge": f.challenge()})
		return 200, b, nil
	}
	ch := h.Get("X-Akam-Challenge")
	want, err := akamapi.Sign(f.Key, method, path, "", body, "challenge:"+ch, "challenge:"+ch)
	ok := err == nil && ch != "" && hmac.Equal([]byte(want.Get("X-Akam-Signature")), []byte(h.Get("X-Akam-Signature")))
	f.Requests = append(f.Requests, Req2K{Method: method, Path: path, Body: string(body), SignedOK: ok})
	if !ok {
		return jerr(401, "bad_signature", "-")
	}
	scope := f.Scope
	if scope == "" {
		scope = "admin"
	}
	switch {
	case method == "GET" && path == "/akam/v1/info":
		b, _ := json.Marshal(map[string]any{"model": "lsc-rotatable-2k-ak3918av130", "app": "0.2.0-test", "key_id": f.Key.ID, "scope": scope})
		return 200, b, nil
	case method == "GET" && path == "/akam/v1/settings":
		m := map[string]string{}
		for k, v := range f.Values {
			m[k] = v
		}
		for k, v := range f.Extra {
			m[k] = v
		}
		b, _ := json.Marshal(m)
		return 200, b, nil
	case method == "POST" && path == "/akam/v1/actions/audio.beep":
		if scope == "read" {
			return jerr(403, "scope", "write scope needed")
		}
		if f.Values["audio.out_enable"] != "1" {
			return jerr(409, "unavailable", "the speaker is not running (audio.out_enable 1, see audio.out_state)")
		}
		f.Beeps++
		return 200, []byte(`{"ok":true}`), nil
	case method == "PATCH" && path == "/akam/v1/settings":
		if scope == "read" {
			return jerr(403, "scope", "write scope needed")
		}
		var in map[string]string
		if json.Unmarshal(body, &in) != nil || len(in) == 0 {
			return jerr(400, "bad_json", "-")
		}
		keys := make([]string, 0, len(in))
		for k := range in {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		res := map[string]map[string]any{}
		bad := false
		for _, k := range keys {
			_, known := f.Values[k]
			if !known && f.WriteOnly[k] {
				known = true
			}
			if !known {
				res[k] = map[string]any{"ok": false, "error": "unknown_key"}
				bad = true
			} else if scope != "admin" && adminRow2K(k) {
				res[k] = map[string]any{"ok": false, "error": "scope"} // the user-file rows need an admin key
				bad = true
			} else if r, refused := f.Refuse[k]; refused {
				code, detail, _ := strings.Cut(r, "|")
				res[k] = map[string]any{"ok": false, "error": code, "detail": detail}
				bad = true
			}
		}
		if bad {
			for _, k := range keys {
				if _, done := res[k]; !done {
					res[k] = map[string]any{"ok": false, "error": "not_applied"}
				}
			}
			b, _ := json.Marshal(map[string]any{"results": res})
			return 422, b, nil
		}
		for _, k := range keys {
			ap := "applied"
			if adminRow2K(k) && !strings.HasPrefix(k, "rtsp.") {
				ap = "restart_component" // the supervisor writes the files; the app reads them at its next start
			}
			if a, o := f.Apply[k]; o {
				ap = a
			}
			if f.WriteOnly[k] {
				if f.Secrets == nil {
					f.Secrets = map[string]string{}
				}
				f.Secrets[k] = in[k]
				res[k] = map[string]any{"ok": true, "apply": ap} // never echoed
				continue
			}
			f.Values[k] = in[k]
			res[k] = map[string]any{"ok": true, "apply": ap, "value": in[k]}
		}
		b, _ := json.Marshal(map[string]any{"results": res})
		return 200, b, nil
	}
	return jerr(404, "unknown_path", "-")
}

// adminRow2K: the rows of the firmware's cfgreg.c need an admin key.
func adminRow2K(k string) bool {
	for _, p := range []string{"mqtt.", "onvif.", "rtsp.", "ntp.", "audio."} {
		if strings.HasPrefix(k, p) {
			return true
		}
	}
	return false
}
