package state

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestRecordAndReadSeen(t *testing.T) {
	base := t.TempDir()
	c, err := InitCameras(base, "salon", false)
	if err != nil {
		t.Fatal(err)
	}
	dir := c.DirOf("salon")
	st, err := Open(dir, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 11, 11, 8, 30, 0, 0, time.UTC)
	if err := RecordSeen(st, "192.0.2.21", "akam-01", "b35-2af81c5", now); err != nil {
		t.Fatal(err)
	}
	s := c.ReadSeen("salon")
	if s.IP != "192.0.2.21" || s.Host != "akam-01" || s.FW != "b35-2af81c5" || !s.At.Equal(now) {
		t.Errorf("seen = %+v", s)
	}
	// an older image reports nothing: old values stay
	if err := RecordSeen(st, "192.0.2.22", "", "", now.Add(time.Hour)); err != nil {
		t.Fatal(err)
	}
	s = c.ReadSeen("salon")
	if s.IP != "192.0.2.22" || s.Host != "akam-01" || s.FW != "b35-2af81c5" {
		t.Errorf("seen = %+v", s)
	}
	// hostile strings cannot add a line or control characters
	if err := RecordSeen(st, "192.0.2.22", "x\ny\x1b[2J", "v 1\r\ncamera_ip=evil", now); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(dir, "state"))
	if s = c.ReadSeen("salon"); s.IP != "192.0.2.22" || s.Host != "xy[2J" {
		t.Errorf("seen = %+v\n%s", s, b)
	}
	if z := c.ReadSeen("nobody"); z != (Seen{}) {
		t.Errorf("zero = %+v", z)
	}
}
