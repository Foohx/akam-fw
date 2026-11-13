//go:build akamtest

package cli_test

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Configs: the global template, the local config of a camera, copy from another camera, Wi-Fi and the
// secrets as an opt-in, apply with the differences shown first. The camera is the fake one; the helper
// scripts of the camera (MQTT, video password) are the real ones in a fake root.

const (
	pwStream = "stream-pass-77"
	pwMqtt   = "mqtt-secret-55"
	pwWifi   = "wifi-secret-4242"
)

func cfgEnv(t *testing.T) *env {
	e := formEnv(t)
	t.Setenv("AKAM_WIFI_PASS", "")
	t.Setenv("AKAM_WIFI_SSID", "")
	if err := os.WriteFile(filepath.Join(e.state, "current_camera"), []byte("salon\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return e
}

// cr runs a config command on camera salon.
func (e *env) cr(stdin string, args ...string) result { return e.run(stdin, args...) }

func (e *env) globalPlace() profile.Place { return profile.Global(e.state) }
func (e *env) cameraPlace(n string) profile.Place {
	return profile.Camera(n, filepath.Join(e.state, "cameras", n))
}

func (e *env) writeCfg(p profile.Place, kv ...string) {
	e.t.Helper()
	f := profile.New(profile.Settings)
	for i := 0; i < len(kv); i += 2 {
		if err := f.Set(kv[i], kv[i+1]); err != nil {
			e.t.Fatalf("%s: %v", kv[i], err)
		}
	}
	if err := profile.Save(p, profile.Settings, f); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) writeSecrets(p profile.Place, kv ...string) {
	e.t.Helper()
	f := profile.New(profile.Secrets)
	for i := 0; i < len(kv); i += 2 {
		if err := f.Set(kv[i], kv[i+1]); err != nil {
			e.t.Fatalf("%s: %v", kv[i], err)
		}
	}
	if err := profile.Save(p, profile.Secrets, f); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) loadCfg(p profile.Place) *profile.File {
	e.t.Helper()
	f, err := profile.Load(p, profile.Settings)
	if err != nil {
		e.t.Fatal(err)
	}
	return f
}

// noSecretsAnywhere checks a run, the activity log and every command line the camera received.
func (e *env) noSecretsAnywhere(r result, secrets ...string) {
	e.t.Helper()
	all := r.out + r.err + e.logText()
	if b, err := os.ReadFile(filepath.Join(e.state, "cameras", "salon", "akam.log")); err == nil {
		all += string(b)
	}
	for _, c := range e.cam.Commands() {
		all += "\n" + c
	}
	notContains(e.t, all, secrets...)
}

func (e *env) stateKey(k string) string { return readStateOf(e.t, e, "salon", k) }

func (e *env) stateText() string {
	b, _ := os.ReadFile(filepath.Join(e.state, "state"))
	return string(b)
}

func sets(e *env) string { return strings.Join(e.cam.CtlSets(), "|") }

// writesToTheCamera counts the commands that change something on the camera.
func writesToTheCamera(e *env) int {
	n := 0
	for _, c := range e.cam.Commands() {
		for _, w := range []string{" set", "enable", "disable", "set-creds", "forget", "option "} {
			if strings.Contains(c, w) {
				n++
				break
			}
		}
	}
	return n + len(e.cam.CtlSets())
}

// ---------------------------------------------------------------- save

func TestConfigSaveGlobalReadsTheCameraAndAsksNothingElseByDefault(t *testing.T) {
	e := cfgEnv(t)
	e.cam.SetCtl("video.fps_cfg", "20")
	e.cam.SetCtl("video.bitrate_cfg", "2000")
	e.cam.SetCtl("led.status", "off")
	r := e.cr("\n", "config", "save-global") // the Wi-Fi question: Enter = No
	expectRC(t, r, 0)
	contains(t, r.out, "Saved the global config (")
	contains(t, r.err, "Also keep the Wi-Fi name and password in the global config? They are stored on this computer in a private file; anyone with access to your account can read it. [y/N]")
	notContains(t, r.err, "video and MQTT passwords") // no password is set on the camera: nothing to offer
	g := e.loadCfg(e.globalPlace())
	for k, v := range map[string]string{"video.fps_cfg": "20", "video.bitrate_cfg": "2000", "led.status": "off", "video.gop_cfg": "2",
		"onvif.enabled": "0", "ntp.enabled": "0", "mqtt.enabled": "0", "auth.enabled": "0", "key.kind": "shared", "saved.from": "camera salon"} {
		if g.Get(k) != v {
			t.Errorf("%s = %q, want %q", k, g.Get(k), v)
		}
	}
	if profile.Exists(e.globalPlace(), profile.Secrets) {
		t.Error("a secrets file was written without a yes")
	}
	if writesToTheCamera(e) != 0 {
		t.Errorf("save-global wrote to the camera: %v", e.cam.Commands())
	}
	if runtime.GOOS != "windows" {
		for _, p := range []string{e.globalPlace().Settings, filepath.Dir(e.globalPlace().Settings)} {
			st, err := os.Stat(p)
			if err != nil || st.Mode().Perm()&0o077 != 0 {
				t.Errorf("%s is not private: %v %v", p, st, err)
			}
		}
	}
	// show: the template as the user reads it
	r = e.cr("", "config", "show", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Global config (the default template for new cameras)", "Frame rate", "20", "Bitrate (kbit/s)", "2000",
		"Secrets: video password: not set   MQTT password: not set   Wi-Fi: not set", "Saved ", "from camera salon")
}

func TestConfigSecretsAndWifiAreOptInAndNeverLeak(t *testing.T) {
	e := cfgEnv(t)
	e.cam.SetCtl("net.ssid", "HomeNet")
	if r := e.cr(("viewer\n" + pwStream + "\n" + pwStream + "\n"), "rtsp-auth", "set"); r.rc != 0 {
		t.Fatalf("%s%s", r.out, r.err)
	}
	// MQTT on the camera: broker, port, user, password, yes, no test
	r := e.cr("broker.lan\n1883\nbob\n"+pwMqtt+"\ny\nn\n", "mqtt", "set")
	expectRC(t, r, 0)
	// 1st question No: nothing is saved, not even Wi-Fi
	r = e.cr("\n\n", "config", "save-global")
	expectRC(t, r, 0)
	if profile.Exists(e.globalPlace(), profile.Secrets) {
		t.Fatal("a secrets file without a yes")
	}
	contains(t, r.err, "Also keep the video and MQTT passwords in the global config? They are stored on this computer in a private file; anyone with access to your account can read it. [y/N]")
	// yes to the passwords (typed twice), yes to the Wi-Fi (the camera's network is the default)
	in := "y\n" + pwStream + "\n" + pwStream + "\n" + pwMqtt + "\n" + pwMqtt + "\ny\n\n" + pwWifi + "\n"
	r = e.cr("y\n"+in, "config", "save-global") // replace the existing global config? yes
	expectRC(t, r, 0)
	contains(t, r.out, "Secrets saved in a private file (video password: set   MQTT password: set   Wi-Fi: set)")
	sec, err := profile.Load(e.globalPlace(), profile.Secrets)
	if err != nil || sec.Get(profile.AuthPassword) != pwStream || sec.Get(profile.MQTTPassword) != pwMqtt || sec.Get(profile.WifiPassword) != pwWifi || sec.Get(profile.WifiSSID) != "HomeNet" {
		t.Fatalf("secrets: %v %v", sec, err)
	}
	if runtime.GOOS != "windows" {
		if st, _ := os.Stat(e.globalPlace().Secrets); st.Mode().Perm() != 0o600 {
			t.Errorf("secrets mode %v", st.Mode().Perm())
		}
	}
	// the settings file holds no secret at all
	b, _ := os.ReadFile(e.globalPlace().Settings)
	notContains(t, string(b), pwStream, pwMqtt, pwWifi)
	// show never prints a value
	r2 := e.cr("", "config", "show", "--global")
	expectRC(t, r2, 0)
	contains(t, r2.out, "video password: set   MQTT password: set   Wi-Fi: set")
	e.noSecretsAnywhere(r, pwStream, pwMqtt, pwWifi)
	e.noSecretsAnywhere(r2, pwStream, pwMqtt, pwWifi)
}

func TestConfigNoTerminalSavesSettingsOnly(t *testing.T) {
	e := cfgEnv(t)
	t.Setenv("AKAM_FORCE_TTY", "")
	r := e.cr("", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.out, "No terminal: passwords and Wi-Fi are not saved")
	if profile.Exists(e.globalPlace(), profile.Secrets) || !profile.Exists(e.globalPlace(), profile.Settings) {
		t.Error("files")
	}
}

func TestConfigReplacingTheGlobalConfigAsksAndDefaultsToNo(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "on")
	r := e.cr("\n", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.out, "Left as it was.")
	contains(t, r.err, "Replace it with the settings of camera salon (read now over SSH)? [y/N]")
	if e.loadCfg(e.globalPlace()).Get("led.status") != "on" {
		t.Error("replaced without a yes")
	}
}

func TestConfigForgetSecrets(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "on")
	e.writeSecrets(e.globalPlace(), profile.WifiSSID, "Net", profile.WifiPassword, pwWifi)
	e.writeCfg(e.cameraPlace("salon"), "led.status", "off")
	e.writeSecrets(e.cameraPlace("salon"), profile.MQTTPassword, pwMqtt)
	r := e.cr("\n", "config", "forget-secrets", "--global") // Enter = No
	expectRC(t, r, 0)
	contains(t, r.out, "Left as it was.", "Wi-Fi: set")
	notContains(t, r.out+r.err, pwWifi)
	if !profile.Exists(e.globalPlace(), profile.Secrets) {
		t.Fatal("deleted without a yes")
	}
	r = e.cr("y\n", "config", "forget-secrets", "--global")
	expectRC(t, r, 0)
	if profile.Exists(e.globalPlace(), profile.Secrets) || !profile.Exists(e.globalPlace(), profile.Settings) {
		t.Error("the secrets must go and the settings stay")
	}
	if !profile.Exists(e.cameraPlace("salon"), profile.Secrets) {
		t.Error("the camera's secrets were touched by the global forget")
	}
	r = e.cr("y\n", "config", "forget-secrets")
	expectRC(t, r, 0)
	if profile.Exists(e.cameraPlace("salon"), profile.Secrets) || !profile.Exists(e.cameraPlace("salon"), profile.Settings) {
		t.Error("camera forget")
	}
	r = e.cr("", "config", "forget-secrets")
	contains(t, r.out, "Nothing to forget")
	if writesToTheCamera(e) != 0 {
		t.Error("forgetting touched the camera")
	}
}

// ---------------------------------------------------------------- apply

func TestConfigApplyShowsTheDifferencesAndDefaultsToNo(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off", "video.fps_cfg", "20", "video.bitrate_cfg", "2000", "isp.brightness", "5", "video.gop_cfg", "2")
	r := e.cr("\n", "config", "apply", "--global") // Enter = No
	expectRC(t, r, 0)
	contains(t, r.out, "Config: the global config. Camera: salon (settings read just now).", "4 change(s) from the camera's current settings:",
		"LED and night", "Front LED", "auto  ->  off", "Frame rate", "15  ->  20", "Bitrate (kbit/s)", "1536  ->  2000", "Brightness", "0  ->  5", "Nothing was sent.")
	notContains(t, r.out, "Key frame interval") // already 2: not a difference
	contains(t, r.err, "Apply these 4 change(s) to camera salon? [y/N]")
	if n := writesToTheCamera(e); n != 0 {
		t.Errorf("%d writes without a yes: %v", n, e.cam.Commands())
	}
	if e.stateKey("config_source") != "" {
		t.Error("the source was recorded without an apply")
	}
	// yes: the SETs go through the same control-port path as the option screens; the restart is asked (default No)
	r = e.cr("y\n\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "WILL send: SET led.status off", "Front LED", "applied", "4 applied, 0 failed, 0 not applied.",
		"Saved on the camera. It applies when the video service restarts.", "Not restarted:")
	contains(t, r.err, "Restart the video service now?")
	if got := sets(e); !strings.Contains(got, "SET led.status off") || !strings.Contains(got, "SET video.fps_cfg 20") || !strings.Contains(got, "SET isp.brightness 5") {
		t.Errorf("sets: %s", got)
	}
	if e.cam.VideoRestarts() != 0 {
		t.Error("restarted without a yes")
	}
	if e.stateKey("config_source") != "global" {
		t.Errorf("config_source = %q", e.stateKey("config_source"))
	}
	loc := e.loadCfg(e.cameraPlace("salon"))
	if loc.Get("led.status") != "off" || loc.Get("video.fps_cfg") != "20" {
		t.Errorf("the local config is not what the camera has now: %v", loc.Vals)
	}
	// the same config again: nothing to do, nothing sent
	n := len(e.cam.CtlSets())
	r = e.cr("", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Nothing to change: the camera already has these settings")
	if len(e.cam.CtlSets()) != n {
		t.Error("an identical config sent SETs")
	}
	// the screens say where the settings came from
	r = e.cr("", "status")
	contains(t, r.out, "  Settings: local (from global)")
	r = e.cr("3\n8\n10\n")
	contains(t, r.out, "Camera options - salon (127.0.0.1)   Akam b35-2af81c5   settings: local (from global)")
	r = e.cr("", "config", "show")
	contains(t, r.out, "Local config of camera salon", "Settings of this camera: local (from global)", "Frame rate", "20")
}

func TestConfigApplyRestartIsOfferedAndTakenOnYes(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "video.fps_cfg", "25")
	r := e.cr("y\ny\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if e.cam.VideoRestarts() != 1 {
		t.Errorf("restarts: %d\n%s", e.cam.VideoRestarts(), r.out)
	}
	contains(t, r.out, "The video service was asked to restart")
}

func TestConfigApplyPartialFailureIsReportedPerItem(t *testing.T) {
	e := cfgEnv(t)
	// ONVIF needs the stream password, which this camera does not have: the helper refuses, the LED goes through
	e.writeCfg(e.globalPlace(), "led.status", "off", "onvif.enabled", "1", "onvif.port", "8080")
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 1)
	contains(t, r.out, "LED and night", "ONVIF", "off  ->  on, port 8080", "Front LED", "applied")
	contains(t, r.err, "ONVIF: FAILED: akam_onvif: ONVIF needs a password")
	contains(t, r.out, "1 applied, 1 failed, 0 not applied.")
	if got := sets(e); got != "SET led.status off" {
		t.Errorf("sets: %s", got)
	}
	// a failure never claims that the camera follows the config
	if e.stateKey("config_source") != "local" {
		t.Errorf("config_source = %q", e.stateKey("config_source"))
	}
}

func TestConfigApplyOnlyLimitsTheGroups(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off", "video.fps_cfg", "20", "isp.contrast", "9")
	r := e.cr("y\n\n", "config", "apply", "--global", "--only", "video,picture")
	expectRC(t, r, 0)
	contains(t, r.out, "2 change(s)", "Frame rate", "Contrast")
	notContains(t, r.out, "Front LED")
	if strings.Contains(sets(e), "led.status") {
		t.Error("a group outside --only was sent")
	}
	if e.stateKey("config_source") != "local" {
		t.Errorf("a partial apply must not say 'from global': %q", e.stateKey("config_source"))
	}
	r = e.cr("", "config", "apply", "--global", "--only", "wifi")
	expectRC(t, r, 2)
	contains(t, r.err, "--only: 'wifi' is not a group")
}

func TestConfigApplyHelpersSecretsAndOrder(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "auth.enabled", "1", "auth.user", "viewer", "onvif.enabled", "1", "onvif.port", "8080",
		"ntp.enabled", "1", "ntp.servers", "pool.ntp.org,192.0.2.1",
		"mqtt.enabled", "1", "mqtt.host", "broker.lan", "mqtt.port", "1884", "mqtt.user", "bob", "mqtt.base_topic", "home/akam/salon", "mqtt.discovery", "off")
	e.writeSecrets(e.globalPlace(), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer", profile.MQTTPassword, pwMqtt, profile.MQTTPasswordHost, "broker.lan", profile.MQTTPasswordUser, "bob", profile.WifiSSID, "Net", profile.WifiPassword, pwWifi)
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	// the diff names the passwords but never shows them
	contains(t, r.out, "Video password", "off  ->  on, user viewer, password: set (from the config)", "ONVIF", "on, port 8080",
		"Time sync", "on, pool.ntp.org 192.0.2.1", "MQTT broker", "on, broker.lan:1884, user bob, password: set (from the config)",
		"home/akam/salon", "applied")
	contains(t, r.out, "Passwords are sent only with the setting that needs them")
	// on the camera: the video password (before ONVIF, which needs it), ONVIF, time sync, MQTT with its options
	cred, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials"))
	if !strings.Contains(string(cred), "user=viewer") || !strings.Contains(string(cred), "pass="+pwStream) {
		t.Errorf("credentials: %q", cred)
	}
	mq, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "mqtt"))
	if !strings.Contains(string(mq), "host=broker.lan") || !strings.Contains(string(mq), "port=1884") || !strings.Contains(string(mq), "base_topic=home/akam/salon") || !strings.Contains(string(mq), "discovery=off") {
		t.Errorf("mqtt file: %q", mq)
	}
	mp, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "mqtt_pass"))
	if strings.TrimSpace(string(mp)) != pwMqtt {
		t.Errorf("mqtt password file: %q", mp)
	}
	var order []string
	for _, c := range e.cam.Commands() {
		for _, w := range []string{"rtsp_auth.sh set-creds", "akam_onvif.sh enable 8080", "akam_ntp.sh set pool.ntp.org 192.0.2.1", "akam_mqtt.sh set broker.lan 1884 bob", "akam_mqtt.sh option base_topic home/akam/salon"} {
			if strings.Contains(c, w) {
				order = append(order, w)
			}
		}
	}
	if len(order) != 5 || !strings.HasPrefix(order[0], "rtsp_auth") || !strings.HasPrefix(order[1], "akam_onvif") {
		t.Errorf("order: %v", order)
	}
	// secrets: not in argv, the log, the output; and the Wi-Fi was never applied to the running camera
	e.noSecretsAnywhere(r, pwStream, pwMqtt, pwWifi)
	for _, c := range e.cam.Commands() {
		if strings.Contains(strings.ToLower(c), "wifi") || strings.Contains(c, "wpa_supplicant") {
			t.Errorf("the Wi-Fi was sent to a running camera: %s", c)
		}
	}
	// local config: settings only, no secret copied next to it
	if profile.Exists(e.cameraPlace("salon"), profile.Secrets) {
		t.Error("applying a config copied the secrets to the camera's own files")
	}
	b, _ := os.ReadFile(e.cameraPlace("salon").Settings)
	notContains(t, string(b), pwStream, pwMqtt, pwWifi)
	// same config again: the passwords cannot be compared, so nothing is re-sent unless asked
	n := writesToTheCamera(e)
	r = e.cr("", "config", "apply", "--global")
	contains(t, r.out, "Nothing to change")
	if writesToTheCamera(e) != n {
		t.Error("an identical config wrote to the camera")
	}
	r = e.cr("y\n", "config", "apply", "--global", "--include-secrets")
	expectRC(t, r, 0)
	contains(t, r.out, "password: not comparable", "2 change(s)")
	if writesToTheCamera(e) <= n {
		t.Error("--include-secrets sent nothing")
	}
}

func TestConfigApplyAsksForAPasswordThatIsNotSaved(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "auth.enabled", "1", "auth.user", "viewer")
	r := e.cr("y\n"+pwStream+"\n"+pwStream+"\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "password: asked", "The video password is not saved in this config: type it now.")
	cred, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials"))
	if !strings.Contains(string(cred), "pass="+pwStream) {
		t.Errorf("credentials: %q", cred)
	}
	e.noSecretsAnywhere(r, pwStream)
}

func TestConfigApplyRemovingTheVideoPasswordNeedsItsOwnYes(t *testing.T) {
	e := cfgEnv(t)
	if r := e.cr("viewer\n"+pwStream+"\n"+pwStream+"\n", "rtsp-auth", "set"); r.rc != 0 {
		t.Fatal(r.out, r.err)
	}
	e.writeCfg(e.globalPlace(), "auth.enabled", "0")
	r := e.cr("y\n\n", "config", "apply", "--global") // apply yes, the red box: Enter = No
	expectRC(t, r, 0)
	contains(t, r.out, "Video password", "on, user viewer  ->  off", "This REMOVES the video password", "not confirmed")
	if _, err := os.Stat(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials")); err != nil {
		t.Error("the video password was removed without its own yes")
	}
	r = e.cr("y\ny\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	if _, err := os.Stat(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials")); err == nil {
		t.Error("the video password is still there after two yes")
	}
}

func TestConfigApplyDryRunAndUsage(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off")
	r := e.cr("", "--dry-run", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD read the config", "Wi-Fi is never applied to a running camera")
	r = e.cr("", "--dry-run", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD read the settings of the camera")
	if len(e.cam.Commands()) != 0 {
		t.Errorf("a rehearsal sent commands: %v", e.cam.Commands())
	}
	for _, args := range [][]string{{"config", "frob"}, {"config", "apply"}, {"config", "apply", "--global", "--from", "garage"},
		{"config", "show", "--global", "--camera", "salon"}, {"config", "save-global", "--include-secrets"}, {"config", "show", "--only", "video"},
		{"config", "apply", "--from", "salon"}, {"config", "save-local", "--global"}, {"config", "show", "--from", "garage"}} {
		r := e.run("", args...)
		if r.rc != 2 {
			t.Errorf("%v: rc %d\n%s%s", args, r.rc, r.out, r.err)
		}
	}
	r = e.run("", "--camera", "nobody", "config", "show")
	expectRC(t, r, 2)
	contains(t, r.err, "no camera called 'nobody'")
	if _, err := os.Stat(filepath.Join(e.state, "cameras", "nobody")); err == nil {
		t.Error("a camera was created by a config command")
	}
}

func TestConfigApplyWithoutAGlobalConfig(t *testing.T) {
	e := cfgEnv(t)
	r := e.cr("", "config", "apply", "--global")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "there is no global config yet.")
	r = e.cr("", "config", "show", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "None saved yet.")
}

func TestConfigRefusesAFileItCannotRead(t *testing.T) {
	e := cfgEnv(t)
	p := e.globalPlace()
	if err := os.MkdirAll(filepath.Dir(p.Settings), 0o700); err != nil {
		t.Fatal(err)
	}
	for in, want := range map[string]string{"akam_config_version=10\n": "written by a newer akam", "led.status=on\n": "the first line must be", "akam_config_version=1\nled.status=purple\n": "line 2"} {
		if err := os.WriteFile(p.Settings, []byte(in), 0o600); err != nil {
			t.Fatal(err)
		}
		r := e.cr("", "config", "apply", "--global")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, want)
		if writesToTheCamera(e) != 0 {
			t.Error("a file that does not parse reached the camera")
		}
	}
}

func TestConfigKeepsUnknownKeysOfANewerAkam(t *testing.T) {
	e := cfgEnv(t)
	p := e.globalPlace()
	os.MkdirAll(filepath.Dir(p.Settings), 0o700)
	os.WriteFile(p.Settings, []byte("akam_config_version=1\nled.status=off\nfuture.thing=42\n"), 0o600)
	r := e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	r = e.cr("", "config", "show", "--global")
	contains(t, r.out, "1 key(s) of a newer akam are kept in the file and ignored.")
	b, _ := os.ReadFile(p.Settings)
	if !strings.Contains(string(b), "future.thing=42") {
		t.Error("an unknown key was dropped")
	}
}

// ---------------------------------------------------------------- local, copy, isolation

func TestConfigSaveLocalAndPerCameraIsolation(t *testing.T) {
	e := cfgEnv(t)
	e.rememberCamera("garage", "127.0.0.1")
	e.cam.SetCtl("led.status", "off")
	r := e.cr("\n", "config", "save-local")
	expectRC(t, r, 0)
	e.cam.SetCtl("led.status", "on")
	r = e.run("\n", "--camera", "garage", "config", "save-local")
	expectRC(t, r, 0)
	if e.loadCfg(e.cameraPlace("salon")).Get("led.status") != "off" || e.loadCfg(e.cameraPlace("garage")).Get("led.status") != "on" {
		t.Error("the two cameras share a config")
	}
	if e.cameraPlace("salon").Settings == e.cameraPlace("garage").Settings {
		t.Error("same file")
	}
	if profile.Exists(e.globalPlace(), profile.Settings) {
		t.Error("save-local wrote the global config")
	}
	if e.stateKey("config_source") != "local" {
		t.Errorf("config_source %q", e.stateKey("config_source"))
	}
	// a rename takes the camera's own config along
	r = e.run("", "--camera", "salon", "rename-camera", "salon2")
	expectRC(t, r, 0)
	if !profile.Exists(e.cameraPlace("salon2"), profile.Settings) || profile.Exists(e.cameraPlace("salon"), profile.Settings) {
		t.Error("rename-camera did not move camera.conf")
	}
}

func TestConfigCopyFromAnotherCameraLiveAndOffline(t *testing.T) {
	e := cfgEnv(t)
	e.rememberCamera("garage", "127.0.0.1")
	// both cameras answer alike here (one fake camera): the point is which source is used and what is said
	if err := os.WriteFile(filepath.Join(e.state, "cameras", "garage", "state"), []byte("camera_ip=127.0.0.1\nssh_pubkey="+sharedPub(e)+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	e.writeCfg(e.cameraPlace("garage"), "led.status", "on") // what was saved for it earlier (stale)
	// live: the camera answers, and the stale file is not used
	r := e.cr("\n", "config", "apply", "--from", "garage", "--only", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "Using the settings camera garage has right now (read over SSH).", "Config: camera garage (read now over SSH).", "Nothing to change")
	// offline: no address known for it, so what was last saved on this computer is used, and it says so
	if err := os.WriteFile(filepath.Join(e.state, "cameras", "garage", "state"), []byte("camera_kind=akam\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = e.cr("y\n", "config", "apply", "--from", "garage", "--only", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "WARNING: camera garage is not read live (no address is known for it): using the settings last saved for it on this computer",
		"Config: camera garage (as last saved on this computer).", "Front LED", "auto  ->  on", "applied")
	if got := sets(e); got != "SET led.status on" {
		t.Errorf("sets: %s", got)
	}
	if e.stateKey("config_source") != "local" { // --only: the camera does not wholly follow that config
		t.Errorf("config_source %q", e.stateKey("config_source"))
	}
	// nothing saved and no address: a clear failure, nothing sent
	e.rememberCamera("shed", "127.0.0.1")
	os.WriteFile(filepath.Join(e.state, "cameras", "shed", "state"), []byte("camera_kind=akam\n"), 0o600)
	n := writesToTheCamera(e)
	r = e.cr("", "config", "apply", "--from", "shed")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "camera shed cannot be read (no address is known for it) and nothing was saved for it on this computer.")
	if writesToTheCamera(e) != n {
		t.Error("sent something")
	}
	// the camera itself and an unknown camera are usage and failure, never a silent success
	r = e.cr("", "config", "apply", "--from", "salon")
	expectRC(t, r, 2)
	r = e.cr("", "config", "apply", "--from", "nobody")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "no camera called 'nobody' on this computer.")
}

func TestConfigSaveGlobalFromAnotherCamera(t *testing.T) {
	e := cfgEnv(t)
	e.rememberCamera("garage", "127.0.0.1")
	os.WriteFile(filepath.Join(e.state, "cameras", "garage", "state"), []byte("camera_kind=akam\n"), 0o600)
	e.writeCfg(e.cameraPlace("garage"), "led.status", "off", "video.fps_cfg", "10", "saved.at", "2026-01-02 03:04 UTC")
	e.writeSecrets(e.cameraPlace("garage"), profile.WifiSSID, "GarageNet", profile.WifiPassword, pwWifi)
	// offline (no address): its last saved settings; the Wi-Fi question offers to take its saved Wi-Fi
	r := e.cr("y\n", "config", "save-global", "--from", "garage")
	expectRC(t, r, 0)
	contains(t, r.out, "camera garage is not read live", "using the settings last saved for it on this computer (saved 2026-01-02 03:04 UTC)")
	g := e.loadCfg(e.globalPlace())
	if g.Get("led.status") != "off" || g.Get("saved.from") != "camera garage" {
		t.Errorf("%v", g.Vals)
	}
	sec, err := profile.Load(e.globalPlace(), profile.Secrets)
	if err != nil || sec.Get(profile.WifiPassword) != pwWifi {
		t.Errorf("the Wi-Fi saved for that camera was not taken: %v %v", sec, err)
	}
	notContains(t, r.out+r.err, pwWifi)
	if _, err := os.Stat(filepath.Join(e.state, "cameras", "garage", "camera.secrets")); err != nil {
		t.Error("the source's secrets file must stay")
	}
}

// ---------------------------------------------------------------- the key

func TestConfigApplyKeyChoiceNeedsItsOwnConfirmation(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	e.writeCfg(e.globalPlace(), "key.kind", "custom", "key.path", work+".pub")
	before, _ := e.cam.AuthorizedKeys()
	run := func(stdin string, extra ...string) result {
		return e.keyRun(stdin, append([]string{"config", "apply", "--global"}, extra...)...)
	}
	// the diff is shown, then: apply? yes; the key's own box: default No -> nothing changes
	r := run("y\n\n")
	expectRC(t, r, 0)
	contains(t, r.out, "SSH key", "shared  ->  custom (", "This changes the SSH key that opens this camera", "not confirmed")
	after, _ := e.cam.AuthorizedKeys()
	if before != after || e.readState("ssh_pubkey") != sharedPub(e) {
		t.Error("the key changed without its own yes")
	}
	// yes, yes (the key flow's own question), no to removing the old key
	r = run("y\ny\ny\nn\n")
	expectRC(t, r, 0)
	contains(t, r.out, "The new key works and is now used for camera default", "key", "applied")
	if e.readState("ssh_pubkey") != work+".pub" {
		t.Errorf("ssh_pubkey = %q", e.readState("ssh_pubkey"))
	}
	list, _ := e.cam.AuthorizedKeys()
	if len(keyLinesOf(t, list)) != 2 {
		t.Errorf("both keys must still work: %s", list)
	}
}

// ---------------------------------------------------------------- the first install

func TestFirstInstallOffersTheConfigAndFeedsTheCard(t *testing.T) {
	w := newKeyCardWorld(t)
	os.Unsetenv("AKAM_WIFI_SSID")
	os.Unsetenv("AKAM_WIFI_PASS")
	w.writeCfg(w.globalPlace(), "led.status", "off", "key.kind", "own")
	w.writeSecrets(w.globalPlace(), profile.WifiSSID, "Config Net", profile.WifiPassword, pwWifi)
	// the first line answers "register this camera?" (No); 1 = set up; the choice: 1 global; key offer y -> create it (y, passphrase twice); Wi-Fi offer y; erase y
	r := w.env.run("\n1\n1\ny\ny\nhunter2-pass\nhunter2-pass\ny\ny\n10\n", "--camera", "default", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "You have saved configs.", "Use a config for this camera?", "Global config (your default template)", "Keep camera defaults (no config)",
		"Noted: Global config (your default template).",
		"Your config says this camera uses a key for this camera only (~/.ssh/akam-default). Use it? [y/N]",
		"Use the Wi-Fi saved in your config (network \"Config Net\") for this card? [y/N]",
		"Using the saved Wi-Fi: the password goes on the card only", "sd_card: VERIFY: OK")
	if w.readState("config_source") != "global" || w.readState("config_pending") != "1" {
		t.Errorf("state: source=%q pending=%q", w.readState("config_source"), w.readState("config_pending"))
	}
	if w.readState("ssh_pubkey") != ownPriv(w.env, "default")+".pub" {
		t.Errorf("ssh_pubkey = %q", w.readState("ssh_pubkey"))
	}
	// the card has the Wi-Fi of the config, and the secrets reached nothing else
	f, _ := os.Open(w.img)
	defer f.Close()
	card, err := sdcard.OpenCard(f)
	if err != nil {
		t.Fatal(err)
	}
	wifi, _ := card.ReadFile("custom/configs/wifi.conf")
	if !strings.Contains(string(wifi), "SSID='Config Net'") || !strings.Contains(string(wifi), "PASS='"+pwWifi+"'") {
		t.Errorf("wifi.conf: %s", wifi)
	}
	notContains(t, r.out+r.err+w.logText(), pwWifi, "hunter2-pass")
}

func TestFirstInstallWifiOfferDefaultsToNo(t *testing.T) {
	w := newKeyCardWorld(t)
	os.Unsetenv("AKAM_WIFI_SSID")
	os.Unsetenv("AKAM_WIFI_PASS")
	w.writeCfg(w.globalPlace(), "led.status", "off")
	w.writeSecrets(w.globalPlace(), profile.WifiSSID, "Config Net", profile.WifiPassword, pwWifi)
	w.writeState(w.stateText() + "config_source=global\nconfig_pending=1\n")
	// shared key (1), Enter = do not use the saved Wi-Fi, so it is typed
	r := w.env.run("1\n\nTyped Net\n"+"typed-wifi-pass\n"+"y\n", "prepare-flash-sd", "--device", w.img)
	expectRC(t, r, 0)
	contains(t, r.err, "Use the Wi-Fi saved in your config (network \"Config Net\") for this card? [y/N]")
	f, _ := os.Open(w.img)
	defer f.Close()
	card, _ := sdcard.OpenCard(f)
	wifi, _ := card.ReadFile("custom/configs/wifi.conf")
	if !strings.Contains(string(wifi), "SSID='Typed Net'") || strings.Contains(string(wifi), pwWifi) {
		t.Errorf("wifi.conf: %s", wifi)
	}
}

func TestFirstInstallChoiceIsSkippedWhenThereIsNothingToChooseFrom(t *testing.T) {
	w := newKeyCardWorld(t)
	r := w.env.run("\n1\n10\n", "--camera", "default", "--device", w.img)
	notContains(t, r.out+r.err, "Use a config for this camera?")
	if w.readState("config_source") != "" {
		t.Errorf("config_source %q", w.readState("config_source"))
	}
}

func TestFirstInstallKeepCameraDefaultsAndTheChoiceIsAskedOnce(t *testing.T) {
	w := newKeyCardWorld(t)
	w.writeCfg(w.globalPlace(), "led.status", "off")
	r := w.env.run("\n1\n2\n1\ny\n10\n", "--camera", "default", "--device", w.img) // 2 = keep camera defaults
	contains(t, r.out+r.err, "Use a config for this camera?")
	if w.readState("config_source") != "local" || w.readState("config_pending") != "" {
		t.Errorf("state: %q %q", w.readState("config_source"), w.readState("config_pending"))
	}
	r = w.env.run("\n1\n10\n", "--camera", "default", "--device", w.img)
	notContains(t, r.out+r.err, "Use a config for this camera?")
}

func TestFirstInstallCopyFromAnotherCameraIsOffered(t *testing.T) {
	w := newKeyCardWorld(t)
	w.rememberCamera("garage", "127.0.0.1")
	w.writeCfg(w.cameraPlace("garage"), "led.status", "off")
	r := w.env.run("\n1\n1\n9\n10\n", "--camera", "default", "--device", w.img)
	contains(t, r.out+r.err, "Copy from camera garage")
	if w.readState("config_source") != "camera:garage" || w.readState("config_pending") != "1" {
		t.Errorf("state: %q %q", w.readState("config_source"), w.readState("config_pending"))
	}
}

// ---------------------------------------------------------------- the screens

// cfgUI is a plain UI whose forms are played by a script, like the session does.
type cfgUI struct {
	*ui.Plain
	mu     sync.Mutex
	script func(n int, spec ui.FormSpec) ui.FormResult
	specs  []ui.FormSpec
}

func (u *cfgUI) FormKeys(spec ui.FormSpec) (ui.FormResult, error) {
	u.mu.Lock()
	n := len(u.specs)
	u.specs = append(u.specs, spec)
	u.mu.Unlock()
	return u.script(n, spec), nil
}

func runCfgUI(t *testing.T, e *env, u *cfgUI, stdin string, args ...string) result {
	t.Helper()
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		u.Plain = cli.NewPlainUI(o, st, log, secrets, dry)
		return u
	}
	return runWith(e, h, stdin, append(e.base(), args...)...)
}

func TestEditGlobalFormSavesEachChangeAtOnceWithoutACamera(t *testing.T) {
	e := cfgEnv(t)
	u := &cfgUI{script: func(n int, spec ui.FormSpec) ui.FormResult {
		switch n {
		case 0:
			groups := map[string]bool{}
			for _, r := range spec.Rows {
				groups[r.Group] = true
			}
			for _, g := range []string{"Video", "Picture", "LED", "Audio", "Network", "Security"} {
				if !groups[g] {
					t.Errorf("group %s missing", g)
				}
			}
			if r := rowOf(spec, "video.fps_cfg"); r.Kind != ui.FormEnum || r.Value != "not set" || r.Choices[0] != "not set" {
				t.Errorf("fps row: %+v", r)
			}
			if rowOf(spec, "isp.brightness").Kind != ui.FormText || rowOf(spec, "mqtt.host").Kind != ui.FormText {
				t.Error("number and text rows are text")
			}
			for _, c := range [][2]string{{"video.fps_cfg", "20"}, {"isp.brightness", "7"}, {"onvif.enabled", "on"}, {"ntp.servers", "pool.ntp.org 192.0.2.1"}, {"led.status", "off"}} {
				row, err := spec.Apply(c[0], c[1])
				if err != nil {
					t.Errorf("apply %v: %v", c, err)
				}
				if row.ID != c[0] {
					t.Errorf("row %+v", row)
				}
			}
			if _, err := spec.Apply("isp.brightness", "999"); err == nil {
				t.Error("an out of range value was accepted")
			}
			if _, err := spec.Apply("mqtt.host", "bad host!"); err == nil {
				t.Error("a bad host was accepted")
			}
			if _, err := spec.Apply("led.status", "not set"); err != nil {
				t.Error(err)
			}
			if _, err := spec.Apply("isp.brightness", ""); err != nil {
				t.Error(err)
			}
		}
		return ui.FormResult{}
	}}
	r := runCfgUI(t, e, u, "", "--camera", "salon", "config", "edit-global")
	expectRC(t, r, 0)
	g := e.loadCfg(e.globalPlace())
	if g.Get("video.fps_cfg") != "20" || g.Get("onvif.enabled") != "1" || g.Get("ntp.servers") != "pool.ntp.org,192.0.2.1" || g.Has("led.status") || g.Has("isp.brightness") || g.Has("mqtt.host") {
		t.Errorf("template: %v", g.Vals)
	}
	if len(e.cam.Commands()) != 0 {
		t.Errorf("editing the template contacted the camera: %v", e.cam.Commands())
	}
	if !profile.Exists(e.globalPlace(), profile.Settings) || profile.Exists(e.globalPlace(), profile.Secrets) {
		t.Error("files")
	}
}

func TestEditGlobalSecretsKeyAsksItsOwnQuestion(t *testing.T) {
	e := cfgEnv(t)
	u := &cfgUI{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "s"}
		}
		return ui.FormResult{}
	}}
	// passwords and Wi-Fi: item 3 = Wi-Fi; yes (the warning); name; password
	r := runCfgUI(t, e, u, "3\ny\nMy Net\n"+pwWifi+"\n", "--camera", "salon", "config", "edit-global")
	expectRC(t, r, 0)
	contains(t, r.err, "Keep the Wi-Fi name and password in the global config? They are stored on this computer in a private file; anyone with access to your account can read it. [y/N]")
	sec, err := profile.Load(e.globalPlace(), profile.Secrets)
	if err != nil || sec.Get(profile.WifiSSID) != "My Net" || sec.Get(profile.WifiPassword) != pwWifi {
		t.Fatalf("%v %v", sec, err)
	}
	// Enter = No: nothing saved
	e2 := cfgEnv(t)
	r = runCfgUI(t, e2, &cfgUI{script: u.script}, "3\n\n", "--camera", "salon", "config", "edit-global")
	expectRC(t, r, 0)
	if profile.Exists(e2.globalPlace(), profile.Secrets) {
		t.Error("saved without a yes")
	}
	e.noSecretsAnywhere(r, pwWifi)
}

func TestEditGlobalPlainListNeedsNoCamera(t *testing.T) {
	e := cfgEnv(t)
	n := len(profile.OptionIDs()) + 17 // the option rows, then onvif 2, ntp 2, mqtt 9, video password 2, SSH key 2
	// Front LED (1), choices: not set, on, off, auto -> off (3); then Done
	r := e.cr("1\n3\n"+itoa(n+2)+"\n", "config", "edit-global")
	expectRC(t, r, 0)
	contains(t, r.out, "Global config (a '-' means: not part of the template):", "Front LED", "Saved: Front LED off")
	if e.loadCfg(e.globalPlace()).Get("led.status") != "off" {
		t.Error("not saved")
	}
	if len(e.cam.Commands()) != 0 {
		t.Error("the camera was contacted")
	}
}

// rowSession is a board session that opens one row of the home and then leaves.
type rowSession struct {
	*boardSession
	once bool
	row  int
}

func (s *rowSession) BoardKey() (string, int, error) {
	if !s.once {
		s.once = true
		return "enter", s.row, nil
	}
	return "esc", 0, nil
}

type rowOuter struct {
	*ui.Plain
	s *rowSession
}

func (o rowOuter) Session(fn func(ui.UI) error) error { return fn(o.s) }

// The home of the session has a Configs row (shortcut 9) that opens the Configs screen.
func TestHomeSessionConfigsRow(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off")
	rs := &rowSession{row: 7}
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		p := cli.NewPlainUI(o, st, log, secrets, dry)
		rs.boardSession = &boardSession{Plain: p}
		return rowOuter{p, rs}
	}
	// the first line answers "register this camera?"; Configs > 1 (show the global config) > the screen ends on Back
	r := runWith(e, h, "\n1\n", append(e.base(), "--camera", "salon")...)
	expectRC(t, r, 0)
	contains(t, r.out, "Configs - global, local, copy", "Global config (saved)", "Global config (the default template for new cameras)", "Front LED")
	last := rs.boards[0]
	if last.Rows[7].Label != "Configs" || last.Rows[7].Hot != "9" {
		t.Errorf("row: %+v", last.Rows[7])
	}
}

// ---------------------------------------------------------------- after the verify step

// The config chosen at the start of the install is applied after the verify step: the differences are shown,
// the yes/no defaults to No and, when it is not given, the choice stays pending for later.
func TestVerifyAppliesThePendingConfigAfterAYes(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	w.writeCfg(w.globalPlace(), "led.status", "off")
	w.writeState(w.stateText() + "config_source=global\nconfig_pending=1\n")
	// Enter (power cycled); then the config: Enter = No; the stream password and ONVIF offers: no
	r := w.run("\n\nn\nn\n", "verify")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "The config you chose for this camera", "1 change(s) from the camera's current settings:", "auto  ->  off", "Nothing was sent.",
		"Not applied yet. When you want it: ./akam config apply --global")
	if w.readState("config_pending") != "1" || len(w.cam.CtlSets()) != 0 {
		t.Error("applied without a yes, or the choice was forgotten")
	}
	r = w.run("\ny\nn\nn\n", "verify")
	expectRC(t, r, 0)
	if got := sets(w.env); got != "SET led.status off" {
		t.Errorf("sets: %s\n%s", got, r.out)
	}
	if w.readState("config_pending") != "" || w.readState("config_source") != "global" {
		t.Errorf("state: pending=%q source=%q", w.readState("config_pending"), w.readState("config_source"))
	}
}

// In the plain menu 9 is Configs and Quit is 10: the digits of the old menu still mean what they meant.
func TestPlainHomeMenuNumbers(t *testing.T) {
	e := cfgEnv(t)
	r := e.cr("9\n9\n10\n") // Configs > Back > Quit
	expectRC(t, r, 0)
	contains(t, r.out, "9) Configs", "10) Quit", "Configs - global, local, copy")
}
