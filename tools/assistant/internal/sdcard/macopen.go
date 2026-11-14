package sdcard

import (
	"context"
	"fmt"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/blockio"
)

// The macOS Opener (specs/installer.md): `diskutil unmountDisk force /dev/diskN`, then the raw node /dev/rdiskN
// opened read-write through blockio.Aligned. The OS side (the open itself) is injected so the order of the
// steps is tested on Linux; opener_darwin.go supplies the real one.

// macRawFile is an opened raw node.
type macRawFile interface {
	ReadAt(p []byte, off int64) (int, error)
	WriteAt(p []byte, off int64) (int, error)
	Sync() error
	Close() error
	// DevNum is "major:minor" of the opened node (fstat of the descriptor, not of the path).
	DevNum() (string, error)
}

type macOpener struct {
	run     Runner
	openRaw func(path string) (macRawFile, error)

	dev       string // /dev/diskN given to CheckNode
	unmounted bool
}

// CheckNode accepts /dev/diskN only (slices and /dev/rdisk* are refused: the opener maps to the raw node itself).
func (o *macOpener) CheckNode(dev string) error {
	if !macDiskRe.MatchString(dev) {
		return fmt.Errorf("refusing: %s is not a /dev/diskN node", dev)
	}
	o.dev, o.unmounted = dev, false
	return nil
}

// Unmount unmounts the whole disk once, whatever mount point it is called for: diskutil works per disk.
func (o *macOpener) Unmount(point string) error {
	if o.dev == "" {
		return fmt.Errorf("refusing: unmount before the node was checked")
	}
	if o.unmounted {
		return nil
	}
	if _, err := o.run.Run(context.Background(), diskutilPath, "unmountDisk", "force", o.dev); err != nil {
		return err
	}
	o.unmounted = true
	return nil
}

// rawPath maps /dev/diskN to /dev/rdiskN.
func rawPath(dev string) string { return "/dev/r" + strings.TrimPrefix(dev, "/dev/") }

// sameDevNum compares a "major:minor" with the one that ends the identity token.
func sameDevNum(got, ident string) bool {
	want := ident[strings.LastIndex(ident, "|")+1:]
	return got != "" && got == want
}

func (o *macOpener) Open(dev string, c *Candidate) (Disk, error) {
	if dev != o.dev || c.Dev != dev {
		return nil, fmt.Errorf("refusing: %s was not the checked node", dev)
	}
	f, err := o.openRaw(rawPath(dev))
	if err != nil {
		return nil, err
	}
	// what was opened must be the disk of the identity token (the path could have been swapped since the check)
	got, err := f.DevNum()
	if err != nil || !sameDevNum(got, c.Ident) {
		f.Close()
		return nil, fmt.Errorf("refusing: the opened device %s is %q, not the disk that was confirmed (nothing was written)", rawPath(dev), got)
	}
	a, err := blockio.New(macRawAdapter{f})
	if err != nil {
		f.Close()
		return nil, err
	}
	return macDisk{Aligned: a, f: f}, nil
}

// macRawAdapter gives the raw node the logical sector size the policy guarantees.
type macRawAdapter struct{ macRawFile }

func (macRawAdapter) SectorSize() int { return 512 }

type macDisk struct {
	*blockio.Aligned
	f macRawFile
}

func (d macDisk) Close() error { return d.f.Close() }

// Rescan: the OS re-probes the disk when the raw node is closed; the raw node itself is uncached.
func (d macDisk) Rescan() {}
