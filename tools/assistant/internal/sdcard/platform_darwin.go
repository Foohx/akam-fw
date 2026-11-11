//go:build darwin

package sdcard

import "os"

func osRescan(f *os.File) {}

func newPlatform() platform {
	run := execRunner{}
	return platform{
		Inventory: newMacInventory(run, devNumDarwin),
		Opener:    &macOpener{run: run, openRaw: openRawDarwin},
		Transport: newTransport(),
	}
}
