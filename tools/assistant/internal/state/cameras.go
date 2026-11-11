package state

import (
	"bufio"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
)

// Steps are the wizard steps, in the order of WIZ_STEPS in steps_host.sh.
var Steps = []string{"doctor", "get-kit", "prepare-backup-sd", "find-camera", "backup", "image", "prepare-flash-sd", "flash", "verify"}

// NameMax is the longest camera name.
const NameMax = 20

// NameValid: lowercase letters, digits and '-', at most 20, not starting or ending with '-', and not
// a Windows reserved device name (con, nul, com1, ...), so a camera name works on any computer the
// backup folder is moved to. It is for NEW names; a camera that already exists under a reserved name
// keeps working (see NameExists).
func NameValid(n string) bool { return nameShape(n) && !fsutil.ReservedName(n) }

// NameExists is the shape check without the reserved-name rule: for names read back from disk.
// On Windows a reserved name is refused here too: no such folder can exist there, and a name read
// from a copied current_camera must never reach a path.
func NameExists(n string) bool { return nameExistsFor(runtime.GOOS, n) }

func nameExistsFor(goos, n string) bool {
	return nameShape(n) && !(goos == "windows" && fsutil.ReservedName(n))
}

func nameShape(n string) bool {
	if n == "" || strings.HasPrefix(n, "-") || strings.HasSuffix(n, "-") || len(n) > NameMax {
		return false
	}
	for _, r := range n {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '-') {
			return false
		}
	}
	return true
}

// Cameras is the multi-camera layout under Base (cameras.sh): the camera "default" lives in Base
// itself, every other one in Base/cameras/NAME.
type Cameras struct {
	Base  string
	Name  string
	Fresh bool // first run: no camera has any state yet
	Dry   bool
}

// DirOf returns the state directory of a camera.
func (c *Cameras) DirOf(name string) string {
	if name == "default" {
		return c.Base
	}
	return filepath.Join(c.Base, "cameras", name)
}

// CameraExists reports whether a camera called name already has a state under base.
func CameraExists(base, name string) bool {
	c := &Cameras{Base: base}
	d := c.DirOf(name)
	return exists(filepath.Join(d, "state")) || exists(filepath.Join(d, "state.dry"))
}

// Slug is "salon-" for a named camera and "" for "default".
func (c *Cameras) Slug() string {
	if c.Name == "default" {
		return ""
	}
	return c.Name + "-"
}

// InitCameras picks the camera: wanted (--camera), else the one used last, else "default".
func InitCameras(base, wanted string, dry bool) (*Cameras, error) {
	c := &Cameras{Base: base, Dry: dry}
	if err := fsutil.PrivateMkdir(base); err != nil {
		return nil, err
	}
	_ = os.Chmod(base, 0o700)
	if !exists(filepath.Join(base, "current_camera")) && !exists(filepath.Join(base, "state")) && !exists(filepath.Join(base, "cameras")) {
		c.Fresh = true
	}
	if wanted != "" {
		c.Name = wanted
	} else if n := c.CurrentRead(); n != "" {
		c.Name = n
	} else {
		c.Name = "default"
	}
	return c, nil
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

// CurrentRead returns the name stored by the last run ("" when none or invalid).
func (c *Cameras) CurrentRead() string {
	f, err := os.Open(filepath.Join(c.Base, "current_camera"))
	if err != nil {
		return ""
	}
	defer f.Close()
	line, _ := bufio.NewReader(f).ReadString('\n')
	line = strings.TrimRight(line, "\r\n")
	if NameExists(line) {
		return line
	}
	return ""
}

// Remember writes current_camera (never in a rehearsal).
func (c *Cameras) Remember(name string) error {
	if c.Dry {
		return nil
	}
	return fsutil.AtomicWrite(filepath.Join(c.Base, "current_camera"), []byte(name+"\n"), 0o600)
}

// Known lists the cameras that have a state, "default" first.
func (c *Cameras) Known() []string {
	var out []string
	if exists(filepath.Join(c.Base, "state")) || exists(filepath.Join(c.Base, "state.dry")) || c.Name == "default" {
		out = append(out, "default")
	}
	ents, _ := os.ReadDir(filepath.Join(c.Base, "cameras"))
	var named []string
	for _, e := range ents {
		if e.IsDir() && NameExists(e.Name()) {
			named = append(named, e.Name())
		}
	}
	sort.Strings(named)
	return append(out, named...)
}

var doneRe = regexp.MustCompile(`^step_[a-z_]*_done=`)

// Summary is "3/9 steps done  192.168.1.50  last: find-camera".
func (c *Cameras) Summary(name string) string {
	f := filepath.Join(c.DirOf(name), "state")
	n := 0
	ip, last := "", ""
	if b, err := os.ReadFile(f); err == nil {
		lines := strings.Split(string(b), "\n")
		for _, l := range lines {
			if doneRe.MatchString(l) {
				n++
			}
			if ip == "" && strings.HasPrefix(l, "camera_ip=") {
				ip = l[len("camera_ip="):]
			}
		}
		for _, st := range Steps {
			for _, l := range lines {
				if strings.HasPrefix(l, "step_"+StepKey(st)+"_done=") {
					last = st
					break
				}
			}
		}
	}
	s := fmt.Sprintf("%d/%d steps done", n, len(Steps))
	if ip != "" {
		s += "  " + ip
	}
	if last != "" {
		s += "  last: " + last
	}
	return s
}

// HasLegacy: progress from before cameras had names sits in Base/state.
func (c *Cameras) HasLegacy() bool {
	if exists(filepath.Join(c.Base, "current_camera")) || exists(filepath.Join(c.Base, "cameras")) {
		return false
	}
	b, err := os.ReadFile(filepath.Join(c.Base, "state"))
	if err != nil {
		return false
	}
	for _, l := range strings.Split(string(b), "\n") {
		if doneRe.MatchString(l) {
			return true
		}
	}
	return false
}

// MoveFiles moves the state, log and reflash logs of old into the directory of new (never
// overwrites). The kit and the downloads are shared and stay where they are.
func (c *Cameras) MoveFiles(old, nw string) error {
	od, nd := c.DirOf(old), c.DirOf(nw)
	if exists(filepath.Join(nd, "state")) {
		return fmt.Errorf("a camera called '%s' already has a state", nw)
	}
	if err := fsutil.PrivateMkdir(nd); err != nil {
		return err
	}
	_ = os.Chmod(nd, 0o700)
	if old == "default" {
		bak := filepath.Join(od, "state.before-rename.bak")
		if exists(filepath.Join(od, "state")) && !exists(bak) {
			b, err := os.ReadFile(filepath.Join(od, "state"))
			if err != nil {
				return fmt.Errorf("could not keep a backup copy of %s/state: not moving anything", od)
			}
			if err := os.WriteFile(bak, b, 0o600); err != nil {
				return fmt.Errorf("could not keep a backup copy of %s/state: not moving anything", od)
			}
		}
		for _, f := range []string{"state", "state.dry", "akam.log", "reflash.log", "reflash-precheck.log", "rtsp_auth.err"} {
			if exists(filepath.Join(od, f)) {
				if err := fsutil.Rename(filepath.Join(od, f), filepath.Join(nd, f)); err != nil {
					return err
				}
			}
		}
		return nil
	}
	ents, _ := os.ReadDir(od)
	for _, e := range ents {
		if err := fsutil.Rename(filepath.Join(od, e.Name()), filepath.Join(nd, e.Name())); err != nil {
			return err
		}
	}
	_ = os.Remove(od)
	return nil
}

// BackupBeforeBinary keeps one untouched copy of an existing state file the first time this
// binary opens a directory (state.bak-before-binary). It reports whether a copy was made.
func BackupBeforeBinary(dir string) bool {
	src := filepath.Join(dir, "state")
	dst := filepath.Join(dir, "state.bak-before-binary")
	b, err := os.ReadFile(src)
	if err != nil || len(b) == 0 || exists(dst) {
		return false
	}
	return os.WriteFile(dst, b, 0o600) == nil
}
