package sdcard

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// Size range of an acceptable card (backend_linux.sh).
const (
	MinBytes = 128 << 20
	MaxBytes = 128 << 30
)

// Mount is one mounted node of a disk.
type Mount struct {
	Node  string `json:"node"`
	Point string `json:"point"`
}

// Candidate is an eligible whole disk.
type Candidate struct {
	Dev    string  `json:"dev"`
	Name   string  `json:"name"`
	Size   int64   `json:"size"`
	Model  string  `json:"model"`
	Tran   string  `json:"tran"`
	Mounts []Mount `json:"mounts"`
	// Ident is what must not change between the check and the write: size, model, serial, wwn,
	// vendor, major:minor. The parent passes it to the helper, which compares it again.
	Ident string `json:"ident"`
}

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

var systemMounts = []string{"/", "/boot", "/usr", "/var", "/etc", "/home", "/opt", "/srv", "/nix"}

func isSystemMount(p string) bool {
	for _, s := range systemMounts {
		if p == s || (s != "/" && strings.HasPrefix(p, s+"/")) {
			return true
		}
	}
	return false
}

// Check decides whether dev is an eligible card. The error text is the reason for a refusal.
func (s Sys) Check(dev string) (*Candidate, error) {
	name := filepath.Base(dev)
	if filepath.Dir(dev) != filepath.Clean(s.DevDir) || !wholeDiskRe.MatchString(name) {
		return nil, fmt.Errorf("%s is not a whole USB/SD disk node like /dev/sdb or /dev/mmcblk0 (partitions, NVMe, loop and zram nodes are refused)", dev)
	}
	if real, err := filepath.EvalSymlinks(dev); err == nil && real != dev {
		return nil, fmt.Errorf("%s resolves to %s: give the real device", dev, real)
	}
	base := filepath.Join(s.SysRoot, "class", "block", name)
	if _, err := os.Stat(base); err != nil {
		return nil, fmt.Errorf("%s is unknown to the system", dev)
	}
	nodes, majmins := s.diskNodes(name)
	mounts := s.mounts()
	var mine []Mount
	for _, m := range mounts {
		if s.onDisk(m, nodes, majmins) {
			if isSystemMount(m.point) {
				return nil, fmt.Errorf("%s carries the system path %s", dev, m.point)
			}
			mine = append(mine, Mount{Node: m.source, Point: m.point})
		}
	}
	if f, err := os.Open(s.Swaps); err == nil {
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			fl := strings.Fields(sc.Text())
			if len(fl) > 0 && strings.HasPrefix(fl[0], "/dev/") {
				for _, n := range nodes {
					if filepath.Base(fl[0]) == n {
						f.Close()
						return nil, fmt.Errorf("%s carries swap space", dev)
					}
				}
			}
		}
		f.Close()
	}
	if why := s.holders(name, nodes); why != "" {
		return nil, fmt.Errorf("%s is part of a storage stack (%s)", dev, why)
	}
	if why := s.loopBacked(nodes); why != "" {
		return nil, fmt.Errorf("%s is part of a storage stack (%s)", dev, why)
	}
	// the repository, home, working and temp directories must not live on this disk
	for _, gp := range s.GuardPaths {
		var best *mountEnt
		for i := range mounts {
			if underPath(gp, mounts[i].point) && (best == nil || len(mounts[i].point) >= len(best.point)) {
				best = &mounts[i]
			}
		}
		if best != nil && s.onDisk(*best, nodes, majmins) {
			return nil, fmt.Errorf("%s carries %s (the repository, your home, the working or the temp directory)", dev, gp)
		}
	}
	rm := readStr(filepath.Join(base, "removable"))
	tran := ""
	if strings.HasPrefix(name, "mmcblk") {
		if readStr(filepath.Join(base, "device", "type")) != "SD" {
			return nil, fmt.Errorf("%s is not an SD card (device type %q)", dev, readStr(filepath.Join(base, "device", "type")))
		}
		tran = "mmc"
	} else {
		if rm != "1" {
			return nil, fmt.Errorf("%s is not removable (removable=%q)", dev, rm)
		}
		if real, err := filepath.EvalSymlinks(base); err != nil || !strings.Contains(real, "/usb") {
			return nil, fmt.Errorf("%s is not USB-attached", dev)
		}
		tran = "usb"
	}
	secs, _ := strconv.ParseInt(readStr(filepath.Join(base, "size")), 10, 64)
	size := secs * 512
	if size <= 0 {
		return nil, fmt.Errorf("%s reports no medium (size 0)", dev)
	}
	if size < MinBytes || size > MaxBytes {
		return nil, fmt.Errorf("%s size %d bytes is outside 128 MiB .. 128 GiB", dev, size)
	}
	model := strings.Join(strings.Fields(readStr(filepath.Join(base, "device", "model"))), " ")
	if model == "" {
		model = strings.Join(strings.Fields(readStr(filepath.Join(base, "device", "name"))), " ")
	}
	vendor := readStr(filepath.Join(base, "device", "vendor"))
	serial := readStr(filepath.Join(base, "device", "serial"))
	wwn := readStr(filepath.Join(base, "device", "wwid"))
	c := &Candidate{Dev: dev, Name: name, Size: size, Model: model, Tran: tran, Mounts: mine,
		Ident: fmt.Sprintf("%d|%s|%s|%s|%s|%s", size, model, serial, wwn, vendor, readStr(filepath.Join(base, "dev")))}
	return c, nil
}

// List returns every eligible disk, sorted.
func (s Sys) List() []Candidate {
	es, _ := os.ReadDir(filepath.Join(s.SysRoot, "class", "block"))
	var out []Candidate
	for _, e := range es {
		if !wholeDiskRe.MatchString(e.Name()) {
			continue
		}
		if c, err := s.Check(filepath.Join(s.DevDir, e.Name())); err == nil {
			out = append(out, *c)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Dev < out[j].Dev })
	return out
}

// ErrIdentChanged means the disk is not the one that was confirmed.
var ErrIdentChanged = errors.New("the disk changed since it was checked: card replaced?")

// Recheck is the whole guard again, right before a destructive step: still eligible, same identity.
func (s Sys) Recheck(dev, ident string) (*Candidate, error) {
	c, err := s.Check(dev)
	if err != nil {
		return nil, err
	}
	if c.Ident != ident {
		return nil, fmt.Errorf("%s changed since it was checked (was: %s, now: %s): %w", dev, ident, c.Ident, ErrIdentChanged)
	}
	return c, nil
}

// Unmount unmounts every mounted node of the disk (privileged).
func (s Sys) mountPointsOf(c *Candidate) []string {
	var pts []string
	for _, m := range c.Mounts {
		pts = append(pts, m.Point)
	}
	return pts
}
