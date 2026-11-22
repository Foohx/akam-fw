package akamapi

import (
	"context"
	"encoding/hex"
	"errors"
	"net"
	"strings"
	"testing"
	"time"
)

func settingsClient(do func(method, path string, body []byte, h Header) (int, string)) *Client {
	sec, _ := hex.DecodeString(strings.Repeat("01", 32))
	c := &Client{HostPort: "192.0.2.10:8092", Key: Key{ID: "3fa91c07", Secret: sec}}
	c.Do = func(_ context.Context, _ string, path string, h Header) (int, []byte, error) {
		if path == "/akam/v1/hello" {
			return reply(200, `{"api":1,"clock_set":false,"challenge":"`+chal+`"}`)
		}
		code, b := do("GET", path, nil, h)
		return code, []byte(b), nil
	}
	c.DoReq = func(_ context.Context, _ string, method, path string, body []byte, h Header) (int, []byte, error) {
		code, b := do(method, path, body, h)
		return code, []byte(b), nil
	}
	return c
}

func TestPatchSignsTheBody(t *testing.T) {
	var gotBody []byte
	var gotH Header
	c := settingsClient(func(m, p string, body []byte, h Header) (int, string) {
		if m != "PATCH" || p != "/akam/v1/settings" {
			t.Errorf("%s %s", m, p)
		}
		gotBody, gotH = body, h
		return 200, `{"results":{"led.mode":{"ok":true,"apply":"applied","value":"off"},"image.flip":{"ok":true,"apply":"pending_device_restart","value":1}}}`
	})
	res, err := c.Patch(context.Background(), map[string]string{"led.mode": "off", "image.flip": "1"})
	if err != nil || res.Status != 200 || !res.Results["led.mode"].OK || res.Results["image.flip"].Apply != "pending_device_restart" || res.Results["image.flip"].Value != "1" {
		t.Fatalf("%+v %v", res, err)
	}
	if string(gotBody) != `{"image.flip":"1","led.mode":"off"}` {
		t.Errorf("body %s", gotBody)
	}
	want, _ := Sign(c.Key, "PATCH", "/akam/v1/settings", "", gotBody, "challenge:"+chal, "challenge:"+chal)
	if want.Get("X-Akam-Signature") != gotH.Get("X-Akam-Signature") || gotH.Get("X-Akam-Challenge") != chal {
		t.Errorf("the signature does not cover the body: %v", gotH)
	}
	if strings.Contains(string(gotBody), "0101") || strings.Contains(gotH.Get("X-Akam-Signature"), strings.Repeat("01", 32)) {
		t.Error("the secret leaked")
	}
}

func TestPatchAnswers(t *testing.T) {
	cases := []struct {
		code    int
		body    string
		wantErr error
		status  int
		apiErr  string
	}{
		{422, `{"results":{"video0.fps":{"ok":false,"error":"range","detail":"[1,15]"},"led.mode":{"ok":false,"error":"not_applied"}}}`, nil, 422, ""},
		{207, `{"results":{"a":{"ok":true,"apply":"applied","value":"1"},"b":{"ok":false,"error":"setter"}}}`, nil, 207, ""},
		{401, `{"error":"bad_signature","detail":"-"}`, ErrAuth, 0, ""},
		{403, `{"error":"scope","detail":"-"}`, ErrScope, 0, ""},
		{429, `{"error":"rate","detail":"slow down"}`, nil, 0, "HTTP 429 rate: slow down"},
		{400, `{"error":"bad_json","detail":"-"}`, nil, 0, "HTTP 400 bad_json"},
		{500, `<html>`, nil, 0, "HTTP 500"},
	}
	for _, tc := range cases {
		c := settingsClient(func(string, string, []byte, Header) (int, string) { return tc.code, tc.body })
		res, err := c.Patch(context.Background(), map[string]string{"a": "1"})
		switch {
		case tc.wantErr != nil:
			if !errors.Is(err, tc.wantErr) {
				t.Errorf("%d: %v", tc.code, err)
			}
		case tc.apiErr != "":
			var ae *APIError
			if !errors.As(err, &ae) || ae.Error() != tc.apiErr {
				t.Errorf("%d: %v", tc.code, err)
			}
		default:
			if err != nil || res.Status != tc.status || len(res.Results) == 0 {
				t.Errorf("%d: %+v %v", tc.code, res, err)
			}
		}
	}
	c := settingsClient(func(string, string, []byte, Header) (int, string) {
		return 422, `{"results":{"video0.fps":{"ok":false,"error":"range","detail":"[1,15]"}}}`
	})
	res, _ := c.Patch(context.Background(), map[string]string{"video0.fps": "99"})
	if r := res.Results["video0.fps"]; r.OK || r.Error != "range" || r.Detail != "[1,15]" {
		t.Errorf("%+v", r)
	}
}

func TestSettingsRead(t *testing.T) {
	c := settingsClient(func(m, p string, _ []byte, h Header) (int, string) {
		if m != "GET" || p != "/akam/v1/settings" || h.Get("X-Akam-Signature") == "" {
			t.Errorf("%s %s %v", m, p, h)
		}
		return 200, `{"led.mode":"on","image.flip":"0","video0.kbps":1536}`
	})
	m, err := c.Settings(context.Background())
	if err != nil || m["led.mode"] != "on" || m["video0.kbps"] != "1536" || len(m) != 3 {
		t.Fatalf("%v %v", m, err)
	}
	c = settingsClient(func(string, string, []byte, Header) (int, string) { return 403, `{}` })
	if _, err := c.Settings(context.Background()); !errors.Is(err, ErrScope) {
		t.Errorf("%v", err)
	}
	c = settingsClient(func(string, string, []byte, Header) (int, string) { return 200, `[1]` })
	if _, err := c.Settings(context.Background()); err == nil {
		t.Error("an array was accepted")
	}
	c.Do = func(context.Context, string, string, Header) (int, []byte, error) { return reply(404, "") }
	if _, err := c.Settings(context.Background()); !errors.Is(err, ErrClosed) {
		t.Errorf("%v", err)
	}
}

func TestPatchBodyLimit(t *testing.T) {
	big := map[string]string{}
	for i := 0; i < 400; i++ {
		big["k"+strings.Repeat("x", 20)+string(rune('a'+i%26))+strings.Repeat("y", i/26)] = strings.Repeat("v", 20)
	}
	if _, err := PatchBody(big); err == nil {
		t.Error("a body over 8 KiB was accepted")
	}
}

// the raw client sends a PATCH with its length and body
func TestRawPatchAgainstALocalServer(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip("skip: no loopback")
	}
	defer ln.Close()
	got := make(chan string, 1)
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		buf := make([]byte, 4096)
		n, _ := c.Read(buf)
		got <- string(buf[:n])
		_, _ = c.Write([]byte("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"))
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	code, body, err := rawRequest(ctx, ln.Addr().String(), "PATCH", "/akam/v1/settings", []byte(`{"a":"1"}`), Header{"X-Akam-Key": "3fa91c07"})
	if err != nil || code != 200 || string(body) != "{}" {
		t.Fatal(code, string(body), err)
	}
	req := <-got
	if !strings.HasPrefix(req, "PATCH /akam/v1/settings HTTP/1.1\r\n") || !strings.Contains(req, "Content-Length: 9\r\n") || !strings.HasSuffix(req, "\r\n\r\n{\"a\":\"1\"}") {
		t.Fatalf("request %q", req)
	}
	if _, _, err := rawRequest(ctx, "127.0.0.1:1", "DELETE", "/x", nil, nil); err == nil {
		t.Fatal("a method outside GET and PATCH was sent")
	}
}
