package sdcard

import (
	"errors"
	"fmt"
	"strings"
)

// Ident is the identity token of a disk. On Linux it is byte-identical to the historical token
// size|model|serial|wwn|vendor|major:minor; the other OSes add the revision and the layout id.
func Ident(f Facts) string {
	if f.OS == "" || f.OS == "linux" {
		return fmt.Sprintf("%d|%s|%s|%s|%s|%s", f.Size, f.Model, f.Serial, f.WWN, f.Vendor, f.DevNum)
	}
	return fmt.Sprintf("%s|%d|%s|%s|%s|%s|%s|%s|%s", f.OS, f.Size, f.Model, f.Vendor, f.Serial, f.WWN, f.Revision, f.LayoutID, f.DevNum)
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

// Evaluate decides whether the disk described by f is an eligible card. The error text is the reason for a
// refusal. It is pure: no file, no system call. The order of the rules is the order of the historical Linux guard.
func Evaluate(f Facts, p Policy) (*Candidate, error) {
	dev := f.Path
	if !f.Whole {
		return nil, fmt.Errorf("%s is not a whole USB/SD disk node like /dev/sdb or /dev/mmcblk0 (partitions, NVMe, loop and zram nodes are refused)", dev)
	}
	for _, m := range f.Mounts {
		if isSystemMount(m.Point) {
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
		if f.Virtual {
			return nil, fmt.Errorf("%s is a virtual or file-backed disk", dev)
		}
		if f.Internal == Yes {
			return nil, fmt.Errorf("%s is an internal disk", dev)
		}
	}
	if f.Size <= 0 {
		return nil, fmt.Errorf("%s reports no medium (size 0)", dev)
	}
	if f.Size < p.MinBytes || f.Size > p.MaxBytes {
		return nil, fmt.Errorf("%s size %d bytes is outside 128 MiB .. 128 GiB", dev, f.Size)
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
