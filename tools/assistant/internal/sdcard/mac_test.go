package sdcard

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"unsafe"
)

// ---- plist parser ----

func readTestdata(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", "diskutil", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestPlistParsesDiskutilList(t *testing.T) {
	d, err := ParsePlist(readTestdata(t, "list.xml"))
	if err != nil {
		t.Fatal(err)
	}
	whole, ok := d.Arr("WholeDisks")
	if !ok || len(whole) != 5 || whole[0] != "disk0" {
		t.Fatalf("WholeDisks = %v", whole)
	}
	entries, _ := d.Arr("AllDisksAndPartitions")
	if len(entries) != 5 {
		t.Fatalf("entries = %d", len(entries))
	}
	// nested APFS store and volumes
	c := entries[1].(Dict)
	if got := storeDisks(c); len(got) != 1 || got[0] != "disk0" {
		t.Errorf("stores = %v", got)
	}
	vols, _ := c.Arr("APFSVolumes")
	if mp, _ := vols[2].(Dict).Str("MountPoint"); mp != "/" || len(vols) != 3 {
		t.Errorf("volumes: %v", vols)
	}
	if n, ok := entries[0].(Dict).Int("Size"); !ok || n != 500277792768 {
		t.Errorf("integer: %v", n)
	}
	info, err := ParsePlist(readTestdata(t, "info_disk4.xml"))
	if err != nil {
		t.Fatal(err)
	}
	if b, ok := info.Bool("WholeDisk"); !ok || !b {
		t.Error("boolean true")
	}
	if b, ok := info.Bool("Internal"); !ok || b {
		t.Error("boolean false")
	}
	if s, _ := info.Str("BusProtocol"); s != "USB" {
		t.Errorf("string %q", s)
	}
}

func TestPlistRefusesBadInput(t *testing.T) {
	const hdr = `<?xml version="1.0"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">`
	nest := func(n int) string {
		return hdr + `<plist><dict><key>a</key>` + strings.Repeat("<array>", n) + strings.Repeat("</array>", n) + `</dict></plist>`
	}
	cases := map[string]string{
		"empty":            "",
		"not xml":          "hello",
		"truncated":        hdr + `<plist><dict><key>a</key><string>x</string>`,
		"wrong root":       hdr + `<foo/>`,
		"root array":       hdr + `<plist><array/></plist>`,
		"key without val":  hdr + `<plist><dict><key>a</key></dict></plist>`,
		"value w/o key":    hdr + `<plist><dict><string>a</string></dict></plist>`,
		"bad integer":      hdr + `<plist><dict><key>a</key><integer>12x</integer></dict></plist>`,
		"huge integer":     hdr + `<plist><dict><key>a</key><integer>99999999999999999999</integer></dict></plist>`,
		"unknown element":  hdr + `<plist><dict><key>a</key><script/></dict></plist>`,
		"duplicate key":    hdr + `<plist><dict><key>a</key><true/><key>a</key><false/></dict></plist>`,
		"text after":       hdr + `<plist><dict/></plist>junk`,
		"second root":      hdr + `<plist><dict/></plist><plist><dict/></plist>`,
		"internal subset":  `<?xml version="1.0"?><!DOCTYPE plist [<!ENTITY x "boom">]><plist><dict><key>a</key><string>&x;</string></dict></plist>`,
		"external doctype": `<?xml version="1.0"?><!DOCTYPE plist SYSTEM "file:///etc/passwd"><plist><dict/></plist>`,
		"entity ref":       hdr + `<plist><dict><key>a</key><string>&xxe;</string></dict></plist>`,
		"too deep":         nest(plistMaxDepth + 2),
		"long string":      hdr + `<plist><dict><key>a</key><string>` + strings.Repeat("a", plistMaxString+1) + `</string></dict></plist>`,
		"oversized":        hdr + `<plist><dict><key>a</key><string>` + strings.Repeat("a", plistMaxBytes) + `</string></dict></plist>`,
		"too many nodes":   hdr + `<plist><dict><key>a</key><array>` + strings.Repeat("<true/>", plistMaxNodes+1) + `</array></dict></plist>`,
	}
	for name, in := range cases {
		if _, err := ParsePlist([]byte(in)); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// the nesting limit itself is not a false positive for a sane document
	if _, err := ParsePlist([]byte(nest(10))); err != nil {
		t.Errorf("depth 10: %v", err)
	}
	// the predefined entities are honoured, nothing else
	d, err := ParsePlist([]byte(hdr + `<plist><dict><key>a</key><string>x &amp; &lt;y&gt;</string></dict></plist>`))
	if err != nil {
		t.Fatal(err)
	}
	if s, _ := d.Str("a"); s != "x & <y>" {
		t.Errorf("got %q", s)
	}
}

// ---- inventory and Evaluate matrix ----

type fakeRunner struct {
	t     *testing.T
	files map[string]string // "info -plist /dev/disk4" -> testdata file name
	mut   map[string]func([]byte) []byte
	log   *[]string
	fail  map[string]error
}

func (r *fakeRunner) Run(ctx context.Context, name string, args ...string) ([]byte, error) {
	if name != diskutilPath {
		r.t.Errorf("unexpected program %s", name)
	}
	key := strings.Join(args, " ")
	if r.log != nil {
		*r.log = append(*r.log, "run "+key)
	}
	if err := r.fail[key]; err != nil {
		return nil, err
	}
	if strings.HasPrefix(key, "unmountDisk ") {
		return nil, nil
	}
	fn, ok := r.files[key]
	if !ok {
		return nil, fmt.Errorf("no canned answer for %q", key)
	}
	b := readTestdata(r.t, fn)
	if m := r.mut[key]; m != nil {
		b = m(b)
	}
	return b, nil
}

func newFakeRunner(t *testing.T) *fakeRunner {
	return &fakeRunner{t: t, mut: map[string]func([]byte) []byte{}, fail: map[string]error{}, files: map[string]string{
		"list -plist":              "list.xml",
		"info -plist /":            "info_root.xml",
		"info -plist /dev/disk0":   "info_disk0.xml",
		"info -plist /dev/disk3":   "info_disk3.xml",
		"info -plist /dev/disk4":   "info_disk4.xml",
		"info -plist /dev/disk5":   "info_disk5.xml",
		"info -plist /dev/disk6":   "info_disk6.xml",
		"info -plist /dev/disk4s1": "info_disk4.xml",
	}}
}

func (r *fakeRunner) inv() *macInventory {
	return &macInventory{run: r, devNum: func(string) (string, error) { return "14:4", nil }, resolve: func(p string) string { return p }}
}

func replaceIn(old, new string) func([]byte) []byte {
	return func(b []byte) []byte {
		if !strings.Contains(string(b), old) {
			panic("pattern not in canned file: " + old)
		}
		return []byte(strings.ReplaceAll(string(b), old, new))
	}
}

func evalMac(t *testing.T, r *fakeRunner, dev string, guards ...string) (Facts, *Candidate, error) {
	t.Helper()
	f, err := r.inv().Inspect(dev, Context{GuardPaths: guards})
	if err != nil {
		return f, nil, err
	}
	c, err := Evaluate(f, DefaultPolicy)
	return f, c, err
}

func TestMacDevicesListsWholeDisks(t *testing.T) {
	got := strings.Join(newFakeRunner(t).inv().Devices(), ",")
	if got != "/dev/disk0,/dev/disk3,/dev/disk4,/dev/disk5,/dev/disk6" {
		t.Errorf("Devices = %s", got)
	}
}

func TestMacEvaluateMatrix(t *testing.T) {
	type tc struct {
		name   string
		dev    string
		setup  func(r *fakeRunner)
		guards []string
		refuse string // "" = accepted; "ERR" = Inspect itself fails
	}
	rootIsDisk4 := func(r *fakeRunner) {
		// the volume behind "/" says its parent disk is disk4: only the ParentWholeDisk check can catch it
		r.mut["info -plist /"] = replaceIn("<string>disk3</string>", "<string>disk4</string>")
		r.mut["list -plist"] = func(b []byte) []byte { return []byte(strings.ReplaceAll(string(b), "/Volumes/AKAM", "/Volumes/Other")) }
	}
	cases := []tc{
		{name: "USB card reader accepted", dev: "/dev/disk4"},
		{name: "built-in SD slot reporting Internal accepted on protocol", dev: "/dev/disk5"},
		{name: "internal NVMe / Apple Fabric", dev: "/dev/disk0", refuse: "system path"},
		{name: "synthesized APFS container", dev: "/dev/disk3", refuse: "disk"},
		{name: "disk image (SystemImage)", dev: "/dev/disk6", refuse: "virtual"},
		{name: "slice diskNsM", dev: "/dev/disk4s1", refuse: "not a whole"},
		{name: "raw node as input", dev: "/dev/rdisk4", refuse: "ERR"},
		{name: "path trick", dev: "/dev/disk4/../disk0", refuse: "ERR"},
		{name: "4096-byte block", dev: "/dev/disk4", refuse: "4096-byte", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>DeviceBlockSize</key>\n\t<integer>512</integer>", "<key>DeviceBlockSize</key>\n\t<integer>4096</integer>")
		}},
		{name: "sector size not reported", dev: "/dev/disk4", refuse: "sector size", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>DeviceBlockSize</key>", "<key>Unrelated</key>")
		}},
		{name: "disk behind / (ParentWholeDisk check only)", dev: "/dev/disk4", refuse: "system role", setup: rootIsDisk4},
		{name: "disk behind / (mount scan only)", dev: "/dev/disk4", refuse: "system path", setup: func(r *fakeRunner) {
			r.mut["list -plist"] = replaceIn("/Volumes/AKAM", "/")
		}},
		{name: "mount under /Users", dev: "/dev/disk4", refuse: "system path", setup: func(r *fakeRunner) {
			r.mut["list -plist"] = replaceIn("/Volumes/AKAM", "/Users/x/card")
		}},
		{name: "APFS container with a system volume on the card", dev: "/dev/disk4", refuse: "system", setup: func(r *fakeRunner) {
			// the system container's physical store is a slice of disk4
			r.mut["list -plist"] = replaceIn("<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk0s2</string>\n\t\t\t\t</dict>\n\t\t\t</array>", "<key>DeviceIdentifier</key>\n\t\t\t\t\t<string>disk4s1</string>\n\t\t\t\t</dict>\n\t\t\t</array>")
		}},
		{name: "guard path on the card", dev: "/dev/disk4", refuse: "repository", guards: []string{"/Volumes/AKAM/work"}},
		{name: "guard path elsewhere", dev: "/dev/disk4", guards: []string{"/Users/x/work"}},
		{name: "Internal not reported", dev: "/dev/disk4", refuse: "cannot tell", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>Internal</key>", "<key>Other</key>")
		}},
		{name: "bus protocol not reported", dev: "/dev/disk4", refuse: "bus", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>BusProtocol</key>", "<key>Other</key>")
		}},
		{name: "SATA", dev: "/dev/disk4", refuse: "bus", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<string>USB</string>", "<string>SATA</string>")
		}},
		{name: "Thunderbolt", dev: "/dev/disk4", refuse: "bus", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<string>USB</string>", "<string>Thunderbolt</string>")
		}},
		{name: "virtual kind not reported", dev: "/dev/disk4", refuse: "virtual", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>VirtualOrPhysical</key>", "<key>Other</key>")
		}},
		{name: "write-protected", dev: "/dev/disk4", refuse: "write-protect", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>WritableMedia</key>\n\t<true/>", "<key>WritableMedia</key>\n\t<false/>")
		}},
		{name: "size missing", dev: "/dev/disk4", refuse: "no medium", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>TotalSize</key>", "<key>Other</key>")
		}},
		{name: "WholeDisk missing", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = replaceIn("<key>WholeDisk</key>", "<key>Other</key>")
		}},
		{name: "info of / unreadable", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.fail["info -plist /"] = errors.New("boom")
		}},
		{name: "ParentWholeDisk of / missing", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.mut["info -plist /"] = replaceIn("<key>ParentWholeDisk</key>", "<key>Other</key>")
		}},
		{name: "list unreadable", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.fail["list -plist"] = errors.New("boom")
		}},
		{name: "diskutil answers about another disk", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.files["info -plist /dev/disk4"] = "info_disk5.xml"
		}},
		{name: "disk missing from the list", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.mut["list -plist"] = replaceIn("<string>disk4</string>\n\t\t\t<key>Size</key>", "<string>disk44</string>\n\t\t\t<key>Size</key>")
		}},
		{name: "malformed XML", dev: "/dev/disk4", refuse: "ERR", setup: func(r *fakeRunner) {
			r.mut["info -plist /dev/disk4"] = func(b []byte) []byte { return b[:len(b)/2] }
		}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r := newFakeRunner(t)
			if c.setup != nil {
				c.setup(r)
			}
			f, cand, err := evalMac(t, r, c.dev, c.guards...)
			switch {
			case c.refuse == "":
				if err != nil {
					t.Fatalf("refused: %v (facts %+v)", err, f)
				}
				if cand.Dev != c.dev || !strings.Contains(cand.Ident, "14:4") {
					t.Errorf("candidate %+v", cand)
				}
			case err == nil:
				t.Fatalf("accepted (facts %+v)", f)
			case c.refuse != "ERR" && !strings.Contains(strings.ToLower(err.Error()), strings.ToLower(c.refuse)):
				t.Errorf("refusal %q lacks %q", err, c.refuse)
			}
		})
	}
}

// Evaluate on hand-written facts: each rule alone, so no rule hides behind another.
func TestMacEvaluateRulesAlone(t *testing.T) {
	base := Facts{OS: "darwin", Path: "/dev/disk4", Whole: true, Size: 32 << 30, SectorSize: 512, Bus: "usb", Internal: No, Removable: Yes}
	if _, err := Evaluate(base, DefaultPolicy); err != nil {
		t.Fatal(err)
	}
	with := func(mod func(f *Facts)) Facts { f := base; mod(&f); return f }
	for name, f := range map[string]Facts{
		"mount at /":             with(func(f *Facts) { f.Mounts = []Mount{{Node: "/dev/disk4s1", Point: "/"}} }),
		"mount at /System/Vol":   with(func(f *Facts) { f.Mounts = []Mount{{Point: "/System/Volumes/Data"}} }),
		"mount at /private/var":  with(func(f *Facts) { f.Mounts = []Mount{{Point: "/private/var/db"}} }),
		"mount at /Library":      with(func(f *Facts) { f.Mounts = []Mount{{Point: "/Library"}} }),
		"role system":            with(func(f *Facts) { f.Roles = []Role{RoleSystem} }),
		"APFS stack":             with(func(f *Facts) { f.InStack = "APFS container with a system volume" }),
		"internal NVMe":          with(func(f *Facts) { f.Bus, f.Internal = "nvme", Yes }),
		"internal usb":           with(func(f *Facts) { f.Internal = Yes }),
		"nvme external":          with(func(f *Facts) { f.Bus = "nvme" }),
		"internal unknown on sd": with(func(f *Facts) { f.Bus, f.Internal = "sd", Unknown }),
		"disk image":             with(func(f *Facts) { f.Virtual = true }),
		"slice":                  with(func(f *Facts) { f.Whole = false; f.Path = "/dev/disk4s1" }),
		"slice path":             with(func(f *Facts) { f.Path = "/dev/disk4s1" }),
		"raw path":               with(func(f *Facts) { f.Path = "/dev/rdisk4" }),
		"4096":                   with(func(f *Facts) { f.SectorSize = 4096 }),
		"sector unknown":         with(func(f *Facts) { f.SectorSize = 0 }),
	} {
		if _, err := Evaluate(f, DefaultPolicy); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// the built-in slot: Internal yes, protocol Secure Digital
	if _, err := Evaluate(with(func(f *Facts) { f.Bus, f.Internal = "sd", Yes }), DefaultPolicy); err != nil {
		t.Errorf("built-in SD slot refused: %v", err)
	}
	// a Linux disk that says Internal is still not rescued by the macOS exception
	l := Facts{OS: "linux", Path: "/dev/sdb", Whole: true, Size: 32 << 30, Bus: "usb", RemovableRaw: "1"}
	if _, err := Evaluate(l, DefaultPolicy); err != nil {
		t.Fatal(err)
	}
}

// ---- Opener order ----

type macEvents struct{ log []string }

func (e *macEvents) add(s string, a ...any) { e.log = append(e.log, fmt.Sprintf(s, a...)) }

// strictRaw is a raw node that returns EINVAL for anything not sector-aligned (offset, length, buffer address).
type strictRaw struct {
	t      *testing.T
	ev     *macEvents
	devnum string
	wrote  bool
	read   bool
}

func (r *strictRaw) check(p []byte, off int64) error {
	if off%512 != 0 || len(p)%512 != 0 || (len(p) > 0 && uintptr(unsafe.Pointer(&p[0]))%4096 != 0) {
		r.t.Errorf("unaligned raw access: off %d len %d", off, len(p))
		return syscall.EINVAL
	}
	return nil
}
func (r *strictRaw) ReadAt(p []byte, off int64) (int, error) {
	if err := r.check(p, off); err != nil {
		return 0, err
	}
	if !r.read {
		r.ev.add("read")
		r.read = true
	}
	for i := range p {
		p[i] = 0
	}
	return len(p), nil
}
func (r *strictRaw) WriteAt(p []byte, off int64) (int, error) {
	if err := r.check(p, off); err != nil {
		return 0, err
	}
	if !r.wrote {
		r.ev.add("write")
		r.wrote = true
	}
	return len(p), nil
}
func (r *strictRaw) Sync() error             { r.ev.add("fsync"); return nil }
func (r *strictRaw) Close() error            { r.ev.add("close"); return nil }
func (r *strictRaw) DevNum() (string, error) { return r.devnum, nil }

func macHelper(t *testing.T, size string, devnum string) (Helper, *macEvents, *fakeRunner) {
	ev := &macEvents{}
	r := newFakeRunner(t)
	r.log = &ev.log
	r.mut["info -plist /dev/disk4"] = replaceIn("32010928128", size)
	op := &macOpener{run: r, openRaw: func(path string) (macRawFile, error) {
		ev.add("open %s", path)
		return &strictRaw{t: t, ev: ev, devnum: devnum}, nil
	}}
	return Helper{Inv: r.inv(), Opener: op}, ev, r
}

func TestMacOpenerOrder(t *testing.T) {
	h, ev, _ := macHelper(t, "134217728", "14:4")
	tok := "" // taken from a first inspection, like the parent does
	f, err := h.Inv.Inspect("/dev/disk4", Context{})
	if err != nil {
		t.Fatal(err)
	}
	tok = Ident(f)
	ev.log = nil
	resp := h.Handle(Request{Op: OpWipe, Device: "/dev/disk4", Token: tok})
	if !resp.OK {
		t.Fatal(resp.Error)
	}
	var steps []string
	for _, l := range ev.log {
		switch {
		case l == "run info -plist /dev/disk4":
			steps = append(steps, "inspect")
		case strings.HasPrefix(l, "run unmountDisk"):
			steps = append(steps, l[4:])
		case strings.HasPrefix(l, "run "):
		default:
			steps = append(steps, l)
		}
	}
	want := "inspect,unmountDisk force /dev/disk4,inspect,open /dev/rdisk4,write,fsync,read,close"
	if got := strings.Join(steps, ","); got != want {
		t.Errorf("order:\n got %s\nwant %s", got, want)
	}
}

func TestMacOpenerRefusesAnotherDeviceNumber(t *testing.T) {
	h, ev, _ := macHelper(t, "134217728", "14:5") // the raw node is not the disk that was checked
	f, _ := h.Inv.Inspect("/dev/disk4", Context{})
	resp := h.Handle(Request{Op: OpWipe, Device: "/dev/disk4", Token: Ident(f)})
	if resp.OK || !strings.Contains(resp.Error, "nothing was written") {
		t.Fatalf("%+v", resp)
	}
	for _, l := range ev.log {
		if l == "write" {
			t.Fatal("wrote")
		}
	}
}

func TestMacOpenerRefusesWhenTheCardChangesAfterUnmount(t *testing.T) {
	h, ev, r := macHelper(t, "134217728", "14:4")
	f, _ := h.Inv.Inspect("/dev/disk4", Context{})
	n := 0
	orig := r.mut["info -plist /dev/disk4"]
	r.mut["info -plist /dev/disk4"] = func(b []byte) []byte {
		b = orig(b)
		if n++; n >= 1 { // every inspection from now on: another card in the slot
			return replaceIn("Generic Card Reader Media", "Another Card")(b)
		}
		return b
	}
	resp := h.Handle(Request{Op: OpWipe, Device: "/dev/disk4", Token: Ident(f)})
	if resp.OK {
		t.Fatal("accepted")
	}
	for _, l := range ev.log {
		if strings.HasPrefix(l, "open") || l == "write" {
			t.Fatalf("reached %s", l)
		}
	}
}

func TestMacOpenerRefusesBadNodes(t *testing.T) {
	o := &macOpener{run: newFakeRunner(t), openRaw: func(string) (macRawFile, error) { t.Fatal("opened"); return nil, nil }}
	for _, dev := range []string{"/dev/rdisk4", "/dev/disk4s1", "/dev/sda", "disk4", "/dev/disk4/x", ""} {
		if err := o.CheckNode(dev); err == nil {
			t.Errorf("CheckNode(%q) accepted", dev)
		}
	}
	if err := o.CheckNode("/dev/disk4"); err != nil {
		t.Fatal(err)
	}
	if _, err := o.Open("/dev/disk9", &Candidate{Dev: "/dev/disk9"}); err == nil {
		t.Error("opened a node that was not checked")
	}
	if rawPath("/dev/disk12") != "/dev/rdisk12" {
		t.Error("rawPath")
	}
}
