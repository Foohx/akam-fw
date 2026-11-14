package sdcard

import (
	"io"
	"os"
)

// Disk is an opened card: what the FAT32 and MBR writers, the read-back and the wipe need.
type Disk interface {
	Device // ReadAt, WriteAt, Sync
	io.Closer
	// Rescan asks the OS to re-read the partition table and drops cached blocks (best effort).
	Rescan()
}

// Opener is the OS side of the privileged steps. The helper calls it in a fixed order: CheckNode, Unmount for
// every mount point, (the caller re-checks the identity), Open, then writes, Sync, Rescan, reads, Close.
type Opener interface {
	// CheckNode refuses a path that is not the raw node of a disk.
	CheckNode(dev string) error
	// Unmount unmounts one mount point of the disk.
	Unmount(point string) error
	// Open opens the raw node for writing, exclusively. c is the freshly re-checked candidate: the opener
	// verifies that what it opened is that disk.
	Open(dev string, c *Candidate) (Disk, error)
}

// fileDisk is a Disk over an *os.File (a block device node on Linux, a regular file in image-file mode).
type fileDisk struct {
	f      *os.File
	rescan func(f *os.File)
}

func (d fileDisk) ReadAt(p []byte, off int64) (int, error)  { return d.f.ReadAt(p, off) }
func (d fileDisk) WriteAt(p []byte, off int64) (int, error) { return d.f.WriteAt(p, off) }
func (d fileDisk) Sync() error                              { return d.f.Sync() }
func (d fileDisk) Close() error                             { return d.f.Close() }
func (d fileDisk) Rescan() {
	if d.rescan != nil {
		d.rescan(d.f)
		return
	}
	osRescan(d.f)
}
