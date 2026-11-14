package sdcard

import "fmt"

// Facts is what an OS inventory knows about one disk: a plain, JSON-serialisable struct with no decision in
// it. Evaluate turns Facts into a verdict, so the rules are unit-tested on Linux for every OS (specs/installer.md).
type Facts struct {
	OS         string `json:"os"`   // "linux" | "darwin" | "windows"
	Path       string `json:"path"` // /dev/sdb | /dev/disk4 | \\.\PhysicalDrive2
	Name       string `json:"name"`
	Whole      bool   `json:"whole"` // a whole disk, not a partition/slice
	Size       int64  `json:"size"`
	SectorSize int    `json:"sector_size,omitempty"` // logical; 0 = not reported (Linux sysfs sizes are in 512-byte units)
	// Bus: usb | sd | mmc | sata | nvme | scsi | raid | virtual | other | unknown
	Bus       string `json:"bus"`
	Removable Tri    `json:"removable"`
	Internal  Tri    `json:"internal"`
	Virtual   bool   `json:"virtual,omitempty"` // disk image, file-backed, loop, zram, storage spaces
	ReadOnly  bool   `json:"read_only,omitempty"`
	Model     string `json:"model,omitempty"`
	Vendor    string `json:"vendor,omitempty"`
	Serial    string `json:"serial,omitempty"`
	WWN       string `json:"wwn,omitempty"`
	Revision  string `json:"revision,omitempty"`
	LayoutID  string `json:"layout_id,omitempty"` // MBR signature, GPT GUID, or ""
	// DevNum is the device number (Linux major:minor), part of the identity token.
	DevNum string  `json:"dev_num,omitempty"`
	Mounts []Mount `json:"mounts,omitempty"` // every mounted node of the disk
	Roles  []Role  `json:"roles,omitempty"`  // boot, system, pagefile, swap, hibernation, esp, recovery, crashdump
	// InStack is the reason when the disk is part of LVM/LUKS/RAID/dynamic disk/storage pool, else "".
	InStack string `json:"in_stack,omitempty"`
	Dynamic bool   `json:"dynamic,omitempty"` // Windows dynamic disk
	// GuardHit is the first guarded path (repository, home, working, temp...) that lives on this disk, else "".
	// The inventory computes it from the Context it was given: only it sees the whole mount table.
	GuardHit string `json:"guard_hit,omitempty"`
	// Linux detail kept for the exact refusal sentences.
	DevType      string `json:"dev_type,omitempty"`      // sysfs device/type (mmcblk: "SD")
	RemovableRaw string `json:"removable_raw,omitempty"` // sysfs removable attribute
}

// Tri is a three-state flag: yes, no or unknown (the zero value).
type Tri int8

const (
	Unknown Tri = iota
	Yes
	No
)

func (t Tri) String() string {
	switch t {
	case Yes:
		return "yes"
	case No:
		return "no"
	}
	return "unknown"
}

func (t Tri) MarshalText() ([]byte, error) { return []byte(t.String()), nil }

func (t *Tri) UnmarshalText(b []byte) error {
	switch string(b) {
	case "yes":
		*t = Yes
	case "no":
		*t = No
	case "unknown", "":
		*t = Unknown
	default:
		return fmt.Errorf("bad tri-state %q", b)
	}
	return nil
}

// Role is a system role a disk can carry.
type Role string

const (
	RoleBoot        Role = "boot"
	RoleSystem      Role = "system"
	RolePagefile    Role = "pagefile"
	RoleSwap        Role = "swap"
	RoleHibernation Role = "hibernation"
	RoleESP         Role = "esp"
	RoleRecovery    Role = "recovery"
	RoleCrashdump   Role = "crashdump"
)

// Mount is one mounted node of a disk.
type Mount struct {
	Node  string `json:"node"`
	Point string `json:"point"`
	FS    string `json:"fs,omitempty"`
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

// Context is what the caller adds to the question "may this disk be written": the paths that must not sit on it.
type Context struct {
	GuardPaths []string
}

// Policy is the size window of an acceptable card.
type Policy struct {
	MinBytes, MaxBytes int64
}

// Size range of an acceptable card (backend_linux.sh).
const (
	MinBytes = 128 << 20
	MaxBytes = 128 << 30
)

// DefaultPolicy is the policy of every OS.
var DefaultPolicy = Policy{MinBytes: MinBytes, MaxBytes: MaxBytes}
