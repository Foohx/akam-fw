package cli

import (
	"strings"
	"testing"
	"time"
)

func TestWifiLevelThresholds(t *testing.T) {
	for rssi, want := range map[int]string{0: "", -30: "good", -60: "good", -61: "fair", -70: "fair", -71: "weak", -75: "weak", -76: "poor", -90: "poor"} {
		if got := wifiLevel(rssi); got != want {
			t.Errorf("%d dBm: %q, want %q", rssi, got, want)
		}
	}
}

func TestWifiReportPerLevel(t *testing.T) {
	base := camHealth{rate: 65, kbpsCfg: 1536, gop: 2, fpsCfg: 15, fps10: 150}
	for _, c := range []struct {
		rssi     int
		wantAll  []string
		wantNone []string
	}{
		{-50, []string{"-50 dBm (good), link 65 Mb/s."}, []string{"Best fix"}},
		{-65, []string{"(fair)"}, []string{"Best fix"}},
		{-72, []string{"(weak)", "Best fix: move the camera or the access point closer", "repeater", "UDP loses key frames", "TCP lags",
			"now: bitrate 1536 kbit/s, key frame every 2 s", "akam --camera salon video bitrate 1024 gop 1", "nothing is changed automatically"}, nil},
		{-78, []string{"(poor)", "akam --camera salon video bitrate 1024 gop 1"}, nil},
	} {
		h := base
		h.rssi = c.rssi
		got := strings.Join(wifiReport(h, "akam --camera salon"), "\n")
		for _, w := range c.wantAll {
			if !strings.Contains(got, w) {
				t.Errorf("%d dBm: lacks %q:\n%s", c.rssi, w, got)
			}
		}
		for _, w := range c.wantNone {
			if strings.Contains(got, w) {
				t.Errorf("%d dBm: must not contain %q:\n%s", c.rssi, w, got)
			}
		}
	}
	// settings already low: no command to suggest; only the value that is above is named
	h := base
	h.rssi, h.kbpsCfg, h.gop = -78, 1024, 1
	if got := strings.Join(wifiReport(h, "akam --camera salon"), "\n"); strings.Contains(got, "akam video") || !strings.Contains(got, "already low") {
		t.Error(got)
	}
	h.kbpsCfg, h.gop = 1536, 1
	if got := strings.Join(wifiReport(h, "akam --camera salon"), "\n"); !strings.Contains(got, "video bitrate 1024   (") || strings.Contains(got, "gop 1   (") {
		t.Error(got)
	}
	h.kbpsCfg, h.gop = 1024, 2
	if got := strings.Join(wifiReport(h, "akam --camera salon"), "\n"); !strings.Contains(got, "video gop 1   (") || strings.Contains(got, "bitrate 1024   (") {
		t.Error(got)
	}
	if wifiReport(camHealth{}, "akam") != nil {
		t.Error("an unknown signal must print nothing")
	}
}

func TestWifiReportNightFpsIsNotBlamedOnWifi(t *testing.T) {
	h := camHealth{rssi: -78, rate: 65, kbpsCfg: 1536, gop: 2, fpsCfg: 15, fps10: 100, night: "night"}
	got := strings.Join(wifiReport(h, "akam --camera salon"), "\n")
	if !strings.Contains(got, "at night this is normal") || strings.Contains(got, "frames are being dropped") {
		t.Error(got)
	}
	h.night = "day"
	got = strings.Join(wifiReport(h, "akam --camera salon"), "\n")
	if !strings.Contains(got, "frames are being dropped") || strings.Contains(got, "at night") {
		t.Error(got)
	}
}

func TestClockReport(t *testing.T) {
	now := time.Now().Unix()
	for _, c := range []struct {
		name  string
		h     camHealth
		want  string
		offer bool
	}{
		{"unset flag", camHealth{timeSet: "0", camEpoch: 946684900, pcEpoch: now}, "Clock: not set", true},
		{"unset by year", camHealth{camEpoch: 946684900, pcEpoch: now}, "reads 2000-01-01", true},
		{"ok", camHealth{timeSet: "1", camEpoch: now + 5, pcEpoch: now}, "Clock: set, +5 s", false},
		{"edge", camHealth{timeSet: "1", camEpoch: now - 120, pcEpoch: now}, "Clock: set, -120 s", false},
		{"off", camHealth{timeSet: "1", camEpoch: now - 3600*5, pcEpoch: now}, "off by 5 hour(s)", true},
		{"unreadable", camHealth{timeSet: "1"}, "Clock: set.", false},
		{"ntp off, 1 h off", camHealth{timeSet: "1", ntp: "off", camEpoch: now - 3600, pcEpoch: now}, "off by 1 hour(s)", true},
		{"ntp on, 1 h off: no offer", camHealth{timeSet: "1", ntp: "on", camEpoch: now - 3600, pcEpoch: now}, "may be wrong", false},
		{"ntp on, 1 h off: both times and difference", camHealth{timeSet: "1", ntp: "on", camEpoch: now - 3600, pcEpoch: now}, "difference 1 hour(s)", false},
		{"ntp on, unset: offer", camHealth{timeSet: "0", ntp: "on", camEpoch: 946684900, pcEpoch: now}, "Clock: not set", true},
		{"ntp on, reads 2000: offer", camHealth{ntp: "on", camEpoch: 946684900, pcEpoch: now}, "reads 2000-01-01", true},
	} {
		l, offer := clockReport(c.h)
		if !strings.Contains(strings.Join(l, "\n"), c.want) || offer != c.offer {
			t.Errorf("%s: %v offer=%v", c.name, l, offer)
		}
	}
	if l, o := clockReport(camHealth{}); l != nil || o {
		t.Error("unknown clock must print nothing and offer nothing")
	}
}

func TestDateGuard(t *testing.T) {
	ts := time.Date(2026, 5, 1, 12, 0, 5, 0, time.UTC)
	if remoteDateSet(ts) != "date -u -s '2026-05-01 12:00:05'" {
		t.Error(remoteDateSet(ts))
	}
	for _, ok := range []string{remoteDateRead(), remoteDateSet(ts), remoteDateSet(time.Unix(0, 0))} {
		if why := forbiddenRemote(ok); why != "" {
			t.Errorf("%q refused: %s", ok, why)
		}
	}
	for _, bad := range []string{
		"date -s '2026-05-01 12:00:05'", "date -u -s @1893456000", "date --set=@1", "date 050112002026", "date 0501120026.05",
		"date -u -s '2026-05-01 12:00:05'; reboot", "date -u -s '2026-05-01 12:00:05' && ls", "date -u -s '2026-05-01 12:00:05'\nls",
		"date -u -s '2026-05-01 12:00:05' | cat", "date -u -s '$(reboot)'", "date -u -s '2026-05-01 12:00:05' x",
		"date +%s; date 01010101", "echo x; date -s 'now'", "busybox date -s '2026-05-01 12:00:05'", "/bin/date -s x", "DATE -s x",
		" date +%s", "date +%s ", "date",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
	if forbiddenRemote("/usr/sbin/akam_ntp.sh status") != "" || forbiddenRemote("cat /proc/mtd") != "" {
		t.Error("the date rule must not touch other commands")
	}
}

func TestClockLostNote(t *testing.T) {
	if got := clockLostNote("on"); strings.Contains(got, "lost") || !strings.Contains(got, "corrects") {
		t.Error(got)
	}
	for _, n := range []string{"off", ""} {
		if got := clockLostNote(n); !strings.Contains(got, "lost at the next reboot") {
			t.Error(got)
		}
	}
}
