//go:build akamtest

package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Review round: secrets bound to what they were typed for, a control port that says nothing, a partial read,
// the host key of a source camera, the key outcome, panels that never nest, stop in the middle.

// garage remembers a second camera that is the same fake device, with its own state lines.
func (e *env) garage(stateLines string) {
	e.t.Helper()
	e.rememberCamera("garage", "127.0.0.1")
	if err := os.WriteFile(filepath.Join(e.state, "cameras", "garage", "state"), []byte(stateLines), 0o600); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) auth(user string) {
	e.t.Helper()
	if r := e.cr(user+"\n"+pwStream+"\n"+pwStream+"\n", "rtsp-auth", "set"); r.rc != 0 {
		e.t.Fatal(r.out, r.err)
	}
}

// ---------------------------------------------------------------- M1 secrets follow their settings

func TestSavedPasswordIsUsedOnlyForTheUserItWasTypedFor(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "auth.enabled", "1", "auth.user", "viewer")
	e.writeSecrets(e.globalPlace(), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "someoneelse")
	r := e.cr("y\n"+"new-pass-9999\nnew-pass-9999\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "password: asked", "The video password is not saved in this config: type it now.")
	notContains(t, r.out, "password: set (from the config)")
	cred, _ := os.ReadFile(filepath.Join(e.cam.Root, "etc", "config", "akam", "credentials"))
	if strings.Contains(string(cred), pwStream) || !strings.Contains(string(cred), "pass=new-pass-9999") {
		t.Errorf("the password of another user was sent: %q", cred)
	}
	// bound to the right user: used without a question
	e2 := cfgEnv(t)
	e2.writeCfg(e2.globalPlace(), "auth.enabled", "1", "auth.user", "viewer")
	e2.writeSecrets(e2.globalPlace(), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	r = e2.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "password: set (from the config)")
	// the MQTT one needs the same broker AND user
	e3 := cfgEnv(t)
	e3.writeCfg(e3.globalPlace(), "mqtt.enabled", "1", "mqtt.host", "broker.lan", "mqtt.user", "bob")
	e3.writeSecrets(e3.globalPlace(), profile.MQTTPassword, pwMqtt, profile.MQTTPasswordHost, "other.lan", profile.MQTTPasswordUser, "bob")
	r = e3.cr("y\n"+pwMqtt+"x\n"+pwMqtt+"x\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "password: asked")
	e3.noSecretsAnywhere(r, pwMqtt+"x")
}

func TestResavingDropsAPasswordThatNoLongerMatchesAndNeverReusesItSilently(t *testing.T) {
	e := cfgEnv(t)
	e.auth("viewer")
	// a saved password for ANOTHER user sits in the global config
	e.writeCfg(e.globalPlace(), "led.status", "on")
	e.writeSecrets(e.globalPlace(), profile.AuthPassword, "old-user-pass-1", profile.AuthPasswordUser, "olduser")
	r := e.cr("y\nn\nn\n", "config", "save-global") // replace yes; passwords No; Wi-Fi No
	expectRC(t, r, 0)
	contains(t, r.out, "Removed the saved video password: it no longer matches these settings.")
	if profile.Exists(e.globalPlace(), profile.Secrets) {
		t.Error("a stale password stayed")
	}
	// a matching one: a yes offers to keep it or type a new one (default No = type)
	// (the source camera's own saved password is the one offered)
	e.writeSecrets(e.cameraPlace("salon"), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	r = e.cr("y\ny\n\n"+"fresh-pass-1234\nfresh-pass-1234\nn\n", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.err, "A video password saved for user viewer is available. Keep it? (No: type a new one) [y/N]")
	sec, err := profile.Load(e.globalPlace(), profile.Secrets)
	if err != nil || sec.Get(profile.AuthPassword) != "fresh-pass-1234" || sec.Get(profile.AuthPasswordUser) != "viewer" {
		t.Errorf("%v %v", sec, err)
	}
}

func TestSettingAPasswordOnTheCameraMakesTheSavedCopyStale(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.cameraPlace("salon"), "led.status", "on")
	e.writeSecrets(e.cameraPlace("salon"), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	// a new password with the same user: the saved copy is out of date; No (default) removes it
	r := e.cr("viewer\nnew-pass-5555\nnew-pass-5555\n\n", "rtsp-auth", "set")
	expectRC(t, r, 0)
	contains(t, r.err, "A video password is saved for this camera on this computer. Update it to the new one?")
	contains(t, r.out, "out of date: removed")
	if profile.Exists(e.cameraPlace("salon"), profile.Secrets) {
		t.Error("the stale copy stayed")
	}
	// Yes updates it, bound to the user
	e.writeSecrets(e.cameraPlace("salon"), profile.AuthPassword, pwStream, profile.AuthPasswordUser, "viewer")
	r = e.cr("viewer\nnewer-pass-6666\nnewer-pass-6666\ny\n", "rtsp-auth", "set")
	expectRC(t, r, 0)
	sec, _ := profile.Load(e.cameraPlace("salon"), profile.Secrets)
	if sec.Get(profile.AuthPassword) != "newer-pass-6666" {
		t.Errorf("%v", sec)
	}
	e.noSecretsAnywhere(r, "newer-pass-6666")
	// removing the video password forgets the copy too
	r = e.cr("y\n", "rtsp-auth", "disable")
	if profile.Exists(e.cameraPlace("salon"), profile.Secrets) {
		t.Error("the copy of a removed password stayed")
	}
}

// ---------------------------------------------------------------- M2 an outage of the control port

func TestRecordBeforeTheRestartAndNeverOverwriteWithAnEmptyRead(t *testing.T) {
	e := cfgEnv(t)
	e.cam.CtlDownAfterRestart(true)
	e.writeCfg(e.globalPlace(), "video.fps_cfg", "25", "led.status", "off")
	r := e.cr("y\ny\n", "config", "apply", "--global") // apply yes, restart yes: the port is then down
	expectRC(t, r, 0)
	if e.cam.VideoRestarts() != 1 {
		t.Fatal("no restart")
	}
	loc := e.loadCfg(e.cameraPlace("salon"))
	if loc.Get("video.fps_cfg") != "25" || loc.Get("led.status") != "off" {
		t.Errorf("recorded after the restart, with the port down: %v", loc.Vals)
	}
	// the port is down now: neither save-local nor apply may replace the record or compare with nothing
	before, _ := os.ReadFile(e.cameraPlace("salon").Settings)
	r = e.cr("", "config", "save-local")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the camera's control port did not answer, so its settings cannot be read.")
	r = e.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "control port did not answer, so its settings cannot be compared.")
	after, _ := os.ReadFile(e.cameraPlace("salon").Settings)
	if string(before) != string(after) {
		t.Error("the record was overwritten by a read that got nothing")
	}
	// back up: a record works again
	e.cam.CtlDownAfterRestart(false)
	e.cam.SetCtlDown(false)
	r = e.cr("\n", "config", "save-local")
	expectRC(t, r, 0)
}

// ---------------------------------------------------------------- M3 a partial read is not a complete one

func TestAHelperThatDoesNotAnswerIsLeftOutOfTheDiff(t *testing.T) {
	// an image without the time sync helper: its state cannot be read
	e2 := newEnvNoNtp(t)
	e2.writeCfg(e2.globalPlace(), "ntp.enabled", "1", "ntp.servers", "pool.ntp.org", "led.status", "off")
	r := e2.cr("y\n", "config", "apply", "--global")
	expectRC(t, r, 0)
	contains(t, r.out, "Could not be read from this camera: Time sync", "Not applied: Time sync: could not be read from the camera, so it is left out.", "1 change(s)")
	if strings.Contains(strings.Join(e2.cam.Commands(), "\n"), "akam_ntp.sh set") {
		t.Error("time sync was set although its state could not be read")
	}
	// the record keeps no invented value for it
	if e2.loadCfg(e2.cameraPlace("salon")).Has("ntp.enabled") {
		t.Error("an unread helper became a recorded value")
	}
}

func newEnvNoNtp(t *testing.T) *env {
	t.Helper()
	e := newEnv(t, func(o *testfake.Options) { o.NoNtpHelper = true })
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	t.Setenv("AKAM_WIFI_PASS", "")
	t.Setenv("AKAM_WIFI_SSID", "")
	os.WriteFile(filepath.Join(e.state, "current_camera"), []byte("salon\n"), 0o600)
	return e
}

func TestSaveGlobalSaysWhatIsMissingBeforeTheReplaceQuestion(t *testing.T) {
	e := newEnvNoNtp(t)
	e.writeCfg(e.globalPlace(), "led.status", "on")
	r := e.cr("\n", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.out, "Could not be read from this camera: Time sync")
	contains(t, r.err, "WITHOUT Time sync (not read)? [y/N]")
}

func TestAControlPortThatSaysNothingFallsBackToTheSavedFileOrRefuses(t *testing.T) {
	e := cfgEnv(t)
	e.cam.SetCtlDown(true)
	r := e.cr("", "config", "save-global")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "camera salon cannot be read (its control port did not answer) and nothing was saved for it on this computer.")
	e.writeCfg(e.cameraPlace("salon"), "led.status", "off", "saved.at", "2026-01-02 03:04 UTC")
	r = e.cr("\n", "config", "save-global")
	expectRC(t, r, 0)
	contains(t, r.out, "is not read live (its control port did not answer): using the settings last saved for it on this computer (saved 2026-01-02 03:04 UTC)")
	if e.loadCfg(e.globalPlace()).Get("led.status") != "off" {
		t.Error("the saved file was not used")
	}
}

// ---------------------------------------------------------------- L1, L2 the source camera

func TestSourceCameraHostKeyChangedUnpinnedOrAnotherName(t *testing.T) {
	e := cfgEnv(t)
	e.garage("camera_ip=127.0.0.1\nssh_pubkey=" + sharedPub(e) + "\ncamera_host=akam-01\n")
	e.writeCfg(e.cameraPlace("garage"), "led.status", "off")
	// the right name, the pinned key: read live
	r := e.cr("\n", "config", "save-global", "--from", "garage")
	expectRC(t, r, 0)
	contains(t, r.out, "Using the settings camera garage has right now")
	if writesToTheCamera(e) != 0 {
		t.Error("reading the source wrote to a camera")
	}
	// another name than the one known: not used live
	e.garage("camera_ip=127.0.0.1\nssh_pubkey=" + sharedPub(e) + "\ncamera_host=someone-else\n")
	r = e.cr("y\nn\n", "config", "save-global", "--from", "garage")
	contains(t, r.out, "it calls itself by another name than the one this computer knows")
	// the host key changed: said loudly, not read
	e.garage("camera_ip=127.0.0.1\nssh_pubkey=" + sharedPub(e) + "\ncamera_host=akam-01\n")
	e.cam.RotateHostKey()
	r = e.cr("y\nn\n", "config", "save-global", "--from", "garage")
	contains(t, r.out+r.err, "REMOTE HOST IDENTIFICATION HAS CHANGED for camera garage", "its SSH host key changed")
	// not remembered yet
	os.Remove(filepath.Join(e.state, "known_hosts"))
	r = e.cr("y\nn\n", "config", "save-global", "--from", "garage")
	contains(t, r.out, "its SSH host key is not remembered yet")
	if writesToTheCamera(e) != 0 {
		t.Error("a source was written")
	}
}

func TestApplyFromRefusesNamesThatAreNotCameras(t *testing.T) {
	e := cfgEnv(t)
	for _, n := range []string{"..", "../x", "a/b", "UPPER"} {
		r := e.cr("", "config", "apply", "--from", n)
		if r.rc == 0 || strings.Contains(r.out, "Using the settings") {
			t.Errorf("--from %q: rc %d\n%s%s", n, r.rc, r.out, r.err)
		}
	}
	r := e.cr("", "config", "apply", "--from", "..")
	contains(t, r.out+r.err, "no camera called '..' on this computer.")
}

// ---------------------------------------------------------------- L4 the key outcome and the pending choice

func TestAnUnconfirmedKeyChangeKeepsTheChoicePendingAndTheSourceLocal(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	e.writeCfg(e.globalPlace(), "key.kind", "custom", "key.path", work+".pub", "led.status", "off")
	e.writeState(e.stateText() + "config_source=global\nconfig_pending=1\n")
	r := e.keyRun("y\n\n", "config", "apply", "--global") // apply yes, the key's own box No
	expectRC(t, r, 0)
	contains(t, r.out, "1 applied, 0 failed, 1 not applied.")
	if e.readState("config_pending") != "1" {
		t.Error("the choice was cleared although an item was declined")
	}
	if e.readState("config_source") != "global" { // a first-install choice stays what it was while it is pending
		t.Errorf("config_source = %q", e.readState("config_source"))
	}
}

// ---------------------------------------------------------------- L5 panels never nest, stop in the middle

type planUI struct {
	*ui.Plain
	mu            sync.Mutex
	depth, maxDep int
	plans         int
}

func (u *planUI) OpPlan(items []string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.depth++
	u.plans++
	if u.depth > u.maxDep {
		u.maxDep = u.depth
	}
}
func (u *planUI) OpStart(int)          {}
func (u *planUI) OpDone(int, string)   {}
func (u *planUI) OpStatus(string)      {}
func (u *planUI) OpBytes(int64, int64) {}
func (u *planUI) OpWriting(bool)       {}
func (u *planUI) OpEnd() {
	u.mu.Lock()
	u.depth--
	u.mu.Unlock()
}

func TestTheKeyChangeRunsOutsideTheApplyPanel(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.useSharedKey()
	work := filepath.Join(e.home, ".ssh", "id_work")
	if err := camssh.GenerateKey(work, "work", nil); err != nil {
		t.Fatal(err)
	}
	e.writeCfg(e.globalPlace(), "key.kind", "custom", "key.path", work+".pub", "led.status", "off")
	u := &planUI{}
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		u.Plain = cli.NewPlainUI(o, st, log, secrets, dry)
		return u
	}
	r := runWith(e, h, "y\ny\ny\nn\n", append(keyArgs(e), "config", "apply", "--global")...)
	expectRC(t, r, 0)
	if u.maxDep != 1 || u.depth != 0 {
		t.Errorf("panels nested or left open: max depth %d, depth %d", u.maxDep, u.depth)
	}
	if u.plans < 2 {
		t.Errorf("expected the apply panel and the key panel, got %d plan(s)", u.plans)
	}
	if e.readState("ssh_pubkey") != work+".pub" {
		t.Error("the key was not changed")
	}
}

type cfgStopUI struct {
	*ui.Plain
	mu    sync.Mutex
	calls int
}

func (u *cfgStopUI) StopChan() <-chan struct{} {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.calls++
	ch := make(chan struct{})
	if u.calls >= 2 { // the question after the confirmation sees no stop; the next item does
		close(ch)
	}
	return ch
}

func TestStopInTheMiddleOfAnApplyLeavesTheRestUnsent(t *testing.T) {
	e := cfgEnv(t)
	e.writeCfg(e.globalPlace(), "led.status", "off", "ntp.enabled", "1", "ntp.servers", "pool.ntp.org")
	e.writeState("") // keep the default camera's state out of it
	su := &cfgStopUI{}
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		su.Plain = cli.NewPlainUI(o, st, log, secrets, dry)
		return su
	}
	r := runWith(e, h, "y\n", append(e.base(), "config", "apply", "--global")...)
	expectRC(t, r, 0)
	contains(t, r.out, "stopped before it was sent", "1 applied, 0 failed, 1 not applied.")
	if got := sets(e); got != "SET led.status off" {
		t.Errorf("sets: %s", got)
	}
	if strings.Contains(strings.Join(e.cam.Commands(), "\n"), "akam_ntp.sh set") {
		t.Error("an item was sent after the stop")
	}
	if e.stateKey("config_source") != "local" {
		t.Errorf("config_source = %q", e.stateKey("config_source"))
	}
}

// ---------------------------------------------------------------- L11

func TestConfigOptionsAreRefusedOnOtherCommands(t *testing.T) {
	e := cfgEnv(t)
	for _, args := range [][]string{{"status", "--global"}, {"status", "--only", "video"}, {"status", "--from", "x"}, {"status", "--include-secrets"}} {
		r := e.cr("", args...)
		expectRC(t, r, 2)
		contains(t, r.err, "are options of the config command")
	}
}

func TestEditGlobalSecretsNeedsTheUserBeforeAPassword(t *testing.T) {
	e := cfgEnv(t)
	u := &cfgUI{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "s"}
		}
		return ui.FormResult{}
	}}
	// item 1 = the video password; the template has no user yet
	r := runCfgUI(t, e, u, "1\ny\n", "--camera", "salon", "config", "edit-global")
	expectRC(t, r, 0)
	contains(t, r.out, "The template has no video user name yet")
	notContains(t, r.out, "Saved in a private file")
	// item 3 = Wi-Fi, answered No: nothing is said to be saved
	e2 := cfgEnv(t)
	r = runCfgUI(t, e2, &cfgUI{script: u.script}, "3\ny\n\n", "--camera", "salon", "config", "edit-global") // yes, then no SSID
	notContains(t, r.out, "Saved in a private file")
}

func TestRAMOnlyOptionsAreNotPartOfAConfig(t *testing.T) {
	e := cfgEnv(t)
	r := e.cr("\n", "config", "save-global")
	expectRC(t, r, 0)
	g := e.loadCfg(e.globalPlace())
	if g.Has("night.mode") || g.Has("img.wb") {
		t.Error("an option that goes back to its default at a reboot was saved in a config")
	}
}
