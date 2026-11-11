//go:build linux

package sdcard

func newPlatform() platform {
	return platform{Inventory: RealSys(), Opener: linuxOpener{}, Transport: newTransport()}
}
