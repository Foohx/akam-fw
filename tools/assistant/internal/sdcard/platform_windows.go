//go:build windows

package sdcard

import (
	"errors"
	"os"
)

// ErrUnsupported is what the card layer answers for a step this build does not have.
var ErrUnsupported = errors.New("SD card preparation is not supported on this build")

// osRescan is only used for image files (tests); a physical drive is rescanned by winDisk.
func osRescan(f *os.File) {}

func newPlatform() platform {
	return platform{Inventory: winInventory{}, Opener: newWinOpener(), Transport: newTransport()}
}
