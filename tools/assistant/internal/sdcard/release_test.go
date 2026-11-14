//go:build !akamtest

package sdcard

import "testing"

// A release build has no image-file mode: the helper refuses a regular file even when asked.
func TestReleaseBuildHasNoImageFileMode(t *testing.T) {
	if imageFileAllowed {
		t.Fatal("image-file mode is compiled into a release build")
	}
	f, p := sparse(t, 192<<20)
	_ = f
	r := Helper{}.Handle(Request{Op: OpWipe, Device: p, Token: ImageToken(192 << 20), ImageFile: true})
	if r.OK || r.Error == "" {
		t.Errorf("got %+v", r)
	}
}
