package akamapi

import (
	"context"
	"testing"
)

func TestPatchReadsSavedFalse(t *testing.T) {
	c := settingsClient(func(m, p string, body []byte, h Header) (int, string) {
		return 200, `{"results":{"led.mode":{"ok":true,"apply":"applied","saved":false,"value":"off"},"night.mode":{"ok":true,"apply":"applied","value":"day"}}}`
	})
	res, err := c.Patch(context.Background(), map[string]string{"led.mode": "off", "night.mode": "day"})
	if err != nil || !res.Results["led.mode"].NotSaved || res.Results["night.mode"].NotSaved {
		t.Fatalf("%+v %v", res, err)
	}
}
