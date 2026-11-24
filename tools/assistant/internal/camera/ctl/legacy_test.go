package ctl

import (
	"context"
	"errors"
	"reflect"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
)

type fakeClient struct {
	settings, state map[string]string
	patchRes        akamapi.PatchResult
	patchErr        error
	actionErr       error
	reads           []string
	patches         []map[string]string
	actions         []string
}

func (f *fakeClient) Settings(context.Context) (map[string]string, error) {
	f.reads = append(f.reads, "settings")
	return f.settings, nil
}
func (f *fakeClient) State(context.Context) (map[string]string, error) {
	f.reads = append(f.reads, "state")
	return f.state, nil
}
func (f *fakeClient) Patch(_ context.Context, kv map[string]string) (akamapi.PatchResult, error) {
	f.patches = append(f.patches, kv)
	return f.patchRes, f.patchErr
}
func (f *fakeClient) Action(_ context.Context, name string, _ map[string]string) error {
	f.actions = append(f.actions, name)
	return f.actionErr
}

func TestGetTranslatesNamesAndReadsOnlyWhatItNeeds(t *testing.T) {
	f := &fakeClient{
		settings: map[string]string{"led.mode": "auto", "image.contrast": "-5", "video0.kbps": "1536", "video0.gop": "2", "audio.out_volume": "40"},
		state: map[string]string{"sys.version": "b35-x", "net.hostname": "salon", "night.active": "1", "net.wifi_rssi": "-1000000",
			"video0.fps_x10": "-1000000", "video0.bitrate_kbps": "1400", "sys.time_set": "1"},
	}
	got, err := Exchange(context.Background(), f, []string{"GET led.status", "GET isp.contrast", "GET video.bitrate_cfg", "GET video.gop_cfg", "GET audio.out_volume", "QUIT"})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"led.status=auto", "isp.contrast=-5", "video.bitrate_cfg=1536", "video.gop_cfg=2", "audio.out_volume=40"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("got %q", got)
	}
	if !reflect.DeepEqual(f.reads, []string{"settings"}) {
		t.Errorf("only settings rows were asked for, yet the camera was read: %v", f.reads)
	}
	f.reads = nil
	got, _ = Exchange(context.Background(), f, []string{"GET fw.version", "GET sys.hostname", "GET night.state", "GET net.wifi_rssi", "GET video.fps_actual", "GET video.bitrate", "GET net.mac", "GET ctl.debug", "GET isp.contrast"})
	want = []string{"fw.version=b35-x", "sys.hostname=salon", "night.state=night", "net.wifi_rssi=unknown", "video.fps_actual=unknown", "video.bitrate=1400", "isp.contrast=-5"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("state rows: got %q", got)
	}
	if !reflect.DeepEqual(f.reads, []string{"settings", "state"}) {
		t.Errorf("each of settings and state is read once: %v", f.reads)
	}
	for _, l := range got {
		if Get("net.wifi_rssi", got) != "unknown" {
			t.Errorf("the old parsers read the answer: %q", l)
		}
	}
}

func TestSetsTravelInOnePatchAndAnswerLikeTheOldPort(t *testing.T) {
	f := &fakeClient{patchRes: akamapi.PatchResult{Status: 200, Results: map[string]akamapi.KeyResult{
		"led.mode":         {OK: true, Apply: "applied", Value: "off"},
		"video0.min_qp":    {OK: true, Apply: "restart_component", Value: "30"},
		"night.ir_led":     {OK: true, Apply: "applied", Value: "off"},
		"audio.mic_enable": {OK: true, Apply: "restart_component", Value: "1"},
	}}}
	lines := []string{"SET led.status off", "SET video.minqp_cfg 30", "SET night.ir_led off", "SET audio.mic_enable 1"}
	got, err := Exchange(context.Background(), f, lines)
	if err != nil {
		t.Fatal(err)
	}
	if len(f.patches) != 1 || len(f.patches[0]) != 4 || f.patches[0]["led.mode"] != "off" || f.patches[0]["video0.min_qp"] != "30" {
		t.Fatalf("one PATCH with registry names expected: %v", f.patches)
	}
	want := []string{"OK audio.mic_enable=1 (restart to apply)", "OK led.status=off", "OK night.ir_led=off", "OK video.minqp_cfg=30 (restart to apply)"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("got %q", got)
	}
	if r := ParseLedSet("led.status", "off", got); !r.OK || !r.Answered {
		t.Errorf("ParseLedSet: %+v", r)
	}
	if r := ParseSet([]string{"OK video.minqp_cfg=30 (restart to apply)"}); !r.OK || !r.Restart {
		t.Errorf("ParseSet: %+v", r)
	}
}

func TestRefusedSetKeepsTheOldErrorShapes(t *testing.T) {
	f := &fakeClient{patchRes: akamapi.PatchResult{Status: 422, Results: map[string]akamapi.KeyResult{
		"image.contrast": {Error: "range", Detail: "[-50,50]"},
		"led.mode":       {Error: "not_applied"},
		"net.hostname":   {Error: "unknown_key"},
	}}}
	got, _ := Exchange(context.Background(), f, []string{"SET isp.contrast 99", "SET led.status off", "SET sys.hostname x", "SET net.mac y"})
	want := []string{"ERR unknown param net.mac", "ERR invalid isp.contrast value 99 (range [-50,50])", "ERR led.status not applied: another setting of the same change was refused", "ERR unknown param sys.hostname"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("got %q", got)
	}
	if r := ParseSet(got[1:2]); r.OK || !r.Answered || r.Err == "" {
		t.Errorf("ParseSet: %+v", r)
	}
	if r := ParseLedSet("led.status", "off", got); r.OK || !r.Answered {
		t.Errorf("ParseLedSet: %+v", r)
	}
}

func TestBeepIsTheAction(t *testing.T) {
	f := &fakeClient{}
	got, err := Exchange(context.Background(), f, []string{"SET audio.out_test 1"})
	if err != nil || !reflect.DeepEqual(got, []string{"OK audio.out_test=1"}) || !reflect.DeepEqual(f.actions, []string{"audio.beep"}) || len(f.patches) != 0 {
		t.Fatalf("%v %v %v %v", got, err, f.actions, f.patches)
	}
	f.actionErr = &akamapi.APIError{Status: 422, Code: "audio", Detail: "the speaker is off"}
	got, _ = Exchange(context.Background(), f, []string{"SET audio.out_test 1"})
	if len(got) != 1 || got[0] != "ERR audio.out_test: HTTP 422 audio: the speaker is off" {
		t.Errorf("got %q", got)
	}
}

func TestTransportErrorsAreReturnedNotHidden(t *testing.T) {
	f := &fakeClient{patchErr: akamapi.ErrAuth}
	got, err := Exchange(context.Background(), f, []string{"SET led.status off"})
	if !errors.Is(err, akamapi.ErrAuth) || len(got) != 0 {
		t.Errorf("%v %v", got, err)
	}
	f = &fakeClient{patchErr: &akamapi.APIError{Status: 429, Code: "flash_rate"}}
	got, err = Exchange(context.Background(), f, []string{"SET led.status off"})
	if err != nil || len(got) != 1 || got[0] != "ERR led.status: the camera answered HTTP 429 flash_rate" {
		t.Errorf("%v %v", got, err)
	}
}

func TestEveryLegacyKeyOfTheCatalogueHasARow(t *testing.T) {
	for _, k := range []string{"led.status", "night.ir_led", "night.mode", "isp.brightness", "isp.contrast", "isp.saturation", "isp.sharpness", "isp.denoise",
		"img.wb", "image.flip", "image.mirror", "image.ceiling", "audio.out_volume", "audio.out_enable", "audio.talk_enable", "audio.mic_enable",
		"video.bitrate_cfg", "video.fps_cfg", "video.gop_cfg", "video.minqp_cfg", "video.maxqp_cfg", "video.cfg_pending", "video.bitrate", "video.fps_actual",
		"net.wifi_rssi", "net.wifi_rate", "night.state", "sys.time_set", "fw.version", "sys.hostname"} {
		if _, ok := RegistryKey(k); !ok {
			t.Errorf("%s has no registry row", k)
		}
	}
	if _, ok := RegistryKey("net.mac"); ok {
		t.Error("net.mac is identity data and stays off the API")
	}
}
