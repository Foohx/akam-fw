//go:build windows

package sdcard

import (
	"fmt"
	"io"
	"sync"
	"time"

	"golang.org/x/sys/windows"

	"github.com/Foohx/akam-fw/tools/assistant/internal/blockio"
)

// winOpener implements the privileged steps of specs/installer.md (helper only, elevated):
//
//	CheckNode  the node is \\.\PhysicalDriveN
//	Unmount    per volume path: open the volume, check that every extent is on disk N, FSCTL_LOCK_VOLUME,
//	           FSCTL_DISMOUNT_VOLUME; the handle stays open until Close so that the system cannot remount
//	Open       \\.\PhysicalDriveN write-through, compare identities, FSCTL_ALLOW_EXTENDED_DASD_IO
//	Rescan     FlushFileBuffers, IOCTL_DISK_UPDATE_PROPERTIES, then reads through a second uncached handle
//	Close      the handles of the disk, then the volume locks last
type winOpener struct {
	mu   sync.Mutex
	disk int
	have bool
	vols map[string]windows.Handle // locked, dismounted volumes by GUID name
}

func newWinOpener() *winOpener { return &winOpener{vols: map[string]windows.Handle{}} }

func (o *winOpener) CheckNode(dev string) error {
	n, ok := WinPhysicalNode(dev)
	if !ok {
		return fmt.Errorf(`refusing: %s is not a physical drive node like \\.\PhysicalDrive2 (a drive letter or a volume is never written)`, dev)
	}
	o.mu.Lock()
	o.disk, o.have = n, true
	o.mu.Unlock()
	return nil
}

// volumeName resolves a mount point (drive letter root, folder, or the GUID path itself) to the volume's GUID name.
func volumeName(point string) (string, error) {
	if m := volumeRe.FindStringSubmatch(point); m != nil {
		return m[1] + `\`, nil
	}
	p := point
	if len(p) == 0 || p[len(p)-1] != '\\' {
		p += `\`
	}
	in, err := windows.UTF16PtrFromString(p)
	if err != nil {
		return "", err
	}
	name := make([]uint16, 64)
	if err := windows.GetVolumeNameForVolumeMountPoint(in, &name[0], uint32(len(name))); err != nil {
		return "", err
	}
	return windows.UTF16ToString(name), nil
}

func (o *winOpener) Unmount(point string) error {
	o.mu.Lock()
	defer o.mu.Unlock()
	if !o.have {
		return fmt.Errorf("refusing: no disk was named before %s", point)
	}
	name, err := volumeName(point)
	if err != nil {
		return fmt.Errorf("cannot resolve %s to a volume: %w", point, err)
	}
	key := volKey(name)
	if _, held := o.vols[key]; held {
		return nil // another path of a volume that is already locked
	}
	h, err := openPath(trimSlash(name), windows.GENERIC_READ|windows.GENERIC_WRITE, 0)
	if err != nil {
		return fmt.Errorf("cannot open %s (in use? close Explorer windows and programs using the card): %w", point, err)
	}
	// the volume must live on this disk and on no other
	raw, err := ioctlGrow(h, ioctlVolumeGetVolumeDiskExtents, nil, DiskExtentsSize(4))
	if err == nil {
		var ext []DiskExtent
		if ext, err = ParseDiskExtents(raw); err == nil {
			if len(ext) == 0 {
				err = fmt.Errorf("refusing: volume %s reports no disk, so it is not shown to be on disk %d", point, o.disk)
			}
			for _, e := range ext {
				if int(e.Disk) != o.disk {
					err = fmt.Errorf("refusing: volume %s is also on disk %d", point, e.Disk)
				}
			}
		}
	}
	if err != nil {
		windows.CloseHandle(h)
		return err
	}
	var lerr error
	for try := 0; try < 4; try++ { // an antivirus scan or an indexer holds a volume for a moment
		if _, lerr = ioctl(h, fsctlLockVolume, nil, nil); lerr == nil {
			break
		}
		time.Sleep(400 * time.Millisecond)
	}
	if lerr != nil {
		windows.CloseHandle(h)
		return fmt.Errorf("cannot lock %s: %v (something is using the card: close Explorer windows and programs using it; a BitLocker To Go card is not supported)", point, lerr)
	}
	if _, err := ioctl(h, fsctlDismountVolume, nil, nil); err != nil {
		ioctl(h, fsctlUnlockVolume, nil, nil)
		windows.CloseHandle(h)
		return fmt.Errorf("cannot dismount %s: %v", point, err)
	}
	o.vols[key] = h
	return nil
}

func trimSlash(s string) string {
	for len(s) > 0 && s[len(s)-1] == '\\' {
		s = s[:len(s)-1]
	}
	return s
}

// release unlocks and closes every held volume.
func (o *winOpener) release() error {
	o.mu.Lock()
	defer o.mu.Unlock()
	var first error
	for k, h := range o.vols {
		if _, err := ioctl(h, fsctlUnlockVolume, nil, nil); err != nil && first == nil {
			first = fmt.Errorf("cannot unlock a volume: %v", err)
		}
		windows.CloseHandle(h)
		delete(o.vols, k)
	}
	return first
}

func (o *winOpener) Open(dev string, c *Candidate) (Disk, error) {
	n, ok := WinPhysicalNode(dev)
	if !ok {
		return nil, fmt.Errorf("refusing: %s is not a physical drive node", dev)
	}
	o.mu.Lock()
	named := o.have && o.disk == n
	o.mu.Unlock()
	if !named {
		return nil, fmt.Errorf("refusing: %s was not the node checked before", dev)
	}
	// open for writing, then read the identity from that very handle: what was opened must be the confirmed disk
	w, err := openRawWrite(n, 512)
	if err != nil {
		o.release()
		return nil, err
	}
	fail := func(err error) (Disk, error) {
		w.Close()
		o.release()
		return nil, err
	}
	f, err := diskFacts(w.h, n)
	if err != nil {
		return fail(fmt.Errorf("refusing: cannot read the opened disk: %w", err))
	}
	if Ident(f) != c.Ident {
		return fail(fmt.Errorf("refusing: the opened disk is not the one confirmed (nothing was written): %w", ErrIdentChanged))
	}
	if f.SectorSize != 512 {
		return fail(fmt.Errorf("refusing: %d-byte sectors", f.SectorSize))
	}
	// every volume that is on the disk now must be one of the volumes locked and dismounted above (a volume that
	// appeared since the check would be written under); an unreadable volume list refuses
	vols, err := enumVolumes()
	if err != nil {
		return fail(fmt.Errorf("refusing: cannot list the volumes of the opened disk: %w", err))
	}
	o.mu.Lock()
	for _, v := range vols {
		if _, held := o.vols[volKey(v.Name)]; v.onDisk(n) && !held {
			o.mu.Unlock()
			return fail(fmt.Errorf("refusing: volume %s is on the disk but was not locked", v.Name))
		}
	}
	o.mu.Unlock()
	d, err := newWinDisk(winDevOps{
		Write:            w,
		UpdateProperties: w.updateProperties,
		OpenVerify: func() (blockio.Raw, io.Closer, error) {
			v, err := openRawVerify(n, f.SectorSize)
			if err != nil {
				return nil, nil, err
			}
			return v, v, nil
		},
		CloseWrite: w.Close,
		Release:    o.release,
	})
	if err != nil {
		return fail(err)
	}
	return d, nil
}
