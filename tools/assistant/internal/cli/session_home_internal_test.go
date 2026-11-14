package cli

import (
	"strings"
	"testing"
	"time"
)

// The header badges come from what a flow read: ready, clock, Wi-Fi (specs/installer.md, section 10).
func TestHealthBadges(t *testing.T) {
	now := time.Now().Unix()
	for _, tc := range []struct {
		name string
		h    camHealth
		want string
	}{
		{"synced", camHealth{rssi: -58, camEpoch: now, pcEpoch: now, ntp: "on", ntpSync: "ok"}, "ok:ready ok:synced ok:-58 dBm"},
		{"clock ok", camHealth{rssi: -65, camEpoch: now, pcEpoch: now}, "ok:ready ok:clock ok ok:-65 dBm"},
		{"clock off weak", camHealth{rssi: -73, camEpoch: now - 3*3600, pcEpoch: now}, "ok:ready warn:clock off warn:weak wifi"},
		{"unset poor", camHealth{rssi: -80, timeSet: "0"}, "ok:ready warn:clock unset error:poor wifi"},
		{"nothing known", camHealth{}, "ok:ready"},
	} {
		var got []string
		for _, b := range healthBadges(tc.h) {
			got = append(got, b.Level+":"+b.Text)
		}
		if s := strings.Join(got, " "); s != tc.want {
			t.Errorf("%s: %q, want %q", tc.name, s, tc.want)
		}
	}
}

func TestShortAgo(t *testing.T) {
	now := time.Now()
	for d, want := range map[time.Duration]string{time.Second: "just now", 2 * time.Minute: "2 min", 3 * time.Hour: "3 h", 72 * time.Hour: "3 d"} {
		if got := shortAgo(now.Add(-d), now); got != want {
			t.Errorf("%v: %q, want %q", d, got, want)
		}
	}
}
