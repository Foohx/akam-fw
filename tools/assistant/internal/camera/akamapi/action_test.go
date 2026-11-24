package akamapi

import (
	"context"
	"errors"
	"testing"
)

func TestStateRead(t *testing.T) {
	c := settingsClient(func(m, p string, body []byte, h Header) (int, string) {
		if m != "GET" || p != "/akam/v1/state" || h.Get("X-Akam-Challenge") != chal {
			t.Errorf("%s %s %v", m, p, h)
		}
		return 200, `{"sys.version":"b35-x","video0.fps_x10":"-1000000","rtsp.clients":3}`
	})
	st, err := c.State(context.Background())
	if err != nil || st["sys.version"] != "b35-x" || st["rtsp.clients"] != "3" {
		t.Fatalf("%v %v", st, err)
	}
}

func TestActionPostsTheSignedBody(t *testing.T) {
	var gotBody []byte
	var gotH Header
	code := 200
	c := settingsClient(func(m, p string, body []byte, h Header) (int, string) {
		if m != "POST" || p != "/akam/v1/actions/audio.beep" {
			t.Errorf("%s %s", m, p)
		}
		gotBody, gotH = body, h
		if code != 200 {
			return code, `{"error":"audio","detail":"the speaker is off"}`
		}
		return 200, `{"ok":true}`
	})
	if err := c.Action(context.Background(), "audio.beep", nil); err != nil {
		t.Fatal(err)
	}
	if string(gotBody) != "{}" {
		t.Errorf("body %q", gotBody)
	}
	want, _ := Sign(c.Key, "POST", "/akam/v1/actions/audio.beep", "", gotBody, "challenge:"+chal, "challenge:"+chal)
	if want.Get("X-Akam-Signature") != gotH.Get("X-Akam-Signature") {
		t.Error("the signature does not cover the POST")
	}
	code = 422
	var ae *APIError
	if err := c.Action(context.Background(), "audio.beep", nil); !errors.As(err, &ae) || ae.Status != 422 || ae.Code != "audio" {
		t.Errorf("error %v", err)
	}
	code = 403
	if err := c.Action(context.Background(), "audio.beep", nil); !errors.Is(err, ErrScope) {
		t.Errorf("error %v", err)
	}
	for _, bad := range []string{"", "../x", "A", "a b", "a/b"} {
		if err := c.Action(context.Background(), bad, nil); err == nil {
			t.Errorf("action name %q accepted", bad)
		}
	}
}
