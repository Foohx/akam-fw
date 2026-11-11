package discover

import (
	"sort"
	"strconv"
	"time"
)

// Known is one remembered camera, read from its state directory.
type Known struct {
	Name    string
	IP      string
	Host    string
	FW      string
	Seen    time.Time // last time it answered (zero: never recorded)
	HostKey string    // SSH host key fingerprint pinned for it ("" when none)
}

// Camera is one camera met during a search, with what Identify said and, when the caller fetched
// it, the SSH host key fingerprint the address presented.
type Camera struct {
	IP      string
	Kind    Kind
	Host    string
	FW      string
	HostKey string
}

// Row is one line of the camera table.
type Row struct {
	Name     string // remembered name, "" for a new camera
	Camera          // zero for a vanished camera
	Found    bool   // answered now
	OldIP    string // set when a remembered camera answered from another address (same host key)
	LastIP   string // vanished camera: where it was last seen
	LastSeen time.Time
}

// Merge reconciles remembered cameras with the ones found now. A remembered camera matches by
// address first, then by host key (the same key at a new address: the camera moved, OldIP says
// where it was). A remembered camera nothing matched stays in the table as last seen. Found cameras
// no remembered one matches are new rows. Order: found rows by address, then vanished ones by name.
func Merge(known []Known, found []Camera, now time.Time) []Row {
	used := make([]bool, len(found))
	var rows, gone []Row
	for _, k := range known {
		idx := -1
		for i, f := range found {
			if !used[i] && f.IP == k.IP {
				idx = i
				break
			}
		}
		moved := false
		if idx < 0 && k.HostKey != "" {
			for i, f := range found {
				if !used[i] && f.HostKey == k.HostKey {
					idx, moved = i, true
					break
				}
			}
		}
		if idx < 0 {
			gone = append(gone, Row{Name: k.Name, LastIP: k.IP, LastSeen: k.Seen})
			continue
		}
		used[idx] = true
		r := Row{Name: k.Name, Camera: found[idx], Found: true, LastSeen: now}
		if moved {
			r.OldIP = k.IP
		}
		rows = append(rows, r)
	}
	for i, f := range found {
		if !used[i] {
			rows = append(rows, Row{Camera: f, Found: true, LastSeen: now})
		}
	}
	sort.SliceStable(rows, func(a, b int) bool { return ipLess(rows[a].IP, rows[b].IP) })
	sort.SliceStable(gone, func(a, b int) bool { return gone[a].Name < gone[b].Name })
	return append(rows, gone...)
}

// Ago is "just now", "5 min ago", "3 hours ago", "3 days ago" or "never".
func Ago(t, now time.Time) string {
	if t.IsZero() {
		return "never"
	}
	d := now.Sub(t)
	switch {
	case d < time.Minute:
		return "just now"
	case d < time.Hour:
		return ago(int(d/time.Minute), "min")
	case d < 48*time.Hour:
		return ago(int(d/time.Hour), "hour")
	}
	return ago(int(d/(24*time.Hour)), "day")
}

func ago(n int, unit string) string {
	s := strconv.Itoa(n) + " " + unit
	if n != 1 && unit != "min" {
		s += "s"
	}
	return s + " ago"
}
