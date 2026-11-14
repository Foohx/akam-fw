//go:build akamtest

package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camopt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Review round on dev (5a2ef00): the first-install choice that must stay pending, what counts as a clean
// apply, a config that does not mention a key, partial reads, key paths, MQTT options, saved passwords.

func ncCount(e *env) int {
	n := 0
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "nc -w 2") {
			n++
		}
	}
	return n
}

// setPending marks camera salon as having chosen the global config at the first install.
func (e *env) setPending() {
	e.t.Helper()
	if err := os.WriteFile(filepath.Join(e.state, "cameras", "salon", "state"),
		[]byte(e.stateTextOf("salon")+"config_source=global\nconfig_pending=1\n"), 0o600); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) stateTextOf(cam string) string {
	b, _ := os.ReadFile(filepath.Join(e.state, "cameras", cam, "state"))
	return string(b)
}

// ---------------------------------------------------------------- 1, 3: pending and clean

func TestAnApplyThatIsNotCleanKeepsTheFirstInstallChoicePending(t *testing.T) {
	// a helper failure: ONVIF needs the video password, which the camera does not have yet
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off", "onvif.enabled", "1")
	e.setPending()
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 1)
	if e.stateKey("config_pending") != "1" || e.stateKey("config_source") != "global" {
		t.Errorf("source %q pending %q", e.stateKey("config_source"), e.stateKey("config_pending"))
	}
	contains(t, r.out, "stays pending")
	// the card step still offers what the choice holds, and the next apply can finish it
	e.auth("viewer")
	r = e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if e.stateKey("config_pending") != "" || e.stateKey("config_source") != "global" {
		t.Errorf("after the fix: source %q pending %q", e.stateKey("config_source"), e.stateKey("config_pending"))
	}
}

func TestAConfigPartLeftOutIsNotAFullApply(t *testing.T) {
	e := cfgEnv(t)
	e.cam.SetCtl("video.gop_cfg", "-") // an older image without that option
	e.writeCfg(e.globalPlace(), "led.status", "off", "video.gop_cfg", "3")
	e.setPending()
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Not applied: Key frame interval", "1 applied, 0 failed, 0 not applied.")
	if e.stateKey("config_pending") != "1" || e.stateKey("config_source") != "global" {
		t.Errorf("a part was left out yet source %q pending %q", e.stateKey("config_source"), e.stateKey("config_pending"))
	}
	// without a pending choice it is plainly "local"
	e2 := cfgEnv(t)
	e2.cam.SetCtl("video.gop_cfg", "-")
	e2.writeCfg(e2.globalPlace(), "led.status", "off", "video.gop_cfg", "3")
	r = e2.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if e2.stateKey("config_source") != "local" {
		t.Errorf("config_source %q", e2.stateKey("config_source"))
	}
}

func TestADeclinedKeyBoxDuringTheFirstInstallApplyKeepsItPending(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	e.writeCfg(e.globalPlace(), "key.kind", "custom", "key.path", work+".pub")
	e.writeState(e.stateText() + "config_source=global\nconfig_pending=1\n")
	r := e.keyRun("y\n\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if e.readState("config_pending") != "1" || e.readState("config_source") != "global" {
		t.Errorf("source %q pending %q", e.readState("config_source"), e.readState("config_pending"))
	}
}

// ---------------------------------------------------------------- 2, 6: MQTT

func TestAConfigWithoutAMQTTUserKeepsTheCamerasUser(t *testing.T) {
	e := cfgEnv(t)
	r := e.cr("broker.lan\n1883\nbob\n"+pwMqtt+"\ny\nn\n", "mqtt", "set")
	expectRC(t, r, 0)
	e.writeCfg(e.globalPlace(), "mqtt.enabled", "1", "mqtt.host", "broker.lan")
	n := writesToTheCamera(e)
	r = e.cr("", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Nothing to change")
	if writesToTheCamera(e) != n {
		t.Error("a config without a user wrote to the camera")
	}
	// another broker: the camera's user stays, the user is never turned into "-"
	e.writeCfg(e.globalPlace(), "mqtt.enabled", "1", "mqtt.host", "other.lan")
	r = e.cr("y\n"+pwMqtt+"\n"+pwMqtt+"\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "on, other.lan:1883, user bob")
	var found bool
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "akam_mqtt.sh set other.lan 1883 bob") {
			found = true
		}
		if strings.Contains(c, "akam_mqtt.sh set other.lan 1883 -") {
			t.Error("the camera's broker user was removed")
		}
	}
	if !found {
		t.Error("the broker change was not sent with the camera's user")
	}
}

func TestMQTTOptionsNeedABrokerToAttachTo(t *testing.T) {
	e := cfgEnv(t)
	// no broker anywhere: the options are reported, not queued to fail
	e.writeCfg(e.globalPlace(), "mqtt.enabled", "1", "mqtt.base_topic", "home/salon", "led.status", "off")
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Not applied: MQTT: the config has no broker address", "Not applied: MQTT options: no broker is saved on the camera")
	notContains(t, r.out+r.err, "FAILED")
	// a broker whose password could not be given: its options are skipped with a reason, not failed
	e2 := cfgEnv(t)
	e2.writeCfg(e2.globalPlace(), "mqtt.enabled", "1", "mqtt.host", "broker.lan", "mqtt.user", "bob", "mqtt.base_topic", "home/salon")
	r = e2.cr("y\n"+pwMqtt+"\nsomething-else\n", "config", "apply", "--global") // the two entries differ
	expectRC(t, r, 1)
	contains(t, r.out, "the broker settings were not applied")
	notContains(t, r.err+r.out, "no MQTT broker saved yet")
}

// ---------------------------------------------------------------- 4: a partial read never wipes the record

func TestAPartialReadKeepsTheEarlierRecordForWhatItCouldNotRead(t *testing.T) {
	e := newEnvNoNtp(t)
	e.writeCfg(e.cameraPlace("salon"), "ntp.enabled", "1", "ntp.servers", "pool.ntp.org", "led.status", "on")
	e.writeCfg(e.globalPlace(), "led.status", "off")
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	loc := e.loadCfg(e.cameraPlace("salon"))
	if loc.Get("ntp.enabled") != "1" || loc.Get("ntp.servers") != "pool.ntp.org" {
		t.Errorf("the group that could not be read was wiped: %v", loc.Vals)
	}
	if loc.Get("led.status") != "off" {
		t.Errorf("what was read must be the new value: %v", loc.Vals)
	}
	contains(t, r.out, "value(s) of the earlier record were kept for them")
}

// ---------------------------------------------------------------- 5: key paths

func TestACustomKeyPathWrittenWithATildeIsTheSameKey(t *testing.T) {
	e := cfgEnv(t)
	// the camera is recorded with the absolute file; the config says ~ (the diff itself is in configkey_internal_test.go)
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	// the form stores the absolute file
	e2 := cfgEnv(t)
	u := &cfgUI{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			if row, err := spec.Apply("key.path", "~/.ssh/id_work.pub"); err != nil || row.Value != filepath.Join(e2.home, ".ssh", "id_work.pub") {
				t.Errorf("row %+v err %v", row, err)
			}
		}
		return ui.FormResult{}
	}}
	runCfgUI(t, e2, u, "", "--camera", "salon", "config", "edit-global")
	if got := e2.loadCfg(e2.globalPlace()).Get("key.path"); got != filepath.Join(e2.home, ".ssh", "id_work.pub") {
		t.Errorf("key.path = %q", got)
	}
}

// ---------------------------------------------------------------- 7: the saved password just sent

func TestAPasswordJustSentIsNotDeletedFromTheCamerasSavedSecrets(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "auth.enabled", "1", "auth.user", "viewer")
	e.writeSecrets(e.globalPlace(), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	e.writeSecrets(e.cameraPlace("salon"), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	sec, err := profile.Load(e.cameraPlace("salon"), profile.Secrets)
	if err != nil || sec.Get(profile.AuthPassword) != pwStream {
		t.Errorf("the saved copy that was just sent was deleted: %v %v", sec, err)
	}
	notContains(t, r.out, "out of date")
	// a different password was sent: the saved copy IS out of date
	e.writeSecrets(e.cameraPlace("salon"), profile.AuthPassword, "older-pass-1111", profile.AuthPasswordUser, "viewer")
	e.writeCfg(e.globalPlace(), "auth.enabled", "1", "auth.user", "viewer2")
	e.writeSecrets(e.globalPlace(), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer2")
	r = e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if profile.Exists(e.cameraPlace("salon"), profile.Secrets) {
		t.Error("an out of date copy stayed")
	}
}

// ---------------------------------------------------------------- 8: one read

func TestANoChangeApplyReadsTheCameraOnce(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "auto")
	r := e.cr("", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Nothing to change")
	if n := ncCount(e); n != 1 {
		t.Errorf("%d control-port exchanges for a no-change apply, want 1", n)
	}
}

// ---------------------------------------------------------------- 10: every key is shown

func TestEveryKeyOfTheFormatIsInShowAndInTheForm(t *testing.T) {
	e := cfgEnv(t)
	vals := map[string]string{profile.OnvifEnabled: "1", profile.OnvifPort: "8080", profile.NTPEnabled: "1", profile.NTPServers: "pool.ntp.org",
		profile.MQTTEnabled: "1", profile.MQTTHost: "broker.lan", profile.MQTTPort: "1883", profile.MQTTUser: "bob", profile.MQTTBaseTopic: "a/b",
		profile.MQTTClientID: "salon", profile.MQTTDiscovery: "on", profile.MQTTPrefix: "homeassistant", profile.MQTTInterval: "30",
		profile.AuthEnabled: "1", profile.AuthUser: "viewer", profile.KeyKind: "custom", profile.KeyPath: "/k/id.pub"}
	var kv []string
	keys := profile.SettingKeys()
	for _, k := range keys {
		v, ok := vals[k]
		if !ok {
			o, found := camopt.Find(k)
			if !found {
				t.Fatalf("%s has no sample value", k)
			}
			if len(o.Values) > 0 {
				v = o.Values[0]
			}
			if o.Kind == camopt.Int {
				v = itoa(o.Min)
				if o.Min < 0 {
					v = "-" + itoa(-o.Min)
				}
			}
		}
		kv = append(kv, k, v)
	}
	e.writeCfg(e.globalPlace(), kv...)
	r := e.cr("", "config", "show", "--global")
	expectRC(t, r, 0)
	lines := 0
	for _, l := range strings.Split(r.out, "\n") {
		if strings.HasPrefix(l, "    ") {
			lines++
		}
	}
	if lines != len(keys) {
		t.Errorf("show lists %d settings, the format has %d:\n%s", lines, len(keys), r.out)
	}
	var rows map[string]bool
	u := &cfgUI{script: func(n int, spec ui.FormSpec) ui.FormResult {
		rows = map[string]bool{}
		for _, row := range spec.Rows {
			rows[row.ID] = true
		}
		return ui.FormResult{}
	}}
	runCfgUI(t, e, u, "", "--camera", "salon", "config", "edit-global")
	for _, k := range keys {
		if !rows[k] {
			t.Errorf("the form has no row for %s", k)
		}
	}
	_ = testfake.Options{}
}
