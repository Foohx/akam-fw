package squashfs

import (
	"errors"
	"fmt"
)

var (
	// ErrCorrupt reports an image that is malformed or inconsistent (including truncated ones).
	ErrCorrupt = errors.New("squashfs: corrupt image")
	// ErrUnsupported reports a valid image that uses a feature the reader does not implement.
	ErrUnsupported = errors.New("squashfs: unsupported feature")
	// ErrLimit reports that a configured cap (Limits) was exceeded.
	ErrLimit = errors.New("squashfs: limit exceeded")
	// ErrNotDir is returned when a path component is not a directory.
	ErrNotDir = errors.New("squashfs: not a directory")
	// ErrNotSymlink is returned by Readlink on something that is not a symbolic link.
	ErrNotSymlink = errors.New("squashfs: not a symbolic link")
	// ErrNotRegular is returned by ReadFile and OpenFile on something that is not a regular file.
	ErrNotRegular = errors.New("squashfs: not a regular file")
)

func corrupt(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrCorrupt, fmt.Sprintf(format, a...))
}

func unsupported(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrUnsupported, fmt.Sprintf(format, a...))
}

func limited(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrLimit, fmt.Sprintf(format, a...))
}
