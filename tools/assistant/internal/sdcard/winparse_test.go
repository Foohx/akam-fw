package sdcard

import (
	"encoding/binary"
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/blockio"
)

// The buffers below are built from the documented layouts of the Windows structures, not captured: the
// hardware plan (docs/hardware-test-checklists.md) checks them against a real machine.

func TestControlCodes(t *testing.T) {
	for name, c := range map[string][2]uint32{
		"storage query":    {ctlCode(0x2D, 0x500, 0, 0), ioctlStorageQueryProperty},
		"geometry ex":      {ctlCode(7, 0x28, 0, 0), ioctlDiskGetDriveGeometryEx},
		"layout ex":        {ctlCode(7, 0x14, 0, 0), ioctlDiskGetDriveLayoutEx},
		"length info":      {ctlCode(7, 0x17, 0, 1), ioctlDiskGetLengthInfo},
		"is writable":      {ctlCode(7, 0x09, 0, 0), ioctlDiskIsWritable},
		"update props":     {ctlCode(7, 0x50, 0, 0), ioctlDiskUpdateProperties},
		"volume extents":   {ctlCode(0x56, 0, 0, 0), ioctlVolumeGetVolumeDiskExtents},
		"lock":             {ctlCode(9, 6, 0, 0), fsctlLockVolume},
		"unlock":           {ctlCode(9, 7, 0, 0), fsctlUnlockVolume},
		"dismount":         {ctlCode(9, 8, 0, 0), fsctlDismountVolume},
		"extended dasd io": {ctlCode(9, 32, 3, 0), fsctlAllowExtendedDasdIo},
	} {
		if c[0] != c[1] {
			t.Errorf("%s: computed %#x, constant %#x", name, c[0], c[1])
		}
	}
}

// descBuf builds a STORAGE_DEVICE_DESCRIPTOR with the four strings after the 40-byte header.
func descBuf(bus uint32, removable bool, vendor, product, rev, serial string) []byte {
	b := make([]byte, 40)
	binary.LittleEndian.PutUint32(b[0:], 1)
	if removable {
		b[10] = 1
	}
	binary.LittleEndian.PutUint32(b[28:], bus)
	for i, s := range []string{vendor, product, rev, serial} {
		if s == "" {
			continue
		}
		binary.LittleEndian.PutUint32(b[12+4*i:], uint32(len(b)))
		b = append(b, s...)
		b = append(b, 0)
	}
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)))
	return b
}

func TestParseDeviceDescriptorEveryBus(t *testing.T) {
	for _, c := range []struct {
		bus      uint32
		name     string
		virtual  bool
		internal Tri
	}{
		{busUsb, "usb", false, No}, {busSd, "sd", false, No}, {busMmc, "mmc", false, No},
		{busSata, "sata", false, Yes}, {busAta, "sata", false, Yes}, {busNvme, "nvme", false, Yes},
		{busScsi, "scsi", false, Yes}, {busSas, "scsi", false, Yes}, {busRaid, "raid", false, Yes},
		{busVirtual, "virtual", true, Yes}, {busFileVirt, "virtual", true, Yes}, {busSpaces, "virtual", true, Yes},
		{busIscsi, "other", false, Unknown}, {busUnknown, "unknown", false, Unknown}, {0x99, "unknown", false, Unknown},
	} {
		d, err := ParseDeviceDescriptor(descBuf(c.bus, true, "  Generic ", "STORAGE DEVICE", "1.00", "0123456789AB"))
		if err != nil {
			t.Fatal(err)
		}
		name, virtual, internal := BusFacts(d.BusType)
		if name != c.name || virtual != c.virtual || internal != c.internal {
			t.Errorf("bus %#x: %s %v %v, want %s %v %v", c.bus, name, virtual, internal, c.name, c.virtual, c.internal)
		}
		if d.Vendor != "Generic" || d.Product != "STORAGE DEVICE" || d.Revision != "1.00" || d.Serial != "0123456789AB" || !d.Removable {
			t.Errorf("strings: %+v", d)
		}
	}
	d, err := ParseDeviceDescriptor(descBuf(busUsb, false, "", "Card", "", ""))
	if err != nil || d.Vendor != "" || d.Product != "Card" || d.Removable {
		t.Errorf("absent strings: %+v %v", d, err)
	}
}

func TestParseDeviceDescriptorRefusesBadBuffers(t *testing.T) {
	good := descBuf(busUsb, true, "V", "P", "R", "S")
	if _, err := ParseDeviceDescriptor(good[:35]); err == nil {
		t.Error("short header accepted")
	}
	for off := 12; off <= 24; off += 4 {
		b := append([]byte(nil), good...)
		binary.LittleEndian.PutUint32(b[off:], uint32(len(b))) // one past the end
		if _, err := ParseDeviceDescriptor(b); err == nil {
			t.Errorf("offset past the end at %d accepted", off)
		}
		b = append([]byte(nil), good...)
		binary.LittleEndian.PutUint32(b[off:], 8) // inside the header
		if _, err := ParseDeviceDescriptor(b); err == nil {
			t.Errorf("offset inside the header at %d accepted", off)
		}
	}
	if _, err := ParseDeviceDescriptor(good[:len(good)-1]); err == nil { // the last NUL is cut
		t.Error("unterminated string accepted")
	}
}

func geoBuf(ss uint32, size int64) []byte {
	b := make([]byte, 40)
	binary.LittleEndian.PutUint32(b[20:], ss)
	binary.LittleEndian.PutUint64(b[24:], uint64(size))
	return b
}

func TestParseGeometryAndLength(t *testing.T) {
	g, err := ParseGeometryEx(geoBuf(512, 32<<30))
	if err != nil || g.BytesPerSector != 512 || g.DiskSize != 32<<30 {
		t.Fatalf("%+v %v", g, err)
	}
	if _, err := ParseGeometryEx(geoBuf(512, 1)[:31]); err == nil {
		t.Error("short geometry accepted")
	}
	if _, err := ParseGeometryEx(geoBuf(512, -1)); err == nil {
		t.Error("negative size accepted")
	}
	l := make([]byte, 8)
	binary.LittleEndian.PutUint64(l, 1<<30)
	if n, err := ParseLengthInfo(l); err != nil || n != 1<<30 {
		t.Errorf("%d %v", n, err)
	}
	if _, err := ParseLengthInfo(l[:7]); err == nil {
		t.Error("short length accepted")
	}
}

type partSpec struct {
	mbrType byte
	gpt     [16]byte // as written in memory (mixed endian)
	isGPT   bool
}

func gptGUID(s string) [16]byte { // "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" to the in-memory form
	h := strings.ReplaceAll(s, "-", "")
	var raw [16]byte
	for i := range raw {
		raw[i] = hexByte(h[2*i : 2*i+2])
	}
	var g [16]byte
	g[0], g[1], g[2], g[3] = raw[3], raw[2], raw[1], raw[0]
	g[4], g[5] = raw[5], raw[4]
	g[6], g[7] = raw[7], raw[6]
	copy(g[8:], raw[8:])
	return g
}

func hexByte(s string) byte {
	var v byte
	for _, c := range []byte(s) {
		v <<= 4
		switch {
		case c >= '0' && c <= '9':
			v |= c - '0'
		default:
			v |= c - 'A' + 10
		}
	}
	return v
}

func layoutBuf(style uint32, mbrSig uint32, diskGUID [16]byte, parts ...partSpec) []byte {
	b := make([]byte, 48+144*len(parts))
	binary.LittleEndian.PutUint32(b[0:], style)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(parts)))
	switch style {
	case 0:
		binary.LittleEndian.PutUint32(b[8:], mbrSig)
	case 1:
		copy(b[8:], diskGUID[:])
	}
	for i, p := range parts {
		e := b[48+144*i:]
		if p.isGPT {
			binary.LittleEndian.PutUint32(e[0:], 1)
			copy(e[32:], p.gpt[:])
		} else {
			binary.LittleEndian.PutUint32(e[0:], 0)
			e[32] = p.mbrType
		}
		binary.LittleEndian.PutUint64(e[8:], uint64(1048576*(i+1)))
		binary.LittleEndian.PutUint64(e[16:], 1048576)
		binary.LittleEndian.PutUint32(e[24:], uint32(i+1))
	}
	return b
}

func TestParseDriveLayoutExMBRGPTRaw(t *testing.T) {
	l, err := ParseDriveLayoutEx(layoutBuf(0, 0xA1B2C3D4, [16]byte{}, partSpec{mbrType: 0x0C}, partSpec{}, partSpec{mbrType: 0x83}))
	if err != nil || l.Style != 0 || l.ID != "A1B2C3D4" || len(l.Partitions) != 2 || l.Partitions[0].MBRType != 0x0C || l.Partitions[1].Number != 3 {
		t.Fatalf("mbr: %+v %v", l, err)
	}
	dyn, stack, roles := l.Classify()
	if dyn || stack != "" || len(roles) != 0 {
		t.Errorf("plain MBR classified as %v %q %v", dyn, stack, roles)
	}
	diskID := gptGUID("11223344-5566-7788-99AA-BBCCDDEEFF00")
	l, err = ParseDriveLayoutEx(layoutBuf(1, 0, diskID, partSpec{isGPT: true, gpt: gptGUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")}))
	if err != nil || l.ID != "11223344-5566-7788-99AA-BBCCDDEEFF00" || l.Partitions[0].GPTType != "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" {
		t.Fatalf("gpt: %+v %v", l, err)
	}
	l, err = ParseDriveLayoutEx(layoutBuf(2, 0, [16]byte{}))
	if err != nil || l.ID != "" || len(l.Partitions) != 0 || l.Style != 2 {
		t.Fatalf("raw: %+v %v", l, err)
	}
}

func TestLayoutClassifyLDMStorageSpacesAndRoles(t *testing.T) {
	g := func(s string) partSpec { return partSpec{isGPT: true, gpt: gptGUID(s)} }
	for _, c := range []struct {
		name    string
		buf     []byte
		dynamic bool
		stack   string
		roles   []Role
	}{
		{"mbr ldm", layoutBuf(0, 1, [16]byte{}, partSpec{mbrType: 0x42}), true, "", nil},
		{"gpt ldm metadata", layoutBuf(1, 0, [16]byte{1}, g(gptLDMMeta)), true, "", nil},
		{"gpt ldm data", layoutBuf(1, 0, [16]byte{1}, g(gptLDMData)), true, "", nil},
		{"storage spaces", layoutBuf(1, 0, [16]byte{1}, g(gptStorageSpc)), false, "Storage Spaces", nil},
		{"esp and recovery", layoutBuf(1, 0, [16]byte{1}, g(gptESP), g(gptRecovery)), false, "", []Role{RoleESP, RoleRecovery}},
		{"mbr esp", layoutBuf(0, 1, [16]byte{}, partSpec{mbrType: 0xEF}), false, "", []Role{RoleESP}},
	} {
		l, err := ParseDriveLayoutEx(c.buf)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		dyn, stack, roles := l.Classify()
		if dyn != c.dynamic || stack != c.stack || len(roles) != len(c.roles) {
			t.Errorf("%s: %v %q %v", c.name, dyn, stack, roles)
			continue
		}
		for i := range roles {
			if roles[i] != c.roles[i] {
				t.Errorf("%s: roles %v", c.name, roles)
			}
		}
	}
}

func TestParseDriveLayoutExRefusesBadBuffers(t *testing.T) {
	good := layoutBuf(0, 1, [16]byte{}, partSpec{mbrType: 0x0C}, partSpec{mbrType: 0x0C})
	if _, err := ParseDriveLayoutEx(good[:7]); err == nil {
		t.Error("short header accepted")
	}
	if _, err := ParseDriveLayoutEx(good[:len(good)-1]); err == nil {
		t.Error("truncated partition entry accepted")
	}
	b := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(b[4:], 0xFFFFFFFF)
	if _, err := ParseDriveLayoutEx(b); err == nil {
		t.Error("huge partition count accepted")
	}
	b = append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(b[0:], 7)
	if _, err := ParseDriveLayoutEx(b); err == nil {
		t.Error("unknown style accepted")
	}
	b = append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(b[48:], 9)
	if _, err := ParseDriveLayoutEx(b); err == nil {
		t.Error("unknown partition style accepted")
	}
	if _, err := ParseDriveLayoutEx(layoutBuf(1, 0, [16]byte{})[:20]); err == nil {
		t.Error("truncated GPT header accepted")
	}
}

func extentsBuf(exts ...[3]int64) []byte {
	b := make([]byte, 8+24*len(exts))
	binary.LittleEndian.PutUint32(b, uint32(len(exts)))
	for i, e := range exts {
		o := 8 + 24*i
		binary.LittleEndian.PutUint32(b[o:], uint32(e[0]))
		binary.LittleEndian.PutUint64(b[o+8:], uint64(e[1]))
		binary.LittleEndian.PutUint64(b[o+16:], uint64(e[2]))
	}
	return b
}

func TestParseDiskExtentsSpannedVolume(t *testing.T) {
	b := extentsBuf([3]int64{3, 1 << 20, 1 << 30}, [3]int64{4, 2 << 20, 2 << 30})
	ext, err := ParseDiskExtents(b)
	if err != nil || len(ext) != 2 || ext[0].Disk != 3 || ext[1].Disk != 4 || ext[1].Length != 2<<30 || ext[1].Start != 2<<20 {
		t.Fatalf("%+v %v", ext, err)
	}
	// a buffer too small for the extents still tells how many there are (the retry size)
	if n, err := DiskExtentCount(b[:8]); err != nil || n != 2 || DiskExtentsSize(n) != len(b) {
		t.Errorf("count %d %v", n, err)
	}
	if _, err := ParseDiskExtents(b[:8]); err == nil {
		t.Error("extents that do not fit accepted")
	}
	if _, err := ParseDiskExtents(b[:3]); err == nil {
		t.Error("short header accepted")
	}
	huge := make([]byte, 8)
	binary.LittleEndian.PutUint32(huge, 1<<30)
	if _, err := ParseDiskExtents(huge); err == nil {
		t.Error("absurd extent count accepted")
	}
}

func TestWritableFromError(t *testing.T) {
	if ro, err := WritableFromError(0); ro || err != nil {
		t.Error("success is writable")
	}
	if ro, err := WritableFromError(winErrWriteProtect); !ro || err != nil {
		t.Error("write protect is read-only")
	}
	for _, c := range []uint32{5, 21, 1117, 0xFFFFFFFF} {
		if _, err := WritableFromError(c); err == nil {
			t.Errorf("error %d must not read as writable", c)
		}
	}
}

func TestWinPhysicalNode(t *testing.T) {
	for p, want := range map[string]int{`\\.\PhysicalDrive0`: 0, `\\.\PhysicalDrive12`: 12, `\\.\PhysicalDrive999`: 999} {
		if n, ok := WinPhysicalNode(p); !ok || n != want {
			t.Errorf("%s: %d %v", p, n, ok)
		}
	}
	for _, p := range []string{"", "E:", `E:\`, `\\.\E:`, `\\.\PhysicalDrive`, `\\.\PhysicalDrive1234`, `\\.\PhysicalDrive1\x`, `\\.\physicaldrive1`,
		`\\?\Volume{11111111-2222-3333-4444-555555555555}`, `\\.\PhysicalDrive2 `, "/dev/sdb", `\\.\PhysicalDrive-1`, `\\.\HarddiskVolume3`} {
		if _, ok := WinPhysicalNode(p); ok {
			t.Errorf("%q accepted", p)
		}
	}
}

func TestParsePagingFiles(t *testing.T) {
	got, err := ParsePagingFiles([]string{`C:\pagefile.sys 0 0`, "", `d:\my files\pagefile.sys 1024 2048`, `?:\pagefile.sys`,
		`\??\E:\pagefile.sys`, `\\?\Volume{ABCDEF01-2222-3333-4444-555555555555}\pagefile.sys 0 0`})
	if err != nil || len(got) != 5 {
		t.Fatalf("%+v %v", got, err)
	}
	if got[0].Letter != "C:" || got[1].Letter != "D:" || !got[2].Auto || got[3].Letter != "E:" || got[4].Volume != `\\?\Volume{ABCDEF01-2222-3333-4444-555555555555}` {
		t.Errorf("%+v", got)
	}
	if got, err := ParsePagingFiles(nil); err != nil || got != nil {
		t.Errorf("empty: %v %v", got, err)
	}
	for _, bad := range []string{`pagefile.sys 0 0`, `\Device\HarddiskVolume3\pagefile.sys`, `\\server\share\pagefile.sys`} {
		if _, err := ParsePagingFiles([]string{bad}); err == nil {
			t.Errorf("%q accepted: the guard would be blind", bad)
		}
	}
}

func TestBuildWinFactsAndView(t *testing.T) {
	desc := WinDescriptor{Removable: true, BusType: busUsb, Vendor: "SanDisk", Product: "Ultra", Revision: "1.00", Serial: "AA11"}
	l, _ := ParseDriveLayoutEx(layoutBuf(0, 0xDEADBEEF, [16]byte{}, partSpec{mbrType: 0x0C}))
	f, err := BuildWinFacts(2, desc, WinGeometry{BytesPerSector: 512, DiskSize: 16 << 30}, 16<<30, l, false)
	if err != nil {
		t.Fatal(err)
	}
	if f.Path != `\\.\PhysicalDrive2` || f.DevNum != "2" || f.LayoutID != "DEADBEEF" || f.Vendor != "SanDisk" || f.Model != "Ultra" || f.Removable != Yes || f.Internal != No || !f.Whole {
		t.Errorf("%+v", f)
	}
	if _, err := BuildWinFacts(2, desc, WinGeometry{BytesPerSector: 512, DiskSize: 16 << 30}, 8<<30, l, false); err == nil {
		t.Error("two different sizes accepted")
	}
	if _, err := BuildWinFacts(2, desc, WinGeometry{DiskSize: 16 << 30}, 0, l, false); err == nil {
		t.Error("no sector size accepted")
	}
	if _, err := BuildWinFacts(2, desc, WinGeometry{BytesPerSector: 512, DiskSize: 16 << 30}, 0, l, false); err != nil {
		t.Errorf("an unavailable length (not elevated) must not refuse: %v", err)
	}

	vols := []WinVolume{
		{Name: `\\?\Volume{A}\`, Paths: []string{`C:\`}, FS: "NTFS", Disks: []int{0}},
		{Name: `\\?\Volume{B}\`, Paths: []string{`E:\`, `C:\mnt\card\`}, FS: "FAT32", Disks: []int{2}},
		{Name: `\\?\Volume{C}\`, Disks: []int{2, 5}}, // spanned across two disks, no path
		{Name: `\\?\Volume{D}\`, Paths: []string{`D:\`}, Disks: []int{1}},
	}
	roleVols := WinRoleVolumes{RoleBoot: {`\\?\Volume{A}\`}, RoleSystem: {`\\?\Volume{A}\`}, RolePagefile: {`\\?\Volume{D}\`}, RoleCrashdump: {`\\?\Volume{c}\`}}
	guards := []WinGuard{{Path: `C:\Users\me`, Volume: `\\?\Volume{A}\`}, {Path: `E:\repo`, Volume: `\\?\Volume{B}\`}}
	mounts, roles, hit := WinDiskView(2, vols, roleVols, guards)
	if len(mounts) != 3 || mounts[0].Point != `E:\` || mounts[1].Point != `C:\mnt\card\` || mounts[2].Point != `\\?\Volume{C}\` || mounts[0].FS != "FAT32" {
		t.Errorf("mounts %+v", mounts)
	}
	if len(roles) != 1 || roles[0] != RoleCrashdump || hit != `E:\repo` {
		t.Errorf("roles %v guard %q", roles, hit)
	}
	_, roles, hit = WinDiskView(0, vols, roleVols, guards)
	if len(roles) != 2 || roles[0] != RoleBoot || roles[1] != RoleSystem || hit != `C:\Users\me` {
		t.Errorf("system disk: %v %q", roles, hit)
	}
	_, roles, _ = WinDiskView(1, vols, roleVols, guards)
	if len(roles) != 1 || roles[0] != RolePagefile {
		t.Errorf("page file disk: %v", roles)
	}
	if m, r, h := WinDiskView(9, vols, roleVols, guards); m != nil || r != nil || h != "" {
		t.Errorf("unrelated disk: %v %v %q", m, r, h)
	}
	f.Roles = []Role{RoleESP}
	ApplyWinView(&f, mounts, []Role{RoleESP, RoleCrashdump}, hit)
	if n := len(f.Roles); n != 2 {
		t.Errorf("roles not deduplicated: %v", f.Roles)
	}
}

// ---- winDisk: the order after the write, with a recording fake ----

type fakeRaw struct {
	name string
	log  *[]string
	data []byte
}

func (r *fakeRaw) SectorSize() int { return 512 }
func (r *fakeRaw) ReadAt(p []byte, off int64) (int, error) {
	*r.log = append(*r.log, r.name+" read")
	if off >= int64(len(r.data)) {
		return 0, io.EOF
	}
	return copy(p, r.data[off:]), nil
}
func (r *fakeRaw) WriteAt(p []byte, off int64) (int, error) {
	*r.log = append(*r.log, r.name+" write")
	return copy(r.data[off:], p), nil
}
func (r *fakeRaw) Sync() error { *r.log = append(*r.log, r.name+" flush"); return nil }

type fakeCloser struct {
	name string
	log  *[]string
}

func (c fakeCloser) Close() error { *c.log = append(*c.log, c.name+" close"); return nil }

func newFakeWinDisk(t *testing.T, openVerifyErr error) (*winDisk, *[]string) {
	log := new([]string)
	w := &fakeRaw{name: "write", log: log, data: make([]byte, 4096)}
	v := &fakeRaw{name: "verify", log: log, data: make([]byte, 4096)}
	copy(v.data, "from the card")
	d, err := newWinDisk(winDevOps{
		Write:            w,
		UpdateProperties: func() error { *log = append(*log, "update properties"); return nil },
		OpenVerify: func() (blockio.Raw, io.Closer, error) {
			*log = append(*log, "open verify")
			if openVerifyErr != nil {
				return nil, nil, openVerifyErr
			}
			return v, fakeCloser{"verify", log}, nil
		},
		CloseWrite: func() error { *log = append(*log, "write close"); return nil },
		Release:    func() error { *log = append(*log, "release volumes"); return nil },
	})
	if err != nil {
		t.Fatal(err)
	}
	return d, log
}

// flush, update properties, uncached verify handle, reads from it, then the handles and the volume locks last.
func TestWinDiskOrderFlushUpdateVerifyRelease(t *testing.T) {
	d, log := newFakeWinDisk(t, nil)
	if _, err := d.WriteAt([]byte("x"), 0); err != nil { // partial sector: read-modify-write through the write handle
		t.Fatal(err)
	}
	d.Rescan()
	buf := make([]byte, 13)
	if _, err := d.ReadAt(buf, 0); err != nil || string(buf) != "from the card" {
		t.Fatalf("read %q %v", buf, err)
	}
	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	got := strings.Join(*log, ",")
	want := "write read,write write,write flush,update properties,open verify,verify read,verify close,write close,release volumes"
	if got != want {
		t.Errorf("order:\n got %s\nwant %s", got, want)
	}
}

// If the uncached handle cannot be opened, nothing is read from the cached one.
func TestWinDiskNeverReadsCachedAfterVerifyFailure(t *testing.T) {
	d, log := newFakeWinDisk(t, errors.New("access denied"))
	d.Rescan()
	if _, err := d.ReadAt(make([]byte, 16), 0); err == nil || !strings.Contains(err.Error(), "uncached") {
		t.Fatalf("got %v", err)
	}
	for _, s := range *log {
		if s == "write read" {
			t.Error("read through the write handle")
		}
	}
	d.Close()
	if (*log)[len(*log)-1] != "release volumes" {
		t.Errorf("volumes not released last: %v", *log)
	}
}
