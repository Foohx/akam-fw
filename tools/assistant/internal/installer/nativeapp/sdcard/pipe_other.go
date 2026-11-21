//go:build !windows

package sdcard

import (
	"errors"
	"time"
)

// The pipe exists on Windows only: elsewhere `sd-helper --pipe` is refused before it gets here.
func dialPipeOS(string, time.Duration) (uacConn, error) {
	return nil, errors.New("named pipes are only used on Windows")
}
