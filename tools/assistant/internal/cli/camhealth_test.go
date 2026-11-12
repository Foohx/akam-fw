package cli_test

import (
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The camera options screen shows the Wi-Fi signal and the clock. home 3 = options; "8" = Back, "9" = quit
// when no question is asked first.
func optionsHealth(t *testing.T, tune func(c *testfake.Camera)) (*env, result) {
	t.Helper()
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	if tune != nil {
		tune(e.cam)
	}
	return e, e.run("3\n8\n9\n", "--camera", "salon")
}

func TestOptionsShowsWifiLevels(t *testing.T) {
	for _, c := range []struct {
		rssi, level string
		warn        bool
	}{{"-55", "good", false}, {"-66", "fair", false}, {"-73", "weak", true}, {"-78", "poor", true}} {
		_, r := optionsHealth(t, func(cam *testfake.Camera) { cam.SetCtl("net.wifi_rssi", c.rssi); cam.SetCtl("net.wifi_rate", "65") })
		expectRC(t, r, 0)
		contains(t, r.out, "Wi-Fi signal: "+c.rssi+" dBm ("+c.level+"), link 65 Mb/s.")
		if c.warn {
			contains(t, r.out, "move the camera or the access point closer", "repeater", "UDP loses key frames", "TCP lags",
				"now: bitrate 1536 kbit/s, key frame every 2 s", "./akam --camera salon video bitrate 1024 gop 1", "nothing is changed automatically")
		} else {
			notContains(t, r.out, "move the camera", "video bitrate 1024")
		}
	}
}

func TestOptionsWifiWarningIsReadOnlyAndSkipsLowSettings(t *testing.T) {
	e, r := optionsHealth(t, func(cam *testfake.Camera) {
		cam.SetCtl("net.wifi_rssi", "-80")
		cam.SetCtl("video.bitrate_cfg", "1024")
		cam.SetCtl("video.gop_cfg", "1")
	})
	expectRC(t, r, 0)
	contains(t, r.out, "(poor)", "The video settings are already low (now: bitrate 1024 kbit/s, key frame every 1 s).")
	notContains(t, r.out, "video bitrate 1024")
	if len(e.cam.CtlSets()) != 0 {
		t.Errorf("sets: %v", e.cam.CtlSets())
	}
}

func TestOptionsNightFpsIsNotBlamedOnWifi(t *testing.T) {
	_, r := optionsHealth(t, func(cam *testfake.Camera) {
		cam.SetCtl("net.wifi_rssi", "-78")
		cam.SetCtl("video.fps_actual", "100")
		cam.SetCtl("night.state", "night")
	})
	contains(t, r.out, "Measured 10.0 fps: at night this is normal (long exposure), it is not the Wi-Fi.")
	notContains(t, r.out, "frames are being dropped")
	_, r = optionsHealth(t, func(cam *testfake.Camera) {
		cam.SetCtl("net.wifi_rssi", "-78")
		cam.SetCtl("video.fps_actual", "100")
	})
	contains(t, r.out, "Measured 10.0 fps for 15 configured: frames are being dropped")
	notContains(t, r.out, "at night")
}

func TestOptionsOlderImageWithoutHealthMetrics(t *testing.T) {
	_, r := optionsHealth(t, func(cam *testfake.Camera) {
		for _, k := range []string{"net.wifi_rssi", "net.wifi_rate", "night.state", "sys.time_set"} {
			cam.SetCtl(k, "-")
		}
	})
	expectRC(t, r, 0)
	notContains(t, r.out, "Wi-Fi signal")
}

func TestOptionsClockSetAndOffAreOffered(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	t.Setenv("AKAM_FORCE_TTY", "1")
	// a clock that is set and right: shown, nothing asked
	r := e.run("3\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "Clock: set, ")
	notContains(t, r.err, "Set the camera clock")
	// never set: asked, default No (Enter)
	e.cam.SetClock(time.Date(2000, 1, 1, 0, 0, 30, 0, time.UTC), false)
	r = e.run("3\n\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "Clock: not set", "reads 2000-01-01", "Clock left as it is.")
	contains(t, r.err, "Set the camera clock from this computer now? It is lost at the next reboot unless time sync (NTP) is on. [y/N]")
	if len(e.cam.ClockSets()) != 0 {
		t.Errorf("set without a yes: %v", e.cam.ClockSets())
	}
	// yes: one date -s with the PC time, in UTC
	before := time.Now().UTC().Add(-2 * time.Second)
	r = e.run("3\ny\n8\n9\n", "--camera", "salon")
	sets := e.cam.ClockSets()
	if len(sets) != 1 {
		t.Fatalf("clock sets: %v\n%s", sets, r.out)
	}
	got, err := time.ParseInLocation("2006-01-02 15:04:05", sets[0], time.UTC)
	if err != nil || got.Before(before) || got.After(time.Now().UTC().Add(2*time.Second)) {
		t.Errorf("set to %q", sets[0])
	}
	contains(t, r.out, "Camera clock set to "+sets[0]+" UTC.")
	// now it is right: no question again
	r = e.run("3\n8\n9\n", "--camera", "salon")
	notContains(t, r.err, "Set the camera clock")
	// an hour off: asked again; two minutes or less: not
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	r = e.run("3\n\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "Clock: off by 1 hour(s)")
	contains(t, r.err, "Set the camera clock from this computer now?")
	e.cam.SetClock(time.Now().Add(-90*time.Second), true)
	r = e.run("3\n8\n9\n", "--camera", "salon")
	notContains(t, r.err, "Set the camera clock")
	// no terminal: nothing is asked
	t.Setenv("AKAM_FORCE_TTY", "")
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	r = e.run("", "--camera", "salon", "ntp", "status")
	contains(t, r.out, "Clock: off by 1 hour(s)")
	notContains(t, r.err, "Set the camera clock")
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "date") && c != "date +%s" && !strings.HasPrefix(c, "date -u -s '") {
			t.Errorf("unexpected date command %q", c)
		}
	}
}

func TestVideoShowAndNtpStatusShowHealth(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	e.cam.SetCtl("net.wifi_rssi", "-77")
	r := e.run("", "--camera", "salon", "video", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "Wi-Fi signal: -77 dBm (poor)", "./akam --camera salon video bitrate 1024 gop 1")
	r = e.run("", "--camera", "salon", "ntp", "status")
	expectRC(t, r, 0)
	contains(t, r.out, "Clock: set, ")
}

func TestVerifyOffersTimeSyncDefaultNo(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	// Enter (power cycled), no password, no ONVIF, Enter = No to time sync
	r := w.run("\nn\nn\n\n", "verify")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Optional: time sync.", "pool.ntp.org", "Turn on time sync? [y/N]", "Time sync left off.")
	for _, c := range w.cam.Commands() {
		if strings.Contains(c, "akam_ntp.sh set") {
			t.Errorf("time sync turned on without a yes: %q", c)
		}
	}
}

func TestVerifyTimeSyncYesWithLanServer(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	r := w.run("\nn\nn\ny\n192.0.2.7\n", "verify")
	expectRC(t, r, 0)
	found := false
	for _, c := range w.cam.Commands() {
		found = found || c == "/usr/sbin/akam_ntp.sh set 192.0.2.7"
	}
	if !found {
		t.Errorf("no ntp set: %q\n%s", w.cam.Commands(), r.out+r.err)
	}
	contains(t, r.out, "Time sync on, servers: 192.0.2.7")
	// Enter on the server question: the default pool
	w = newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	w.run("\nn\nn\ny\n\n", "verify")
	found = false
	for _, c := range w.cam.Commands() {
		found = found || c == "/usr/sbin/akam_ntp.sh set pool.ntp.org"
	}
	if !found {
		t.Errorf("no default ntp set: %q", w.cam.Commands())
	}
}

func TestVerifyDryRunMentionsTimeSync(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	r := w.run("", "--dry-run", "verify")
	contains(t, r.out+r.err, "WOULD ask: Turn on time sync? [y/N] (default: No)", "WOULD run over SSH on the camera: /usr/sbin/akam_ntp.sh set pool.ntp.org")
}

// With time sync on, the clock offer must not say the time is lost at the next reboot.
func TestClockOfferWithTimeSyncOn(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	t.Setenv("AKAM_FORCE_TTY", "1")
	expectRC(t, e.run("", "--camera", "salon", "ntp", "set", "pool.ntp.org"), 0)
	e.cam.SetClock(time.Date(2000, 1, 1, 0, 0, 30, 0, time.UTC), false) // never set: the offer stays with time sync on
	r := e.run("3\n\n8\n9\n", "--camera", "salon")
	contains(t, r.err, "Time sync is on and corrects the clock shortly")
	notContains(t, r.err, "lost at the next reboot")
	contains(t, r.out, "time sync should correct it shortly.")
}

// A camera that keeps its time with NTP is the better reference: a difference with this computer
// must never lead to overwriting the camera clock (a PC with a wrong clock once set a camera 1 h wrong).
func TestOptionsClockNtpOnNeverOffersToOverwrite(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	expectRC(t, e.run("", "--camera", "salon", "ntp", "set", "pool.ntp.org"), 0)
	t.Setenv("AKAM_FORCE_TTY", "1")
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	r := e.run("3\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "Clock: off by 1 hour(s)", "keeps its time with NTP", "may be wrong", "difference 1 hour(s)")
	notContains(t, r.err, "Set the camera clock")
	// the same through "ntp status"
	r = e.run("", "--camera", "salon", "ntp", "status")
	contains(t, r.out, "may be wrong")
	// the clock never set: the offer stays, even with time sync on
	e.cam.SetClock(time.Date(2000, 1, 1, 0, 0, 30, 0, time.UTC), false)
	r = e.run("3\n\n8\n9\n", "--camera", "salon")
	contains(t, r.err, "Set the camera clock from this computer now?")
	if len(e.cam.ClockSets()) != 0 {
		t.Errorf("clock set: %v", e.cam.ClockSets())
	}
}

// A hung control port: the answers received before the timeout are used, not thrown away.
func TestCtlTimeoutKeepsPartialAnswers(t *testing.T) {
	defer cli.SetCtlTimeout(500 * time.Millisecond)()
	e := newEnv(t, func(o *testfake.Options) { o.CtlAnswerLimit = 3 })
	e.rememberCamera("salon", "127.0.0.1")
	r := e.run("", "--camera", "salon", "led", "show")
	expectRC(t, r, 0)
	contains(t, r.out, "Front LED")
}

// Time sync is on and has worked: a difference means this computer is wrong. On but not (yet) synced:
// the camera clock is not known to be right, so the offer stays.
func TestClockNtpOnWithoutASuccessfulSyncStillOffers(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	expectRC(t, e.run("", "--camera", "salon", "ntp", "set", "pool.ntp.org"), 0)
	t.Setenv("AKAM_FORCE_TTY", "1")
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	for _, log := range []string{"akam_ntp: sync failed (pass 2), next try in 60s", ""} {
		e.cam.SetNtpLog(log)
		r := e.run("3\n\n8\n9\n", "--camera", "salon")
		contains(t, r.out, "Time sync is on but has not succeeded yet")
		contains(t, r.err, "Set the camera clock from this computer now?", "Time sync is on but has not succeeded yet")
		notContains(t, r.out, "may be wrong")
	}
	e.cam.SetNtpLog("akam_ntp: sync ok after 1 failed pass(es)")
	r := e.run("3\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "may be wrong")
	notContains(t, r.err, "Set the camera clock")
	if len(e.cam.ClockSets()) != 0 {
		t.Errorf("clock set: %v", e.cam.ClockSets())
	}
}

// video show, ntp status and the options screen must tell the same story about the clock.
func TestVideoShowClockAgreesWithNtpStatusAndOptions(t *testing.T) {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	// time sync off: the offset is shown by all three
	r := e.run("", "--camera", "salon", "video", "show")
	contains(t, r.out, "Clock: off by 1 hour(s)")
	notContains(t, r.out, "Clock: set.")
	r = e.run("", "--camera", "salon", "ntp", "status")
	contains(t, r.out, "Clock: off by 1 hour(s)")
	// time sync on and working: the same warning everywhere
	expectRC(t, e.run("", "--camera", "salon", "ntp", "set", "pool.ntp.org"), 0)
	e.cam.SetClock(time.Now().Add(-time.Hour), true)
	for _, args := range [][]string{{"video", "show"}, {"ntp", "status"}} {
		r = e.run("", append([]string{"--camera", "salon"}, args...)...)
		contains(t, r.out, "Clock: off by 1 hour(s)", "keeps its time with NTP", "may be wrong")
	}
	t.Setenv("AKAM_FORCE_TTY", "1")
	r = e.run("3\n8\n9\n", "--camera", "salon")
	contains(t, r.out, "Clock: off by 1 hour(s)", "keeps its time with NTP", "may be wrong")
}
