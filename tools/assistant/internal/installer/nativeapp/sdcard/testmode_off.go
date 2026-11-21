//go:build !akamtest

package sdcard

// imageFileAllowed is false in a release build: the helper only ever opens a block device node that
// passed the guards. The image-file mode used by the tests does not exist in the binary.
const imageFileAllowed = false
