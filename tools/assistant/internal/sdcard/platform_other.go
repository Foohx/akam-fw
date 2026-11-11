//go:build !linux && !darwin

package sdcard

import (
	"errors"
	"os"
)

// ErrUnsupported is what the card layer answers on an OS whose disk layer is not built in yet.
var ErrUnsupported = errors.New("SD card preparation is not supported on this build")

func osRescan(f *os.File) {}

func newPlatform() platform {
	return platform{Inventory: unsupportedInventory{}, Opener: unsupportedOpener{}, Transport: newTransport()}
}

type unsupportedInventory struct{}

func (unsupportedInventory) Devices() []string                      { return nil }
func (unsupportedInventory) Inspect(string, Context) (Facts, error) { return Facts{}, ErrUnsupported }

type unsupportedOpener struct{}

func (unsupportedOpener) CheckNode(string) error                { return ErrUnsupported }
func (unsupportedOpener) Unmount(string) error                  { return ErrUnsupported }
func (unsupportedOpener) Open(string, *Candidate) (Disk, error) { return nil, ErrUnsupported }
