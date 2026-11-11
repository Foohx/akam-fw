package sdcard

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// macOS disk facts from `diskutil ... -plist` (spec 11, 4.4.4). Everything here is pure or goes through the
// Runner, so it is unit-tested on Linux with canned XML; only the thin *_darwin.go files touch the OS.
// The decisions stay in Evaluate: this file only reads, and a fact that cannot be read is an error or an
// Unknown, never a default that lets a disk through.

// Runner runs an external program and returns its standard output.
type Runner interface {
	Run(ctx context.Context, name string, args ...string) ([]byte, error)
}

const (
	diskutilPath  = "/usr/sbin/diskutil"
	runnerMaxOut  = plistMaxBytes
	runnerTimeout = 60 * time.Second
)

// execRunner runs a real program with a minimal, fixed environment and bounded output.
type execRunner struct{}

func (execRunner) Run(ctx context.Context, name string, args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, runnerTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Env = []string{"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LANG=C", "LC_ALL=C"}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	var stderr strings.Builder
	cmd.Stderr = &limitedWriter{w: &stderr, n: 4096}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	data, rerr := io.ReadAll(io.LimitReader(out, runnerMaxOut+1))
	if len(data) > runnerMaxOut {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("%s: output too large", filepath.Base(name))
	}
	if werr := cmd.Wait(); werr != nil {
		return nil, fmt.Errorf("%s failed: %v: %s", filepath.Base(name), werr, strings.TrimSpace(stderr.String()))
	}
	return data, rerr
}

// limitedWriter keeps at most n bytes and drops the rest.
type limitedWriter struct {
	w io.Writer
	n int
}

func (l *limitedWriter) Write(p []byte) (int, error) {
	if l.n > 0 {
		k := len(p)
		if k > l.n {
			k = l.n
		}
		_, _ = l.w.Write(p[:k])
		l.n -= k
	}
	return len(p), nil
}

var (
	macDiskRe    = regexp.MustCompile(`^/dev/disk[0-9]{1,6}$`)
	macSliceRe   = regexp.MustCompile(`^/dev/disk[0-9]{1,6}s[0-9]{1,6}$`)
	macWholeIDRe = regexp.MustCompile(`^disk[0-9]{1,6}$`)
	macIDRe      = regexp.MustCompile(`^disk[0-9]{1,6}(s[0-9]{1,6})*$`)
)

// macInventory implements Inventory from diskutil.
type macInventory struct {
	run Runner
	// devNum returns "major:minor" of the raw node of a /dev/diskN (part of the identity token).
	devNum func(dev string) (string, error)
	// resolve resolves symbolic links of a guard path (/tmp is /private/tmp); nil = identity.
	resolve func(string) string
}

func newMacInventory(run Runner, devNum func(string) (string, error)) *macInventory {
	return &macInventory{run: run, devNum: devNum, resolve: func(p string) string {
		if r, err := filepath.EvalSymlinks(p); err == nil {
			return r
		}
		return p
	}}
}

func (m *macInventory) plist(args ...string) (Dict, error) {
	out, err := m.run.Run(context.Background(), diskutilPath, args...)
	if err != nil {
		return nil, err
	}
	return ParsePlist(out)
}

// Devices lists the whole disks diskutil knows.
func (m *macInventory) Devices() []string {
	d, err := m.plist("list", "-plist")
	if err != nil {
		return nil
	}
	arr, _ := d.Arr("WholeDisks")
	var out []string
	for _, v := range arr {
		if s, ok := v.(string); ok && macWholeIDRe.MatchString(s) {
			out = append(out, "/dev/"+s)
		}
	}
	return out
}

// Inspect reads the facts of /dev/diskN. A slice (/dev/diskNsM) yields a not-whole description so that Evaluate
// refuses it with its usual sentence; a raw node or anything else is an error. Any fact that cannot be read
// makes the whole inspection fail.
func (m *macInventory) Inspect(dev string, ctx Context) (Facts, error) {
	if macSliceRe.MatchString(dev) {
		return Facts{OS: "darwin", Path: dev, Name: strings.TrimPrefix(dev, "/dev/"), Whole: false}, nil
	}
	if !macDiskRe.MatchString(dev) {
		return Facts{}, fmt.Errorf("%s is not a /dev/diskN node (raw /dev/rdisk nodes are chosen by the program itself)", dev)
	}
	id := strings.TrimPrefix(dev, "/dev/")
	info, err := m.plist("info", "-plist", dev)
	if err != nil {
		return Facts{}, fmt.Errorf("cannot read %s: %w", dev, err)
	}
	list, err := m.plist("list", "-plist")
	if err != nil {
		return Facts{}, fmt.Errorf("cannot list the disks: %w", err)
	}
	root, err := m.plist("info", "-plist", "/")
	if err != nil {
		return Facts{}, fmt.Errorf("cannot read the system volume: %w", err)
	}
	var dn string
	if m.devNum != nil {
		if dn, err = m.devNum(dev); err != nil {
			return Facts{}, fmt.Errorf("cannot read the device number of %s: %w", dev, err)
		}
	}
	resolve := m.resolve
	if resolve == nil {
		resolve = func(p string) string { return p }
	}
	return buildMacFacts(id, info, list, root, dn, ctx.GuardPaths, resolve)
}

// wholeOf maps a slice identifier (disk0s2) to its whole disk (disk0).
func wholeOf(id string) string {
	if len(id) > 4 {
		if i := strings.Index(id[4:], "s"); i >= 0 {
			return id[:4+i]
		}
	}
	return id
}

// storeDisks returns the whole disks named by APFSPhysicalStores in a dict (a list entry or an info dict).
func storeDisks(d Dict) []string {
	var out []string
	arr, _ := d.Arr("APFSPhysicalStores")
	for _, v := range arr {
		sd, ok := v.(Dict)
		if !ok {
			continue
		}
		if s, ok := sd.Str("DeviceIdentifier"); ok {
			out = append(out, wholeOf(s))
		} else if s, ok := sd.Str("APFSPhysicalStore"); ok {
			out = append(out, wholeOf(s))
		}
	}
	return out
}

func triOf(d Dict, keys ...string) Tri {
	for _, k := range keys {
		if b, ok := d.Bool(k); ok {
			if b {
				return Yes
			}
			return No
		}
	}
	return Unknown
}

func macBus(proto string) string {
	switch strings.ToLower(proto) {
	case "usb":
		return "usb"
	case "secure digital", "sd", "sdxc", "sdhc":
		return "sd"
	case "sata", "ata":
		return "sata"
	case "pci-express", "pci", "nvme", "apple fabric":
		return "nvme"
	case "virtual interface", "disk image":
		return "virtual"
	case "scsi", "sas":
		return "scsi"
	case "":
		return "unknown"
	}
	return "other"
}

// buildMacFacts is the pure part of Inspect.
func buildMacFacts(id string, info, list, root Dict, devNum string, guards []string, resolve func(string) string) (Facts, error) {
	if got, _ := info.Str("DeviceIdentifier"); got != id {
		return Facts{}, fmt.Errorf("diskutil described %q, not %q", got, id)
	}
	whole, ok := info.Bool("WholeDisk")
	if !ok {
		return Facts{}, errors.New("diskutil did not say whether the node is a whole disk")
	}
	f := Facts{OS: "darwin", Path: "/dev/" + id, Whole: whole, DevNum: devNum}
	f.Name, _ = info.Str("IORegistryEntryName")
	f.Model, _ = info.Str("MediaName")
	if f.Name == "" {
		f.Name = id
	}
	f.LayoutID, _ = info.Str("DiskUUID")
	if n, ok := info.Int("TotalSize"); ok {
		f.Size = n
	} else if n, ok := info.Int("Size"); ok {
		f.Size = n
	}
	if n, ok := info.Int("DeviceBlockSize"); ok {
		f.SectorSize = int(n)
	}
	proto, _ := info.Str("BusProtocol")
	f.Bus = macBus(proto)
	f.Internal = triOf(info, "Internal")
	f.Removable = triOf(info, "RemovableMedia", "Removable")
	// physical only: a disk image, a virtual interface or an unreadable kind is virtual (fail closed)
	vp, _ := info.Str("VirtualOrPhysical")
	img, _ := info.Bool("SystemImage")
	f.Virtual = vp != "Physical" || img
	if w, ok := info.Bool("WritableMedia"); ok && !w {
		f.ReadOnly = true
	}
	if w, ok := info.Bool("Writable"); ok && !w {
		f.ReadOnly = true
	}

	entries, ok := list.Arr("AllDisksAndPartitions")
	if !ok {
		return Facts{}, errors.New("diskutil list has no AllDisksAndPartitions")
	}
	found := false
	var apfsMounts []Mount
	containerOf := map[string][]string{} // container whole disk -> physical store whole disks
	entryByID := map[string]Dict{}
	for _, v := range entries {
		e, ok := v.(Dict)
		if !ok {
			return Facts{}, errors.New("diskutil list: an entry is not a dict")
		}
		eid, ok := e.Str("DeviceIdentifier")
		if !ok || !macIDRe.MatchString(eid) {
			return Facts{}, errors.New("diskutil list: an entry has no valid DeviceIdentifier")
		}
		stores := storeDisks(e)
		containerOf[eid] = stores
		entryByID[eid] = e
		if eid == id {
			found = true
			if mp, _ := e.Str("MountPoint"); mp != "" {
				f.Mounts = append(f.Mounts, Mount{Node: "/dev/" + eid, Point: mp})
			}
			parts, _ := e.Arr("Partitions")
			for _, pv := range parts {
				pd, ok := pv.(Dict)
				if !ok {
					return Facts{}, errors.New("diskutil list: a partition is not a dict")
				}
				pid, _ := pd.Str("DeviceIdentifier")
				if mp, _ := pd.Str("MountPoint"); mp != "" {
					f.Mounts = append(f.Mounts, Mount{Node: "/dev/" + pid, Point: mp})
				}
			}
		}
		for _, s := range stores {
			if s != id {
				continue
			}
			// an APFS container with a physical store on this disk: its volumes live on it
			vols, _ := e.Arr("APFSVolumes")
			for _, vv := range vols {
				vd, ok := vv.(Dict)
				if !ok {
					return Facts{}, errors.New("diskutil list: an APFS volume is not a dict")
				}
				vid, _ := vd.Str("DeviceIdentifier")
				if mp, _ := vd.Str("MountPoint"); mp != "" {
					apfsMounts = append(apfsMounts, Mount{Node: "/dev/" + vid, Point: mp, FS: "apfs"})
				}
			}
		}
	}
	if !found {
		return Facts{}, fmt.Errorf("diskutil list does not know %s", id)
	}
	f.Mounts = append(f.Mounts, apfsMounts...)
	for _, m := range apfsMounts {
		if isDarwinSystemMount(m.Point) {
			f.InStack = "an APFS container on this disk holds a system volume of this Mac"
			break
		}
	}

	// second, independent check: which whole disk backs "/"?
	rootParent, ok := root.Str("ParentWholeDisk")
	if !ok || !macIDRe.MatchString(rootParent) {
		return Facts{}, errors.New("cannot tell which disk holds the system volume")
	}
	rootDisks := map[string]bool{rootParent: true}
	rootStores := storeDisks(root)
	for _, s := range rootStores {
		rootDisks[s] = true
	}
	// fail closed: the disk behind "/" must be in the list, and an APFS container behind "/" must name at least
	// one physical store (from the list or from the volume itself), or a card holding that store could hide
	if rootEntry, ok := entryByID[rootParent]; !ok {
		return Facts{}, errors.New("cannot tell which disk holds the system volume: it is not in the disk list")
	} else if vols, _ := rootEntry.Arr("APFSVolumes"); len(vols) > 0 && len(rootStores) == 0 && len(containerOf[rootParent]) == 0 {
		return Facts{}, errors.New("cannot tell which physical disk holds the system APFS container")
	}
	for changed := true; changed; { // follow containers to their physical disks
		changed = false
		for c, stores := range containerOf {
			if !rootDisks[c] {
				continue
			}
			for _, s := range stores {
				if !rootDisks[s] {
					rootDisks[s] = true
					changed = true
				}
			}
		}
	}
	if rootDisks[id] {
		f.Roles = append(f.Roles, RoleSystem)
	}

	for _, g := range guards {
		for _, cand := range []string{g, resolve(g)} {
			for _, m := range f.Mounts {
				if m.Point != "" && (cand == m.Point || strings.HasPrefix(cand, strings.TrimSuffix(m.Point, "/")+"/")) {
					f.GuardHit = g
					break
				}
			}
			if f.GuardHit != "" {
				break
			}
		}
		if f.GuardHit != "" {
			break
		}
	}
	return f, nil
}
