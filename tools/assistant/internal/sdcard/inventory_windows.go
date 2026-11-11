//go:build windows

package sdcard

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

// winInventory reads the facts of a Windows disk by DeviceIoControl (spec 11, 4.4.5). It never decides: every
// fact it cannot read is an error, and an error makes the disk ineligible (fail closed). The query IOCTLs are
// opened with access 0, so listing needs no administrator rights.
type winInventory struct{}

const maxDisks = 128 // PhysicalDrive0..127: numbers can have gaps after a disk was removed

func (winInventory) Devices() []string {
	var out []string
	for n := 0; n < maxDisks; n++ {
		h, err := openPath(WinPhysicalPath(n), 0, 0)
		if err != nil {
			continue
		}
		windows.CloseHandle(h)
		out = append(out, WinPhysicalPath(n))
	}
	return out
}

func (winInventory) Inspect(dev string, ctx Context) (Facts, error) {
	n, ok := WinPhysicalNode(dev)
	if !ok {
		// a drive letter, a volume path, a partition: not a whole-disk node, Evaluate refuses it
		return Facts{OS: "windows", Path: dev, Name: dev}, nil
	}
	h, err := openPath(WinPhysicalPath(n), 0, 0)
	if err != nil {
		return Facts{}, fmt.Errorf("cannot open %s: %w", dev, err)
	}
	defer windows.CloseHandle(h)
	f, err := diskFacts(h, n)
	if err != nil {
		return Facts{}, fmt.Errorf("%s: %w", dev, err)
	}
	vols, err := enumVolumes()
	if err != nil {
		return Facts{}, fmt.Errorf("%s: %w", dev, err)
	}
	roleVols, err := systemRoleVolumes()
	if err != nil {
		return Facts{}, fmt.Errorf("%s: %w", dev, err)
	}
	guards, err := resolveGuards(ctx.GuardPaths)
	if err != nil {
		return Facts{}, fmt.Errorf("%s: %w", dev, err)
	}
	mounts, roles, hit := WinDiskView(n, vols, roleVols, guards)
	ApplyWinView(&f, mounts, roles, hit)
	return f, nil
}

// diskFacts reads everything about disk n that its own handle can say (descriptor, geometry, length, layout,
// writability). It is also what Open runs on the handle it just opened, to compare identities.
func diskFacts(h windows.Handle, n int) (Facts, error) {
	query := make([]byte, 12) // STORAGE_PROPERTY_QUERY: StorageDeviceProperty (0), PropertyStandardQuery (0)
	raw, err := ioctlGrow(h, ioctlStorageQueryProperty, query, 1024)
	if err != nil {
		return Facts{}, fmt.Errorf("cannot read the device descriptor: %w", err)
	}
	desc, err := ParseDeviceDescriptor(raw)
	if err != nil {
		return Facts{}, err
	}
	raw, err = ioctlGrow(h, ioctlDiskGetDriveGeometryEx, nil, 256)
	if err != nil {
		return Facts{}, fmt.Errorf("cannot read the drive geometry: %w", err)
	}
	geo, err := ParseGeometryEx(raw)
	if err != nil {
		return Facts{}, err
	}
	// needs read access: a process that is not elevated cannot ask, the geometry then stands alone
	var length int64
	if raw, err = ioctlGrow(h, ioctlDiskGetLengthInfo, nil, 8); err == nil {
		if length, err = ParseLengthInfo(raw); err != nil {
			return Facts{}, err
		}
	}
	raw, err = ioctlGrow(h, ioctlDiskGetDriveLayoutEx, nil, 16<<10)
	if err != nil {
		return Facts{}, fmt.Errorf("cannot read the partition layout: %w", err)
	}
	layout, err := ParseDriveLayoutEx(raw)
	if err != nil {
		return Facts{}, err
	}
	_, werr := ioctl(h, ioctlDiskIsWritable, nil, nil)
	readOnly, err := WritableFromError(errno0(werr))
	if err != nil {
		return Facts{}, err
	}
	return BuildWinFacts(n, desc, geo, length, layout, readOnly)
}

// errno0 is the error number of err, 0 for nil.
func errno0(err error) uint32 {
	if err == nil {
		return 0
	}
	return errno(err)
}

// skippable reports a volume with no medium (an empty card reader slot): it is on no disk.
func skippable(err error) bool {
	c := errno0(err)
	return c == winErrNotReady || c == winErrNoMedia
}

// enumVolumes lists every local volume with its paths, file system and disks (also for a spanned volume).
func enumVolumes() ([]WinVolume, error) {
	buf := make([]uint16, 512)
	fh, err := windows.FindFirstVolume(&buf[0], uint32(len(buf)))
	if err != nil {
		if errors.Is(err, windows.ERROR_NO_MORE_FILES) {
			return nil, nil
		}
		return nil, fmt.Errorf("cannot list the volumes: %w", err)
	}
	defer windows.FindVolumeClose(fh)
	var out []WinVolume
	for {
		name := windows.UTF16ToString(buf)
		v, ok, err := inspectVolume(name)
		if err != nil {
			return nil, err
		}
		if ok {
			out = append(out, v)
		}
		if err := windows.FindNextVolume(fh, &buf[0], uint32(len(buf))); err != nil {
			if errors.Is(err, windows.ERROR_NO_MORE_FILES) {
				return out, nil
			}
			return nil, fmt.Errorf("cannot list the volumes: %w", err)
		}
	}
}

// inspectVolume describes one volume `\\?\Volume{GUID}\`. ok is false for what is not on a disk of this
// machine (optical and network volumes, an empty card reader). Any other failure is an error: a volume whose
// disks are unknown could be on the card.
func inspectVolume(name string) (WinVolume, bool, error) {
	root, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return WinVolume{}, false, err
	}
	switch windows.GetDriveType(root) {
	case windows.DRIVE_CDROM, windows.DRIVE_REMOTE:
		return WinVolume{}, false, nil
	}
	h, err := openPath(strings.TrimRight(name, `\`), 0, 0)
	if err != nil {
		if skippable(err) {
			return WinVolume{}, false, nil
		}
		return WinVolume{}, false, fmt.Errorf("cannot open volume %s: %w", name, err)
	}
	defer windows.CloseHandle(h)
	raw, err := ioctlGrow(h, ioctlVolumeGetVolumeDiskExtents, nil, DiskExtentsSize(4))
	if err != nil {
		if skippable(err) {
			return WinVolume{}, false, nil
		}
		return WinVolume{}, false, fmt.Errorf("cannot read the disks of volume %s: %w", name, err)
	}
	ext, err := ParseDiskExtents(raw)
	if err != nil {
		return WinVolume{}, false, err
	}
	v := WinVolume{Name: name, Paths: volumePaths(root)}
	for _, e := range ext {
		v.Disks = append(v.Disks, int(e.Disk))
	}
	fs := make([]uint16, 64)
	if windows.GetVolumeInformation(root, nil, 0, nil, nil, nil, &fs[0], uint32(len(fs))) == nil {
		v.FS = windows.UTF16ToString(fs)
	}
	return v, true, nil
}

// volumePaths lists the drive letters and folder mount points of a volume (a multi-string).
func volumePaths(root *uint16) []string {
	size := uint32(256)
	for tries := 0; tries < 4; tries++ {
		buf := make([]uint16, size)
		err := windows.GetVolumePathNamesForVolumeName(root, &buf[0], size, &size)
		if err == nil {
			var out []string
			for len(buf) > 0 && buf[0] != 0 {
				i := 0
				for buf[i] != 0 {
					i++
				}
				out = append(out, windows.UTF16ToString(buf[:i]))
				buf = buf[i+1:]
			}
			return out
		}
		if !errors.Is(err, windows.ERROR_MORE_DATA) {
			return nil
		}
	}
	return nil
}

// volumeOfPath returns the GUID name of the volume that holds path (which need not exist: the nearest
// existing parent decides).
func volumeOfPath(path string) (string, error) {
	p := filepath.Clean(path)
	var lastErr error
	for {
		in, err := windows.UTF16PtrFromString(p)
		if err != nil {
			return "", err
		}
		root := make([]uint16, 1024)
		if err = windows.GetVolumePathName(in, &root[0], uint32(len(root))); err == nil {
			name := make([]uint16, 64)
			if err = windows.GetVolumeNameForVolumeMountPoint(&root[0], &name[0], uint32(len(name))); err == nil {
				return windows.UTF16ToString(name), nil
			}
		}
		lastErr = err
		if c := errno0(err); c != uint32(windows.ERROR_FILE_NOT_FOUND) && c != uint32(windows.ERROR_PATH_NOT_FOUND) && c != uint32(windows.ERROR_INVALID_NAME) {
			return "", fmt.Errorf("cannot find the volume of %s: %w", path, err)
		}
		parent := filepath.Dir(p)
		if parent == p {
			return "", fmt.Errorf("cannot find the volume of %s: %w", path, lastErr)
		}
		p = parent
	}
}

// resolveGuards finds the volume of each guarded path. A path that is not local (UNC share) cannot sit on a
// local disk and is skipped; any other path whose volume cannot be found makes every disk ineligible.
func resolveGuards(paths []string) ([]WinGuard, error) {
	var out []WinGuard
	for _, p := range paths {
		if p == "" {
			continue
		}
		if abs, err := filepath.Abs(p); err == nil {
			p2 := abs
			if strings.HasPrefix(p2, `\\`) && !strings.HasPrefix(p2, `\\?\`) && !strings.HasPrefix(p2, `\\.\`) {
				continue
			}
			p = p2
		}
		v, err := volumeOfPath(p)
		if err != nil {
			return nil, err
		}
		out = append(out, WinGuard{Path: p, Volume: v})
		// a junction or symbolic link can lead to another volume (the working directory reached through a link
		// to a card): the target is guarded too
		if real, err := filepath.EvalSymlinks(p); err == nil && !strings.EqualFold(real, p) {
			rv, err := volumeOfPath(real)
			if err != nil {
				return nil, err
			}
			out = append(out, WinGuard{Path: p, Volume: rv})
		}
	}
	return out, nil
}

// systemRoleVolumes finds the volumes with a system role: the Windows directory (boot), the system directory
// and %SystemDrive% (system), the page files, a dedicated crash dump file.
//
// Page files come from the PagingFiles registry value, the documented configuration, rather than from
// NtQuerySystemInformation(SystemPageFileInformation): no ntdll call, no undocumented structure, and
// "?:\pagefile.sys" (system managed) is mapped to the system volume, where the system puts it. A page file
// set on a removable disk is not possible (Windows refuses), so the registry cannot hide one on a card.
func systemRoleVolumes() (WinRoleVolumes, error) {
	rv := WinRoleVolumes{}
	add := func(r Role, path string) error {
		v, err := volumeOfPath(path)
		if err != nil {
			return err
		}
		rv[r] = append(rv[r], v)
		return nil
	}
	wd, err := windows.GetWindowsDirectory()
	if err != nil {
		return nil, fmt.Errorf("cannot find the Windows directory: %w", err)
	}
	sd, err := windows.GetSystemDirectory()
	if err != nil {
		return nil, fmt.Errorf("cannot find the system directory: %w", err)
	}
	if err := add(RoleBoot, wd); err != nil {
		return nil, err
	}
	if err := add(RoleSystem, sd); err != nil {
		return nil, err
	}
	if drv := os.Getenv("SystemDrive"); len(drv) == 2 && drv[1] == ':' {
		if err := add(RoleSystem, drv+`\`); err != nil {
			return nil, err
		}
	}
	files, err := pagingFiles()
	if err != nil {
		return nil, err
	}
	for _, pf := range files {
		switch {
		case pf.Auto:
			err = add(RolePagefile, sd)
		case pf.Volume != "":
			rv[RolePagefile] = append(rv[RolePagefile], pf.Volume)
		default:
			// a letter that no longer exists holds no page file
			if name, lerr := letterVolume(pf.Letter); lerr == nil {
				rv[RolePagefile] = append(rv[RolePagefile], name)
			} else if c := errno0(lerr); c != uint32(windows.ERROR_FILE_NOT_FOUND) && c != uint32(windows.ERROR_PATH_NOT_FOUND) {
				err = fmt.Errorf("cannot find the volume of the page file on %s: %w", pf.Letter, lerr)
			}
		}
		if err != nil {
			return nil, err
		}
	}
	if dump, err := dedicatedDump(); err != nil {
		return nil, err
	} else if dump != "" {
		if err := add(RoleCrashdump, dump); err != nil {
			return nil, err
		}
	}
	return rv, nil
}

// letterVolume is the GUID name of the volume mounted on a drive letter ("C:").
func letterVolume(letter string) (string, error) {
	in, err := windows.UTF16PtrFromString(letter + `\`)
	if err != nil {
		return "", err
	}
	name := make([]uint16, 64)
	if err := windows.GetVolumeNameForVolumeMountPoint(in, &name[0], uint32(len(name))); err != nil {
		return "", err
	}
	return windows.UTF16ToString(name), nil
}

func pagingFiles() ([]WinPagefile, error) {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management`, registry.QUERY_VALUE)
	if err != nil {
		return nil, fmt.Errorf("cannot read the page file settings: %w", err)
	}
	defer k.Close()
	entries, _, err := k.GetStringsValue("PagingFiles")
	if err != nil {
		if errors.Is(err, registry.ErrNotExist) {
			return nil, nil
		}
		return nil, fmt.Errorf("cannot read the page file settings: %w", err)
	}
	return ParsePagingFiles(entries)
}

func dedicatedDump() (string, error) {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SYSTEM\CurrentControlSet\Control\CrashControl`, registry.QUERY_VALUE)
	if err != nil {
		if errors.Is(err, registry.ErrNotExist) {
			return "", nil
		}
		return "", fmt.Errorf("cannot read the crash dump settings: %w", err)
	}
	defer k.Close()
	s, _, err := k.GetStringValue("DedicatedDumpFile")
	if err != nil {
		if errors.Is(err, registry.ErrNotExist) {
			return "", nil
		}
		return "", fmt.Errorf("cannot read the crash dump settings: %w", err)
	}
	if s == "" {
		return "", nil
	}
	if e, err := registry.ExpandString(s); err == nil {
		s = e
	}
	return s, nil
}
