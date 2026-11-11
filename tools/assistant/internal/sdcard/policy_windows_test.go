package sdcard

import (
	"io"
	"strings"
	"testing"
)

// winCard is the Facts of a healthy 32 GiB USB stick as the Windows inventory reports them, before the
// system view: it is built through the same functions the inventory uses.
func winCard(t *testing.T, mutate func(*WinDescriptor, *WinGeometry, *WinLayout, *bool)) Facts {
	t.Helper()
	d := WinDescriptor{Removable: true, BusType: busUsb, Vendor: "Generic", Product: "STORAGE DEVICE", Revision: "1.00", Serial: "W1"}
	g := WinGeometry{BytesPerSector: 512, DiskSize: 32 << 30}
	l, err := ParseDriveLayoutEx(layoutBuf(0, 0xA1B2C3D4, [16]byte{}, partSpec{mbrType: 0x0C}))
	if err != nil {
		t.Fatal(err)
	}
	ro := false
	if mutate != nil {
		mutate(&d, &g, &l, &ro)
	}
	f, err := BuildWinFacts(2, d, g, 0, l, ro)
	if err != nil {
		t.Fatal(err)
	}
	ApplyWinView(&f, []Mount{{Node: `\\?\Volume{B}\`, Point: `E:\`, FS: "FAT32"}}, nil, "")
	return f
}

// The Evaluate matrix for Windows (spec 11, T11.7).
func TestEvaluateWindowsMatrix(t *testing.T) {
	type tc struct {
		name   string
		mod    func(*WinDescriptor, *WinGeometry, *WinLayout, *bool)
		view   func(*Facts)
		refuse string
	}
	sys := func(r Role) func(*Facts) {
		return func(f *Facts) { f.Roles = append(f.Roles, r) }
	}
	bus := func(b uint32) func(*WinDescriptor, *WinGeometry, *WinLayout, *bool) {
		return func(d *WinDescriptor, _ *WinGeometry, _ *WinLayout, _ *bool) { d.BusType = b }
	}
	for _, c := range []tc{
		{name: "usb stick accepted"},
		{name: "usb stick that reports RemovableMedia false is still accepted", mod: func(d *WinDescriptor, _ *WinGeometry, _ *WinLayout, _ *bool) { d.Removable = false }},
		{name: "sd bus accepted", mod: bus(busSd)},
		{name: "mmc bus accepted", mod: bus(busMmc)},
		{name: "128 GiB accepted", mod: func(_ *WinDescriptor, g *WinGeometry, _ *WinLayout, _ *bool) { g.DiskSize = 128 << 30 }},
		{name: "system disk", view: sys(RoleSystem), refuse: "system role"},
		{name: "boot disk", view: sys(RoleBoot), refuse: "system role"},
		{name: "page file disk", view: sys(RolePagefile), refuse: "system role"},
		{name: "crash dump disk", view: sys(RoleCrashdump), refuse: "system role"},
		{name: "nvme", mod: bus(busNvme), refuse: "not a USB or SD disk"},
		{name: "sata", mod: bus(busSata), refuse: "not a USB or SD disk"},
		{name: "ata", mod: bus(busAta), refuse: "not a USB or SD disk"},
		{name: "scsi", mod: bus(busScsi), refuse: "not a USB or SD disk"},
		{name: "raid", mod: bus(busRaid), refuse: "not a USB or SD disk"},
		{name: "iscsi", mod: bus(busIscsi), refuse: "not a USB or SD disk"},
		{name: "unknown bus", mod: bus(busUnknown), refuse: "not a USB or SD disk"},
		{name: "unlisted bus code", mod: bus(0x77), refuse: "not a USB or SD disk"},
		{name: "file-backed virtual disk", mod: bus(busFileVirt), refuse: "virtual"},
		{name: "storage spaces bus", mod: bus(busSpaces), refuse: "virtual"},
		{name: "usb over 128 GiB", mod: func(_ *WinDescriptor, g *WinGeometry, _ *WinLayout, _ *bool) { g.DiskSize = 128<<30 + 512 }, refuse: "outside 128 MiB"},
		{name: "usb under 128 MiB", mod: func(_ *WinDescriptor, g *WinGeometry, _ *WinLayout, _ *bool) { g.DiskSize = 64 << 20 }, refuse: "outside 128 MiB"},
		{name: "no medium", mod: func(_ *WinDescriptor, g *WinGeometry, _ *WinLayout, _ *bool) { g.DiskSize = 0 }, refuse: "no medium"},
		{name: "dynamic disk (MBR LDM)", mod: func(_ *WinDescriptor, _ *WinGeometry, l *WinLayout, _ *bool) {
			*l, _ = ParseDriveLayoutEx(layoutBuf(0, 1, [16]byte{}, partSpec{mbrType: 0x42}))
		}, refuse: "dynamic disk"},
		{name: "dynamic disk (GPT LDM)", mod: func(_ *WinDescriptor, _ *WinGeometry, l *WinLayout, _ *bool) {
			*l, _ = ParseDriveLayoutEx(layoutBuf(1, 0, [16]byte{9}, partSpec{isGPT: true, gpt: gptGUID(gptLDMData)}))
		}, refuse: "dynamic disk"},
		{name: "storage spaces member", mod: func(_ *WinDescriptor, _ *WinGeometry, l *WinLayout, _ *bool) {
			*l, _ = ParseDriveLayoutEx(layoutBuf(1, 0, [16]byte{9}, partSpec{isGPT: true, gpt: gptGUID(gptStorageSpc)}))
		}, refuse: "Storage Spaces"},
		{name: "card with an EFI system partition", mod: func(_ *WinDescriptor, _ *WinGeometry, l *WinLayout, _ *bool) {
			*l, _ = ParseDriveLayoutEx(layoutBuf(1, 0, [16]byte{9}, partSpec{isGPT: true, gpt: gptGUID(gptESP)}))
		}, refuse: "system role"},
		{name: "raw (blank) card accepted", mod: func(_ *WinDescriptor, _ *WinGeometry, l *WinLayout, _ *bool) {
			*l, _ = ParseDriveLayoutEx(layoutBuf(2, 0, [16]byte{}))
		}},
		{name: "read-only", mod: func(_ *WinDescriptor, _ *WinGeometry, _ *WinLayout, ro *bool) { *ro = true }, refuse: "write-protect"},
		{name: "4096-byte sectors", mod: func(_ *WinDescriptor, g *WinGeometry, _ *WinLayout, _ *bool) { g.BytesPerSector = 4096 }, refuse: "4096-byte"},
		{name: "disk holding a guard path", view: func(f *Facts) { f.GuardHit = `C:\Users\me\akam` }, refuse: `carries C:\Users\me\akam`},
	} {
		f := winCard(t, c.mod)
		if c.view != nil {
			c.view(&f)
		}
		cand, err := Evaluate(f, DefaultPolicy)
		switch {
		case c.refuse == "" && err != nil:
			t.Errorf("%s: refused: %v", c.name, err)
		case c.refuse != "" && (err == nil || !strings.Contains(err.Error(), c.refuse)):
			t.Errorf("%s: want refusal %q, got %v", c.name, c.refuse, err)
		case err == nil && (cand.Dev != `\\.\PhysicalDrive2` || cand.Model != "STORAGE DEVICE" || cand.Size == 0 || len(cand.Mounts) != 1 || cand.Mounts[0].Point != `E:\`):
			t.Errorf("%s: the red box facts are missing: %+v", c.name, cand)
		}
	}
}

// A drive letter or a volume path given instead of a physical drive is refused, whatever else is true of it.
func TestEvaluateWindowsRefusesWhatIsNotAPhysicalDrive(t *testing.T) {
	for _, p := range []string{"E:", `E:\`, `\\.\E:`, `\\?\Volume{11111111-2222-3333-4444-555555555555}`, `\\.\PhysicalDrive2\`, `\\.\HarddiskVolume3`} {
		f := winCard(t, nil)
		_, ok := WinPhysicalNode(p)
		f.Path, f.Name, f.Whole = p, p, ok
		if _, err := Evaluate(f, DefaultPolicy); err == nil || !strings.Contains(err.Error(), "not a whole") {
			t.Errorf("%q: %v", p, err)
		}
	}
}

// Every fact the inventory cannot read is an unknown, and an unknown refuses.
func TestEvaluateWindowsFailsClosedOnUnknownFacts(t *testing.T) {
	f := winCard(t, nil)
	f.Internal = Unknown
	if _, err := Evaluate(f, DefaultPolicy); err == nil {
		t.Error("an unknown Internal flag was accepted")
	}
	f = winCard(t, func(d *WinDescriptor, _ *WinGeometry, _ *WinLayout, _ *bool) { d.BusType = 0x123 })
	if _, err := Evaluate(f, DefaultPolicy); err == nil {
		t.Error("an unknown bus was accepted")
	}
	// the facts of the identity token are all there
	f = winCard(t, nil)
	if id := Ident(f); !strings.HasPrefix(id, "windows|34359738368|STORAGE DEVICE|Generic|W1||1.00|A1B2C3D4|2") {
		t.Errorf("ident %q", id)
	}
}

// The helper's order on Windows facts: inspect, compare identity, lock and dismount every volume path, inspect
// again, open (which compares the identity of the opened handle), write, flush, rescan (update properties and
// the uncached verify handle), read, release. The recorder stands for the Opener; the order inside the real
// disk is asserted by TestWinDiskOrderFlushUpdateVerifyRelease.
func TestHelperOrderOnWindowsFacts(t *testing.T) {
	facts := winCard(t, nil)
	facts.Size = 128 << 20 // a wipe of 128 MiB: one final sync
	facts.Mounts = []Mount{
		{Node: `\\?\Volume{B}\`, Point: `E:\`, FS: "FAT32"},
		{Node: `\\?\Volume{B}\`, Point: `C:\mnt\card\`, FS: "FAT32"},
		{Node: `\\?\Volume{C}\`, Point: `\\?\Volume{C}\`},
	}
	r := &recorder{facts: facts}
	tok := Ident(facts)
	resp := Helper{Inv: r, Opener: r, Progress: io.Discard}.Handle(Request{Op: OpWipe, Device: facts.Path, Token: tok})
	if !resp.OK {
		t.Fatal(resp.Error)
	}
	got := strings.Join(r.log, ",")
	want := `inspect,checknode,unmount E:\,unmount C:\mnt\card\,unmount \\?\Volume{C}\,inspect,open ` + tok + ",write,sync,rescan,read,close"
	if got != want {
		t.Errorf("order:\n got %s\nwant %s", got, want)
	}
}

// A different disk behind the same node after the volumes were dismounted is never opened.
func TestHelperRefusesChangedWindowsDisk(t *testing.T) {
	facts := winCard(t, nil)
	other := winCard(t, func(d *WinDescriptor, _ *WinGeometry, _ *WinLayout, _ *bool) { d.Serial = "OTHER" })
	r := &swapRec{recorder: recorder{facts: facts}, other: other}
	resp := Helper{Inv: r, Opener: r}.Handle(Request{Op: OpWipe, Device: facts.Path, Token: Ident(facts)})
	if resp.OK || !strings.Contains(resp.Error, "nothing was written") {
		t.Fatalf("got %+v", resp)
	}
	for _, s := range r.log {
		if strings.HasPrefix(s, "open") || s == "write" {
			t.Errorf("reached %s", s)
		}
	}
}
