package sdcard

// Windows disk facts: everything that can be decided without a system call lives here, untagged, so that it is
// unit-tested on Linux with hand-built buffers (spec 11, 4.4.5 and 4.4.8). The IOCTL calls themselves are in
// the *_windows.go files. Every parser bounds-checks every offset it follows and fails closed: a buffer that
// does not say what a disk is makes the disk ineligible.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/blockio"
)

// Control codes (CTL_CODE(device, function, method, access)); the numbers are not exported by x/sys/windows.
func ctlCode(dev, fn, method, access uint32) uint32 { return dev<<16 | access<<14 | fn<<2 | method }

const (
	ioctlStorageQueryProperty       = 0x002D1400 // IOCTL_STORAGE_QUERY_PROPERTY
	ioctlDiskGetDriveGeometryEx     = 0x000700A0 // IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
	ioctlDiskGetDriveLayoutEx       = 0x00070050 // IOCTL_DISK_GET_DRIVE_LAYOUT_EX
	ioctlDiskGetLengthInfo          = 0x0007405C // IOCTL_DISK_GET_LENGTH_INFO
	ioctlDiskIsWritable             = 0x00070024 // IOCTL_DISK_IS_WRITABLE
	ioctlDiskUpdateProperties       = 0x00070140 // IOCTL_DISK_UPDATE_PROPERTIES
	ioctlVolumeGetVolumeDiskExtents = 0x00560000 // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
	fsctlLockVolume                 = 0x00090018 // FSCTL_LOCK_VOLUME
	fsctlUnlockVolume               = 0x0009001C // FSCTL_UNLOCK_VOLUME
	fsctlDismountVolume             = 0x00090020 // FSCTL_DISMOUNT_VOLUME
	fsctlAllowExtendedDasdIo        = 0x00090083 // FSCTL_ALLOW_EXTENDED_DASD_IO
)

// Windows error numbers the callers map (kept here so that the mapping is testable).
const (
	winErrWriteProtect = 19
	winErrNotReady     = 21
	winErrNoMedia      = 1112
)

// Storage bus types (STORAGE_BUS_TYPE).
const (
	busUnknown  = 0x0
	busScsi     = 0x1
	busAtapi    = 0x2
	busAta      = 0x3
	bus1394     = 0x4
	busSsa      = 0x5
	busFibre    = 0x6
	busUsb      = 0x7
	busRaid     = 0x8
	busIscsi    = 0x9
	busSas      = 0xA
	busSata     = 0xB
	busSd       = 0xC
	busMmc      = 0xD
	busVirtual  = 0xE
	busFileVirt = 0xF
	busSpaces   = 0x10
	busNvme     = 0x11
	busScm      = 0x12
	busUfs      = 0x13

	maxWinBuffer  = 1 << 20 // upper bound of any buffer we accept from an IOCTL
	maxExtents    = 4096
	maxPartitions = 4096
)

// BusFacts maps a STORAGE_BUS_TYPE to the Facts bus name, whether the bus is virtual and the Internal flag
// derived from it: USB, SD and MMC are the only buses a card is accepted on (spec 4.4.5), everything else is
// either internal or unknown.
func BusFacts(bus uint32) (name string, virtual bool, internal Tri) {
	switch bus {
	case busUsb:
		return "usb", false, No
	case busSd:
		return "sd", false, No
	case busMmc:
		return "mmc", false, No
	case busSata, busAta:
		return "sata", false, Yes
	case busNvme:
		return "nvme", false, Yes
	case busScsi, busAtapi, busSsa, busFibre, busSas:
		return "scsi", false, Yes
	case busRaid:
		return "raid", false, Yes
	case busVirtual, busFileVirt, busSpaces:
		return "virtual", true, Yes
	case busIscsi, bus1394, busScm, busUfs:
		return "other", false, Unknown
	}
	return "unknown", false, Unknown
}

var physicalNodeRe = regexp.MustCompile(`^\\\\\.\\PhysicalDrive([0-9]{1,3})$`)

// WinPhysicalNode returns the disk number of a `\\.\PhysicalDriveN` path. A drive letter, a volume path or
// anything else is not a whole-disk node and is refused by the caller.
func WinPhysicalNode(p string) (int, bool) {
	m := physicalNodeRe.FindStringSubmatch(p)
	if m == nil {
		return 0, false
	}
	n, err := strconv.Atoi(m[1])
	return n, err == nil
}

// WinPhysicalPath is the node of disk n.
func WinPhysicalPath(n int) string { return fmt.Sprintf(`\\.\PhysicalDrive%d`, n) }

var errShort = errors.New("buffer too short")

func u32(b []byte, off int) (uint32, error) {
	if off < 0 || off+4 > len(b) {
		return 0, errShort
	}
	return binary.LittleEndian.Uint32(b[off:]), nil
}

func i64(b []byte, off int) (int64, error) {
	if off < 0 || off+8 > len(b) {
		return 0, errShort
	}
	return int64(binary.LittleEndian.Uint64(b[off:])), nil
}

// WinDescriptor is the decoded STORAGE_DEVICE_DESCRIPTOR.
type WinDescriptor struct {
	Removable                         bool
	BusType                           uint32
	Vendor, Product, Revision, Serial string
}

// ParseDeviceDescriptor decodes a STORAGE_DEVICE_DESCRIPTOR: a 36-byte header (RemovableMedia at 10, the four
// string offsets at 12..24, BusType at 28) and NUL-terminated ASCII strings at the given offsets from the start
// of the buffer (offset 0 = absent). An offset outside the buffer or a string without its NUL is an error.
func ParseDeviceDescriptor(b []byte) (WinDescriptor, error) {
	var d WinDescriptor
	if len(b) < 36 {
		return d, fmt.Errorf("storage descriptor: %w (%d bytes)", errShort, len(b))
	}
	d.Removable = b[10] != 0
	d.BusType, _ = u32(b, 28)
	for i, dst := range []*string{&d.Vendor, &d.Product, &d.Revision, &d.Serial} {
		off, _ := u32(b, 12+4*i)
		if off == 0 {
			continue
		}
		if off < 36 || int64(off) >= int64(len(b)) {
			return d, fmt.Errorf("storage descriptor: string offset %d outside the buffer", off)
		}
		end := -1
		for j := int(off); j < len(b); j++ {
			if b[j] == 0 {
				end = j
				break
			}
		}
		if end < 0 {
			return d, errors.New("storage descriptor: unterminated string")
		}
		*dst = strings.TrimSpace(string(b[off:end]))
	}
	return d, nil
}

// WinGeometry is the decoded DISK_GEOMETRY_EX.
type WinGeometry struct {
	BytesPerSector uint32
	DiskSize       int64
}

// ParseGeometryEx decodes a DISK_GEOMETRY_EX: DISK_GEOMETRY (24 bytes, BytesPerSector at 20) then DiskSize (i64 at 24).
func ParseGeometryEx(b []byte) (WinGeometry, error) {
	var g WinGeometry
	if len(b) < 32 {
		return g, fmt.Errorf("drive geometry: %w (%d bytes)", errShort, len(b))
	}
	g.BytesPerSector, _ = u32(b, 20)
	g.DiskSize, _ = i64(b, 24)
	if g.DiskSize < 0 {
		return g, errors.New("drive geometry: negative disk size")
	}
	return g, nil
}

// ParseLengthInfo decodes a GET_LENGTH_INFORMATION (one i64).
func ParseLengthInfo(b []byte) (int64, error) {
	n, err := i64(b, 0)
	if err != nil {
		return 0, fmt.Errorf("length information: %w", err)
	}
	if n < 0 {
		return 0, errors.New("length information: negative length")
	}
	return n, nil
}

// Partition styles (PARTITION_STYLE).
const (
	partStyleMBR = 0
	partStyleGPT = 1
	partStyleRAW = 2
)

// GPT partition type GUIDs that matter for the guards.
const (
	gptESP        = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
	gptRecovery   = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"
	gptLDMMeta    = "5808C8AA-7E8F-42E0-85D2-E1E90434CFB3"
	gptLDMData    = "AF9B60A0-1431-4F62-BC68-3311714A69AD"
	gptStorageSpc = "E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D"
)

// WinPartition is one partition of a drive layout. Type is the MBR type byte or the GPT type GUID.
type WinPartition struct {
	Style   uint32
	MBRType byte
	GPTType string
	Number  uint32
	Start   int64
	Length  int64
}

// WinLayout is the decoded DRIVE_LAYOUT_INFORMATION_EX.
type WinLayout struct {
	Style      uint32
	ID         string // "%08X" MBR signature, GPT disk GUID, "" for RAW
	Partitions []WinPartition
}

const (
	layoutHeader  = 8
	layoutUnion   = 40 // DRIVE_LAYOUT_INFORMATION_GPT is the biggest member
	partEntrySize = 144
)

// ParseDriveLayoutEx decodes a DRIVE_LAYOUT_INFORMATION_EX: style and count at 0 and 4, the MBR signature or the
// GPT DiskId in a 40-byte union at 8, then count PARTITION_INFORMATION_EX of 144 bytes from 48.
func ParseDriveLayoutEx(b []byte) (WinLayout, error) {
	var l WinLayout
	style, err := u32(b, 0)
	if err != nil {
		return l, fmt.Errorf("drive layout: %w", err)
	}
	count, err := u32(b, 4)
	if err != nil {
		return l, fmt.Errorf("drive layout: %w", err)
	}
	l.Style = style
	switch style {
	case partStyleMBR:
		sig, err := u32(b, layoutHeader)
		if err != nil {
			return l, fmt.Errorf("drive layout: %w", err)
		}
		l.ID = fmt.Sprintf("%08X", sig)
	case partStyleGPT:
		if layoutHeader+16 > len(b) {
			return l, fmt.Errorf("drive layout: %w", errShort)
		}
		l.ID = guidString(b[layoutHeader : layoutHeader+16])
	case partStyleRAW:
	default:
		return l, fmt.Errorf("drive layout: unknown partition style %d", style)
	}
	if count > maxPartitions || int64(layoutHeader+layoutUnion)+int64(count)*partEntrySize > int64(len(b)) {
		return l, fmt.Errorf("drive layout: %d partitions do not fit in %d bytes", count, len(b))
	}
	for i := 0; i < int(count); i++ {
		e := b[layoutHeader+layoutUnion+i*partEntrySize:][:partEntrySize]
		var p WinPartition
		p.Style, _ = u32(e, 0)
		p.Start, _ = i64(e, 8)
		p.Length, _ = i64(e, 16)
		p.Number, _ = u32(e, 24)
		switch p.Style {
		case partStyleMBR:
			p.MBRType = e[32]
			if p.MBRType == 0 {
				continue // an empty slot
			}
		case partStyleGPT:
			p.GPTType = guidString(e[32:48])
			if p.GPTType == "00000000-0000-0000-0000-000000000000" {
				continue
			}
		default:
			return l, fmt.Errorf("drive layout: partition %d has unknown style %d", i, p.Style)
		}
		l.Partitions = append(l.Partitions, p)
	}
	return l, nil
}

// guidString formats a Windows GUID (Data1..3 little-endian, Data4 as is) in upper case.
func guidString(g []byte) string {
	return fmt.Sprintf("%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		binary.LittleEndian.Uint32(g), binary.LittleEndian.Uint16(g[4:]), binary.LittleEndian.Uint16(g[6:]),
		g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15])
}

// Classify reads the partition types: a dynamic (LDM) disk, a Storage Spaces member, and the roles the types
// imply (EFI system partition, recovery partition).
func (l WinLayout) Classify() (dynamic bool, stack string, roles []Role) {
	var esp, rec bool
	for _, p := range l.Partitions {
		switch p.Style {
		case partStyleMBR:
			switch p.MBRType {
			case 0x42:
				dynamic = true
			case 0xEF:
				esp = true
			case 0x27:
				rec = true
			}
		case partStyleGPT:
			switch p.GPTType {
			case gptLDMMeta, gptLDMData:
				dynamic = true
			case gptStorageSpc:
				stack = "Storage Spaces"
			case gptESP:
				esp = true
			case gptRecovery:
				rec = true
			}
		}
	}
	if esp {
		roles = append(roles, RoleESP)
	}
	if rec {
		roles = append(roles, RoleRecovery)
	}
	return
}

// DiskExtent is one run of a volume on a disk.
type DiskExtent struct {
	Disk          uint32
	Start, Length int64
}

// DiskExtentCount reads NumberOfDiskExtents of a VOLUME_DISK_EXTENTS header (valid even when the buffer was
// too small for the extents, which is how the caller sizes the retry).
func DiskExtentCount(b []byte) (int, error) {
	n, err := u32(b, 0)
	if err != nil {
		return 0, fmt.Errorf("volume extents: %w", err)
	}
	if n > maxExtents {
		return 0, fmt.Errorf("volume extents: %d extents", n)
	}
	return int(n), nil
}

// DiskExtentsSize is the buffer size that holds n extents.
func DiskExtentsSize(n int) int { return 8 + 24*n }

// ParseDiskExtents decodes a VOLUME_DISK_EXTENTS: count at 0, then DISK_EXTENT (24 bytes: disk number u32, pad,
// start i64, length i64) from 8. A spanned volume has several.
func ParseDiskExtents(b []byte) ([]DiskExtent, error) {
	n, err := DiskExtentCount(b)
	if err != nil {
		return nil, err
	}
	if DiskExtentsSize(n) > len(b) {
		return nil, fmt.Errorf("volume extents: %d extents do not fit in %d bytes", n, len(b))
	}
	out := make([]DiskExtent, n)
	for i := range out {
		o := 8 + 24*i
		out[i].Disk, _ = u32(b, o)
		out[i].Start, _ = i64(b, o+8)
		out[i].Length, _ = i64(b, o+16)
	}
	return out, nil
}

// WritableFromError maps the result of IOCTL_DISK_IS_WRITABLE (0 = success) to the read-only flag. Only
// ERROR_WRITE_PROTECT means read-only; any other failure is an unknown, so the disk is refused.
func WritableFromError(code uint32) (readOnly bool, err error) {
	switch code {
	case 0:
		return false, nil
	case winErrWriteProtect:
		return true, nil
	}
	return false, fmt.Errorf("cannot tell whether the disk is writable (error %d)", code)
}

// WinPagefile is one entry of the PagingFiles registry value.
type WinPagefile struct {
	Letter string // "C:" (empty when the entry names a volume)
	Volume string // `\\?\Volume{GUID}` (empty when the entry names a letter)
	Auto   bool   // "?:\pagefile.sys": the system picks the volume
}

var (
	pagingTailRe = regexp.MustCompile(`^(.+?)\s+[0-9]+\s+[0-9]+\s*$`)
	letterRe     = regexp.MustCompile(`^([A-Za-z]:)(\\|$)`)
	volumeRe     = regexp.MustCompile(`(?i)^(\\\\[?.]\\Volume\{[0-9a-f-]+\})(\\|$)`)
)

// ParsePagingFiles reads the REG_MULTI_SZ HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PagingFiles
// ("C:\pagefile.sys 0 0", "?:\pagefile.sys", ...). An entry that names neither a drive letter nor a volume is an
// error: the page-file guard would otherwise be blind.
func ParsePagingFiles(entries []string) ([]WinPagefile, error) {
	var out []WinPagefile
	for _, e := range entries {
		e = strings.TrimSpace(e)
		if e == "" {
			continue
		}
		if m := pagingTailRe.FindStringSubmatch(e); m != nil {
			e = m[1]
		}
		e = strings.TrimPrefix(e, `\??\`)
		switch {
		case strings.HasPrefix(e, "?:"):
			out = append(out, WinPagefile{Auto: true})
		case letterRe.MatchString(e):
			out = append(out, WinPagefile{Letter: strings.ToUpper(e[:2])})
		case volumeRe.MatchString(e):
			out = append(out, WinPagefile{Volume: volumeRe.FindStringSubmatch(e)[1]})
		default:
			return nil, fmt.Errorf("page file entry %q names no drive letter or volume", e)
		}
	}
	return out, nil
}

// BuildWinFacts assembles the Facts of disk n from the decoded IOCTL answers. length is the answer of
// IOCTL_DISK_GET_LENGTH_INFO, or 0 when the call was not possible (it needs read access: a process that is
// not elevated cannot make it); when present it must agree with the geometry, else the disk is refused.
// Volumes, roles from the system and guard hits are added by WinDiskView.
func BuildWinFacts(n int, d WinDescriptor, g WinGeometry, length int64, l WinLayout, readOnly bool) (Facts, error) {
	if g.BytesPerSector == 0 {
		return Facts{}, errors.New("the drive geometry reports no sector size")
	}
	if length != 0 && length != g.DiskSize {
		return Facts{}, fmt.Errorf("the drive reports two sizes (%d and %d)", g.DiskSize, length)
	}
	bus, virtual, internal := BusFacts(d.BusType)
	removable := No
	if d.Removable {
		removable = Yes
	}
	dynamic, stack, roles := l.Classify()
	return Facts{
		OS: "windows", Path: WinPhysicalPath(n), Name: fmt.Sprintf("PhysicalDrive%d", n), Whole: true,
		Size: g.DiskSize, SectorSize: int(g.BytesPerSector), Bus: bus, Removable: removable, Internal: internal,
		Virtual: virtual, ReadOnly: readOnly, Model: d.Product, Vendor: d.Vendor, Serial: d.Serial,
		Revision: d.Revision, LayoutID: l.ID, DevNum: strconv.Itoa(n), Roles: roles, InStack: stack, Dynamic: dynamic,
	}, nil
}

// WinVolume is one volume of the machine: its GUID name (`\\?\Volume{...}\`), every path it is reachable by
// (drive letters and folder mount points, with the trailing backslash), its file system and its disks.
type WinVolume struct {
	Name  string
	Paths []string
	FS    string
	Disks []int
}

// WinGuard is one guarded path and the GUID name of the volume it lives on.
type WinGuard struct {
	Path, Volume string
}

// WinRoleVolumes says which volumes carry a system role (GUID names): the Windows directory (boot), the
// system directory and %SystemDrive% (system), page files, the dedicated crash dump file.
type WinRoleVolumes map[Role][]string

func (v WinVolume) onDisk(n int) bool {
	for _, d := range v.Disks {
		if d == n {
			return true
		}
	}
	return false
}

func volKey(s string) string { return strings.ToLower(strings.TrimRight(s, `\`)) }

// WinDiskView answers, for disk n: the mounts (one per path of every volume on it, so that the red box can show
// every drive letter; a volume with no path shows its GUID name), the system roles, and the first guarded path
// that lives on it. Roles come out in a fixed order.
func WinDiskView(n int, vols []WinVolume, roleVols WinRoleVolumes, guards []WinGuard) (mounts []Mount, roles []Role, guardHit string) {
	on := map[string]bool{}
	for _, v := range vols {
		if !v.onDisk(n) {
			continue
		}
		on[volKey(v.Name)] = true
		paths := v.Paths
		if len(paths) == 0 {
			paths = []string{v.Name}
		}
		for _, p := range paths {
			mounts = append(mounts, Mount{Node: v.Name, Point: p, FS: v.FS})
		}
	}
	for _, r := range []Role{RoleBoot, RoleSystem, RolePagefile, RoleHibernation, RoleCrashdump} {
		for _, name := range roleVols[r] {
			if on[volKey(name)] {
				roles = append(roles, r)
				break
			}
		}
	}
	for _, g := range guards {
		if on[volKey(g.Volume)] {
			guardHit = g.Path
			break
		}
	}
	return
}

// winDevOps is the system side of an opened Windows disk, injectable so that the ORDER of the steps after the
// write (flush, update properties, uncached read-back, release) is asserted on Linux with a recording fake.
type winDevOps struct {
	Write            blockio.Raw // the write-through handle of \\.\PhysicalDriveN
	UpdateProperties func() error
	// OpenVerify opens the second, uncached handle used for the read-back.
	OpenVerify func() (blockio.Raw, io.Closer, error)
	CloseWrite func() error
	// Release unlocks and closes the volume handles: it runs last, so that the system cannot remount earlier.
	Release func() error
}

// winDisk is a Disk over a Windows physical drive: every I/O goes through blockio.Aligned (sector-aligned
// offsets and lengths, page-aligned buffers). Reads come from the write handle until Rescan, after which
// they come from the uncached handle, because a read through the write handle can return the cache.
type winDisk struct {
	ops       winDevOps
	wa        *blockio.Aligned
	va        *blockio.Aligned
	vc        io.Closer
	verifyErr error
}

func newWinDisk(ops winDevOps) (*winDisk, error) {
	wa, err := blockio.New(ops.Write)
	if err != nil {
		return nil, err
	}
	return &winDisk{ops: ops, wa: wa}, nil
}

func (d *winDisk) WriteAt(p []byte, off int64) (int, error) { return d.wa.WriteAt(p, off) }
func (d *winDisk) Sync() error                              { return d.wa.Sync() }

func (d *winDisk) ReadAt(p []byte, off int64) (int, error) {
	if d.verifyErr != nil {
		return 0, d.verifyErr // never fall back to a cached read once the uncached one was asked for
	}
	if d.va != nil {
		return d.va.ReadAt(p, off)
	}
	return d.wa.ReadAt(p, off)
}

// Rescan flushes, asks the system to re-read the layout and switches reads to the uncached handle.
func (d *winDisk) Rescan() {
	_ = d.wa.Sync()
	if d.ops.UpdateProperties != nil {
		_ = d.ops.UpdateProperties()
	}
	if d.va != nil || d.verifyErr != nil {
		return
	}
	raw, c, err := d.ops.OpenVerify()
	if err != nil {
		d.verifyErr = fmt.Errorf("no uncached handle for the read-back: %w", err)
		return
	}
	va, err := blockio.New(raw)
	if err != nil {
		c.Close()
		d.verifyErr = err
		return
	}
	d.va, d.vc = va, c
}

// Close closes the verify handle, then the write handle, then releases the volumes (last).
func (d *winDisk) Close() error {
	var first error
	keep := func(err error) {
		if first == nil {
			first = err
		}
	}
	if d.vc != nil {
		keep(d.vc.Close())
		d.vc = nil
	}
	if d.ops.CloseWrite != nil {
		keep(d.ops.CloseWrite())
	}
	if d.ops.Release != nil {
		keep(d.ops.Release())
	}
	return first
}

// ApplyWinView adds the volume view to facts built by BuildWinFacts (roles from the layout are kept, deduplicated).
func ApplyWinView(f *Facts, mounts []Mount, roles []Role, guardHit string) {
	f.Mounts = mounts
	f.GuardHit = guardHit
	for _, r := range roles {
		dup := false
		for _, have := range f.Roles {
			dup = dup || have == r
		}
		if !dup {
			f.Roles = append(f.Roles, r)
		}
	}
}
