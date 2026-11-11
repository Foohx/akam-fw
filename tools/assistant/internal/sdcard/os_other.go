//go:build !linux

package sdcard

import (
	"errors"
	"os"
)

func osUnmount(point string) error {
	return errors.New("SD card preparation is Linux only in this phase")
}

func osRescan(f *os.File) {}
