package state

import (
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Seen is what discovery remembers about a camera, beyond camera_ip and camera_kind. The keys
// camera_host, camera_fw and camera_seen are ignored by the bash wizard (extra keys are ignored).
type Seen struct {
	IP, Kind, Host, FW string
	At                 time.Time // camera_seen, UTC; zero when absent or malformed
}

// SeenTimeLayout is the format of camera_seen.
const SeenTimeLayout = "2006-01-02T15:04:05Z"

// ReadSeen reads the discovery keys of a camera (zero values when it has no state).
func (c *Cameras) ReadSeen(name string) Seen {
	var s Seen
	b, err := os.ReadFile(filepath.Join(c.DirOf(name), "state"))
	if err != nil {
		return s
	}
	for _, l := range strings.Split(string(b), "\n") {
		k, v, ok := strings.Cut(strings.TrimRight(l, "\r"), "=")
		if !ok {
			continue
		}
		switch k {
		case "camera_ip":
			s.IP = v
		case "camera_kind":
			s.Kind = v
		case "camera_host":
			s.Host = v
		case "camera_fw":
			s.FW = v
		case "camera_seen":
			if t, err := time.Parse(SeenTimeLayout, v); err == nil {
				s.At = t
			}
		}
	}
	return s
}

// RecordSeen writes camera_ip, camera_host, camera_fw and camera_seen into the open store. An
// empty host or firmware leaves the old value alone (an older image does not report them). Values
// come from the network, so only printable ASCII is kept, at most 64 characters.
func RecordSeen(st *Store, ip, host, fw string, now time.Time) error {
	if err := st.Set("camera_ip", ip); err != nil {
		return err
	}
	if h := lineSafe(host); h != "" {
		if err := st.Set("camera_host", h); err != nil {
			return err
		}
	}
	if f := lineSafe(fw); f != "" {
		if err := st.Set("camera_fw", f); err != nil {
			return err
		}
	}
	return st.Set("camera_seen", now.UTC().Format(SeenTimeLayout))
}

func lineSafe(s string) string {
	var b strings.Builder
	for _, r := range s {
		if r > 0x20 && r < 0x7f && b.Len() < 64 {
			b.WriteRune(r)
		}
	}
	return b.String()
}
