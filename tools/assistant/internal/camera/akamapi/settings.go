package akamapi

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strings"
	"time"
)

// The settings calls of the API (docs/porting/lsc-rotatable-2k/control-api.md section 3): GET /settings for the
// writable values and PATCH /settings for a change. Both are signed in challenge mode like Info; the key's secret
// never appears in a request body, an error or a log line.

// ErrScope: the camera knows the key but it has no right to this call (a read key cannot PATCH).
var ErrScope = errors.New("the API key has no write scope")

// APIError is an error answer of the camera ({"error":"code","detail":"text"}); the texts are the camera's own.
type APIError struct {
	Status int
	Code   string
	Detail string
}

func (e *APIError) Error() string {
	s := fmt.Sprintf("HTTP %d", e.Status)
	if e.Code != "" {
		s += " " + e.Code
	}
	if e.Detail != "" && e.Detail != "-" {
		s += ": " + e.Detail
	}
	return s
}

// KeyResult is the camera's answer for one key of a PATCH.
type KeyResult struct {
	OK    bool
	Apply string // applied | restart_component | pending_device_restart
	Value string
	// NotSaved: applied, but the camera could not write it to flash (the reply says "saved":false): lost at the next reboot.
	NotSaved bool
	Error    string // range, format, check, unknown_key, ...
	Detail   string
}

// PatchResult is the answer of PATCH /settings: HTTP status 200 (all applied), 207 (a setter failed after others
// succeeded) or 422 (nothing applied), and one result per key.
type PatchResult struct {
	Status  int
	Results map[string]KeyResult
}

var challengeRe = regexp.MustCompile(`^[0-9a-f]{32}$`)

// call does one signed request: GET /hello for a challenge, then the request itself.
func (c *Client) call(ctx context.Context, method, path string, body []byte) ([]byte, int, error) {
	hb, code, err := c.get(ctx, "/hello", false, "")
	if err != nil {
		return nil, 0, err
	}
	if code == 404 {
		return nil, 0, fmt.Errorf("%w (HTTP 404)", ErrClosed)
	}
	if code != 200 {
		return nil, 0, fmt.Errorf("GET /hello: HTTP %d", code)
	}
	var hello struct {
		API       int    `json:"api"`
		Challenge string `json:"challenge"`
	}
	if json.Unmarshal(hb, &hello) != nil || hello.API != 1 || !challengeRe.MatchString(hello.Challenge) {
		return nil, 0, errors.New("GET /hello: not the Akam control API")
	}
	if method == "GET" {
		b, code, err := c.get(ctx, path, true, hello.Challenge)
		return b, code, err
	}
	h, err := Sign(c.Key, method, prefix+path, "", body, "challenge:"+hello.Challenge, "challenge:"+hello.Challenge)
	if err != nil {
		return nil, 0, err
	}
	to := c.Timeout
	if to == 0 {
		to = 5 * time.Second
	}
	ctx, cancel := context.WithTimeout(ctx, to)
	defer cancel()
	do := c.DoReq
	if do == nil {
		do = rawRequest
	}
	code, b, err := do(ctx, c.HostPort, method, prefix+path, body, h)
	if err != nil {
		return nil, 0, fmt.Errorf("%w: %v", ErrClosed, redactErr(err))
	}
	return b, code, nil
}

// apiError reads an error answer; ok is false when the body is not one.
func apiError(code int, b []byte) *APIError {
	var e struct {
		Error  string `json:"error"`
		Detail string `json:"detail"`
	}
	_ = json.Unmarshal(b, &e)
	return &APIError{Status: code, Code: e.Error, Detail: e.Detail}
}

func authError(code int) error {
	switch code {
	case 401:
		return ErrAuth
	case 403:
		return ErrScope
	}
	return nil
}

// Settings reads the writable settings: GET /settings, key to canonical text.
func (c *Client) Settings(ctx context.Context) (map[string]string, error) {
	return c.readMap(ctx, "/settings")
}

// State reads the read-only state rows: GET /state, key to canonical text (measures, service states, versions).
func (c *Client) State(ctx context.Context) (map[string]string, error) {
	return c.readMap(ctx, "/state")
}

func (c *Client) readMap(ctx context.Context, path string) (map[string]string, error) {
	b, code, err := c.call(ctx, "GET", path, nil)
	if err != nil {
		return nil, err
	}
	if e := authError(code); e != nil {
		return nil, e
	}
	if code != 200 {
		return nil, apiError(code, b)
	}
	var raw map[string]json.RawMessage
	if json.Unmarshal(b, &raw) != nil {
		return nil, errors.New("GET " + path + ": unreadable answer")
	}
	out := make(map[string]string, len(raw))
	for k, v := range raw {
		var s string
		if json.Unmarshal(v, &s) != nil {
			s = strings.TrimSpace(string(v))
		}
		out[k] = s
	}
	return out, nil
}

var actionNameRe = regexp.MustCompile(`^[a-z][a-z0-9_.]{0,39}$`)

// Action runs one action of the camera (POST /actions/<name>, the arguments a flat JSON object of strings, nil for none).
// The camera's error answer comes back as an *APIError; a 200 answer is success.
func (c *Client) Action(ctx context.Context, name string, args map[string]string) error {
	if !actionNameRe.MatchString(name) {
		return errors.New("action name refused")
	}
	body := []byte("{}")
	if len(args) > 0 {
		var err error
		if body, err = PatchBody(args); err != nil {
			return err
		}
	}
	b, code, err := c.call(ctx, "POST", "/actions/"+name, body)
	if err != nil {
		return err
	}
	if e := authError(code); e != nil {
		return e
	}
	if code != 200 {
		return apiError(code, b)
	}
	return nil
}

// PatchBody is the JSON body of a PATCH /settings for these values (every value as a JSON string, the canonical
// text the registry parses; keys sorted).
func PatchBody(kv map[string]string) ([]byte, error) {
	b, err := json.Marshal(kv)
	if err != nil {
		return nil, err
	}
	if len(b) > 8*1024 {
		return nil, errors.New("too many settings for one request (the camera accepts 8 KiB)")
	}
	return b, nil
}

// Patch sends the change in ONE request (all-or-nothing validation on the camera). A 200, 207 or 422 answer that
// carries per-key results is returned as a PatchResult; the other answers are errors.
func (c *Client) Patch(ctx context.Context, kv map[string]string) (PatchResult, error) {
	body, err := PatchBody(kv)
	if err != nil {
		return PatchResult{}, err
	}
	b, code, err := c.call(ctx, "PATCH", "/settings", body)
	if err != nil {
		return PatchResult{}, err
	}
	if e := authError(code); e != nil {
		return PatchResult{}, e
	}
	var rep struct {
		Results map[string]struct {
			OK     bool   `json:"ok"`
			Apply  string `json:"apply"`
			Saved  *bool  `json:"saved"`
			Value  any    `json:"value"`
			Error  string `json:"error"`
			Detail string `json:"detail"`
		} `json:"results"`
	}
	if (code == 200 || code == 207 || code == 422) && json.Unmarshal(b, &rep) == nil && len(rep.Results) > 0 {
		res := PatchResult{Status: code, Results: map[string]KeyResult{}}
		keys := make([]string, 0, len(rep.Results))
		for k := range rep.Results {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			r := rep.Results[k]
			v := ""
			switch x := r.Value.(type) {
			case string:
				v = x
			case nil:
			default:
				vb, _ := json.Marshal(x)
				v = string(vb)
			}
			res.Results[k] = KeyResult{OK: r.OK, Apply: r.Apply, Value: v, NotSaved: r.Saved != nil && !*r.Saved, Error: r.Error, Detail: r.Detail}
		}
		return res, nil
	}
	return PatchResult{}, apiError(code, b)
}
