package sdcard

import (
	"errors"
	"fmt"
	"regexp"
	"strings"
)

// Ident is the identity token of a disk. On Linux it is byte-identical to the historical token
// size|model|serial|wwn|vendor|major:minor; the other OSes add the revision and the layout id.
func Ident(f Facts) string {
	if f.OS == "" || f.OS == "linux" {
		return fmt.Sprintf("%d|%s|%s|%s|%s|%s", f.Size, f.Model, f.Serial, f.WWN, f.Vendor, f.DevNum)
	}
	id := fmt.Sprintf("%s|%d|%s|%s|%s|%s|%s|%s|%s", f.OS, f.Size, f.Model, f.Vendor, f.Serial, f.WWN, f.Revision, f.LayoutID, f.DevNum)
	if f.OS == "windows" {
		// \\.\PhysicalDriveN is renumbered after a replug: the bus and the removable flag are part of what was confirmed
		id += "|" + f.Bus + "|" + f.Removable.String()
	}
	return id
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

var darwinNodeRe = regexp.MustCompile(`^/dev/disk[0-9]+$`)

// isDarwinSystemMount: mount points that belong to the running macOS.
func isDarwinSystemMount(p string) bool {
	for _, s := range []string{"/System", "/Users", "/private", "/Library", "/Applications", "/usr", "/bin", "/sbin", "/opt", "/cores"} {
		if p == s || strings.HasPrefix(p, s+"/") {
			return true
		}
	}
	return false
}

// Evaluate decides whether the disk described by f is an eligible card. The error text is the reason for a
// refusal. It is pure: no file, no system call. The order of the rules is the order of the historical Linux guard.
func Evaluate(f Facts, p Policy) (*Candidate, error) {
	dev := f.Path
	if !f.Whole {
		if f.OS == "windows" {
			return nil, fmt.Errorf(`%s is not a whole physical drive node like \\.\PhysicalDrive2 (drive letters, volumes and partitions are refused)`, dev)
		}
		return nil, fmt.Errorf("%s is not a whole USB/SD disk node like /dev/sdb or /dev/mmcblk0 (partitions, NVMe, loop and zram nodes are refused)", dev)
	}
	if f.OS == "darwin" && !darwinNodeRe.MatchString(f.Path) {
		return nil, fmt.Errorf("%s is not a /dev/diskN node (slices and raw nodes are refused)", dev)
	}
	for _, m := range f.Mounts {
		if isSystemMount(m.Point) || (f.OS == "darwin" && isDarwinSystemMount(m.Point)) {
			return nil, fmt.Errorf("%s carries the system path %s", dev, m.Point)
		}
	}
	for _, r := range f.Roles {
		if r == RoleSwap {
			return nil, fmt.Errorf("%s carries swap space", dev)
		}
	}
	if len(f.Roles) > 0 {
		return nil, fmt.Errorf("%s carries the system role %q", dev, string(f.Roles[0]))
	}
	if f.InStack != "" {
		return nil, fmt.Errorf("%s is part of a storage stack (%s)", dev, f.InStack)
	}
	if f.Dynamic {
		return nil, fmt.Errorf("%s is part of a storage stack (dynamic disk)", dev)
	}
	if f.GuardHit != "" {
		return nil, fmt.Errorf("%s carries %s (the repository, your home, the working or the temp directory)", dev, f.GuardHit)
	}
	if f.Virtual {
		return nil, fmt.Errorf("%s is a virtual or file-backed disk", dev)
	}
	switch f.OS {
	case "", "linux", "darwin", "windows":
	default:
		return nil, fmt.Errorf("%s: disks of the OS %q are not supported", dev, f.OS)
	}
	if f.OS == "" || f.OS == "linux" {
		if f.Bus == "mmc" {
			if f.DevType != "SD" {
				return nil, fmt.Errorf("%s is not an SD card (device type %q)", dev, f.DevType)
			}
		} else {
			if f.RemovableRaw != "1" {
				return nil, fmt.Errorf("%s is not removable (removable=%q)", dev, f.RemovableRaw)
			}
			if f.Bus != "usb" {
				return nil, fmt.Errorf("%s is not USB-attached", dev)
			}
		}
	} else {
		switch f.Bus {
		case "usb", "sd", "mmc":
		default:
			return nil, fmt.Errorf("%s is not a USB or SD disk (bus %q)", dev, f.Bus)
		}
		// An internal eMMC or SD-bus data disk (cheap laptops) is on the same bus as a card slot, and only the
		// system-role scan would stop it. On the SD and MMC buses the disk must positively say it is removable
		// (STORAGE_DEVICE_DESCRIPTOR.RemovableMedia); unknown refuses. USB keeps the spec rule (Q2): many USB
		// readers report RemovableMedia false. IOCTL_STORAGE_GET_HOTPLUG_INFO is not used: DeviceHotplug is "designed
		// to be hot-plugged", which hot-swap capable internal controllers also set, so it is no proof of a card slot.
		if f.OS == "windows" && (f.Bus == "sd" || f.Bus == "mmc") && f.Removable != Yes {
			return nil, fmt.Errorf("%s is on the %s bus but not reported as removable (removable=%s): an internal eMMC or SD controller disk is refused", dev, f.Bus, f.Removable)
		}
		// fail closed: an internal flag the inventory could not read is not "external"
		switch f.Internal {
		case Yes:
			// a Mac's built-in SD slot reports Internal: accepted on the protocol alone (specs/installer.md)
			if f.OS == "darwin" && f.Bus == "sd" {
				break
			}
			return nil, fmt.Errorf("%s is an internal disk", dev)
		case Unknown:
			return nil, fmt.Errorf("%s: cannot tell whether it is an internal disk: refusing", dev)
		}
	}
	if f.Size <= 0 {
		return nil, fmt.Errorf("%s reports no medium (size 0)", dev)
	}
	if f.Size < p.MinBytes || f.Size > p.MaxBytes {
		return nil, fmt.Errorf("%s size %d bytes is outside 128 MiB .. 128 GiB", dev, f.Size)
	}
	if f.OS == "darwin" && f.SectorSize == 0 {
		return nil, fmt.Errorf("%s: cannot read the logical sector size: refusing", dev)
	}
	if f.SectorSize != 0 && f.SectorSize != 512 {
		return nil, fmt.Errorf("%s has %d-byte logical sectors: only 512-byte sectors are supported", dev, f.SectorSize)
	}
	if f.ReadOnly {
		return nil, fmt.Errorf("%s is read-only (the card's write-protect switch)", dev)
	}
	return &Candidate{Dev: dev, Name: f.Name, Size: f.Size, Model: f.Model, Tran: f.Bus, Mounts: f.Mounts, Ident: Ident(f)}, nil
}

// ErrIdentChanged means the disk is not the one that was confirmed.
var ErrIdentChanged = errors.New("the disk changed since it was checked: card replaced?")
