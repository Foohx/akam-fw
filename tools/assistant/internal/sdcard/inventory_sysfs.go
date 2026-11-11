package sdcard

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Sys is the view of the system used by the guards: paths are injectable so that tests run on a
// fake /sys, /proc/self/mountinfo and /dev built in a temp directory.
type Sys struct {
	SysRoot   string // /sys
	MountInfo string // /proc/self/mountinfo
	Swaps     string // /proc/swaps
	DevDir    string // /dev
	// GuardPaths must never sit on the target disk: the repository, home, working and temp directories.
	GuardPaths []string
}

// RealSys is the running system, with the usual guard paths.
func RealSys() Sys {
	s := Sys{SysRoot: "/sys", MountInfo: "/proc/self/mountinfo", Swaps: "/proc/swaps", DevDir: "/dev"}
	if h, err := os.UserHomeDir(); err == nil {
		s.GuardPaths = append(s.GuardPaths, h)
	}
	if wd, err := os.Getwd(); err == nil {
		s.GuardPaths = append(s.GuardPaths, wd)
	}
	s.GuardPaths = append(s.GuardPaths, os.TempDir())
	return s
}

var wholeDiskRe = regexp.MustCompile(`^(sd[a-z]+|mmcblk[0-9]+)$`)

func readStr(p string) string {
	b, err := os.ReadFile(p)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

type mountEnt struct {
	majmin, point, fstype, source string
}

func unescapeMount(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+3 < len(s) {
			if n, err := strconv.ParseUint(s[i+1:i+4], 8, 8); err == nil {
				b.WriteByte(byte(n))
				i += 3
				continue
			}
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

func (s Sys) mounts() []mountEnt {
	f, err := os.Open(s.MountInfo)
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []mountEnt
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		fl := strings.Fields(sc.Text())
		sep := -1
		for i, x := range fl {
			if x == "-" {
				sep = i
				break
			}
		}
		if sep < 6 || len(fl) < sep+3 {
			continue
		}
		out = append(out, mountEnt{majmin: fl[2], point: unescapeMount(fl[4]), fstype: fl[sep+1], source: unescapeMount(fl[sep+2])})
	}
	return out
}

// diskNodes returns the names and maj:min of a disk and its partitions.
func (s Sys) diskNodes(name string) (names []string, majmins map[string]bool) {
	majmins = map[string]bool{}
	base := filepath.Join(s.SysRoot, "class", "block", name)
	names = []string{name}
	majmins[readStr(filepath.Join(base, "dev"))] = true
	es, _ := os.ReadDir(base)
	for _, e := range es {
		if strings.HasPrefix(e.Name(), name) {
			if _, err := os.Stat(filepath.Join(base, e.Name(), "partition")); err == nil {
				names = append(names, e.Name())
				majmins[readStr(filepath.Join(base, e.Name(), "dev"))] = true
			}
		}
	}
	return
}

func (s Sys) holders(name string, nodes []string) string {
	base := filepath.Join(s.SysRoot, "class", "block", name)
	for _, n := range nodes {
		d := filepath.Join(base, "holders")
		if n != name {
			d = filepath.Join(base, n, "holders")
		}
		if es, err := os.ReadDir(d); err == nil && len(es) > 0 {
			return fmt.Sprintf("%s is held by %s", n, es[0].Name())
		}
	}
	return ""
}

// loopBacked reports a loop device whose backing file is one of the nodes of the disk.
func (s Sys) loopBacked(nodes []string) string {
	es, _ := os.ReadDir(filepath.Join(s.SysRoot, "class", "block"))
	for _, e := range es {
		if !strings.HasPrefix(e.Name(), "loop") {
			continue
		}
		bf := readStr(filepath.Join(s.SysRoot, "class", "block", e.Name(), "loop", "backing_file"))
		if !strings.HasPrefix(bf, "/dev/") {
			continue
		}
		for _, n := range nodes {
			if filepath.Base(bf) == n {
				return fmt.Sprintf("%s is the backing device of %s", n, e.Name())
			}
		}
	}
	return ""
}

func (s Sys) onDisk(m mountEnt, nodes []string, majmins map[string]bool) bool {
	if strings.HasPrefix(m.source, "/dev/") {
		b := filepath.Base(m.source)
		for _, n := range nodes {
			if n == b {
				return true
			}
		}
		// a source named by a link (/dev/disk/by-uuid/...) is caught by its device number below
	}
	return m.majmin != "" && !strings.HasPrefix(m.majmin, "0:") && majmins[m.majmin]
}

func underPath(p, dir string) bool {
	p, dir = filepath.Clean(p), filepath.Clean(dir)
	return p == dir || dir == "/" || strings.HasPrefix(p, dir+"/")
}

// Sys implements Inventory (Linux: sysfs, mountinfo, swaps). It only reads files, so it compiles everywhere.
var _ Inventory = Sys{}

// Devices lists the whole-disk nodes (sdX, mmcblkN) known to sysfs.
func (s Sys) Devices() []string {
	es, _ := os.ReadDir(filepath.Join(s.SysRoot, "class", "block"))
	var out []string
	for _, e := range es {
		if wholeDiskRe.MatchString(e.Name()) {
			out = append(out, filepath.Join(s.DevDir, e.Name()))
		}
	}
	return out
}

// Inspect reads the facts of dev. Only the facts: Evaluate decides.
func (s Sys) Inspect(dev string, ctx Context) (Facts, error) {
	name := filepath.Base(dev)
	f := Facts{OS: "linux", Path: dev, Name: name}
	if filepath.Dir(dev) != filepath.Clean(s.DevDir) || !wholeDiskRe.MatchString(name) {
		return f, nil // not a whole disk: Evaluate refuses
	}
	f.Whole = true
	if real, err := filepath.EvalSymlinks(dev); err == nil && real != dev {
		return f, fmt.Errorf("%s resolves to %s: give the real device", dev, real)
	}
	base := filepath.Join(s.SysRoot, "class", "block", name)
	if _, err := os.Stat(base); err != nil {
		return f, fmt.Errorf("%s is unknown to the system", dev)
	}
	nodes, majmins := s.diskNodes(name)
	mounts := s.mounts()
	for _, m := range mounts {
		if s.onDisk(m, nodes, majmins) {
			f.Mounts = append(f.Mounts, Mount{Node: m.source, Point: m.point, FS: m.fstype})
		}
	}
	if sf, err := os.Open(s.Swaps); err == nil {
		defer sf.Close()
		sc := bufio.NewScanner(sf)
	swaps:
		for sc.Scan() {
			fl := strings.Fields(sc.Text())
			if len(fl) > 0 && strings.HasPrefix(fl[0], "/dev/") {
				for _, n := range nodes {
					if filepath.Base(fl[0]) == n {
						f.Roles = append(f.Roles, RoleSwap)
						break swaps
					}
				}
			}
		}
	}
	if why := s.holders(name, nodes); why != "" {
		f.InStack = why
	} else if why := s.loopBacked(nodes); why != "" {
		f.InStack = why
	}
	// the repository, home, working and temp directories must not live on this disk
	for _, gp := range ctx.GuardPaths {
		var best *mountEnt
		for i := range mounts {
			if underPath(gp, mounts[i].point) && (best == nil || len(mounts[i].point) >= len(best.point)) {
				best = &mounts[i]
			}
		}
		if best != nil && s.onDisk(*best, nodes, majmins) {
			f.GuardHit = gp
			break
		}
	}
	f.RemovableRaw = readStr(filepath.Join(base, "removable"))
	f.DevType = readStr(filepath.Join(base, "device", "type"))
	if strings.HasPrefix(name, "mmcblk") {
		f.Bus = "mmc"
	} else if real, err := filepath.EvalSymlinks(base); err == nil && strings.Contains(real, "/usb") {
		f.Bus = "usb"
	} else {
		f.Bus = "other"
	}
	switch f.RemovableRaw {
	case "1":
		f.Removable = Yes
	case "0":
		f.Removable = No
	}
	secs, _ := strconv.ParseInt(readStr(filepath.Join(base, "size")), 10, 64)
	f.Size = secs * 512
	f.Model = strings.Join(strings.Fields(readStr(filepath.Join(base, "device", "model"))), " ")
	if f.Model == "" {
		f.Model = strings.Join(strings.Fields(readStr(filepath.Join(base, "device", "name"))), " ")
	}
	f.Vendor = readStr(filepath.Join(base, "device", "vendor"))
	f.Serial = readStr(filepath.Join(base, "device", "serial"))
	f.WWN = readStr(filepath.Join(base, "device", "wwid"))
	f.DevNum = readStr(filepath.Join(base, "dev"))
	return f, nil
}

// Check decides whether dev is an eligible card. The error text is the reason for a refusal.
func (s Sys) Check(dev string) (*Candidate, error) {
	return checkDevice(s, dev, Context{GuardPaths: s.GuardPaths})
}

// List returns every eligible disk, sorted.
func (s Sys) List() []Candidate { return listDevices(s, Context{GuardPaths: s.GuardPaths}) }

// Recheck is the whole guard again, right before a destructive step: still eligible, same identity.
func (s Sys) Recheck(dev, ident string) (*Candidate, error) {
	return recheckDevice(s, dev, ident, Context{GuardPaths: s.GuardPaths})
}
