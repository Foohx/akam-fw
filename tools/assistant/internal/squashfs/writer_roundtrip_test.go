package squashfs

import (
	"bytes"
	"os"
	"os/exec"
	"strings"
	"testing"
)

// maskDirSizes returns the lines of an `unsquashfs -lln` output with the size of directories (the length of
// their stored listing, which depends on the writer) replaced, so that two images of one tree compare equal.
func maskDirSizes(out string) string {
	lines := strings.Split(strings.TrimRight(out, "\n"), "\n")
	for i, ln := range lines {
		if g := realLsRe.FindStringSubmatch(ln); g != nil && g[1][0] == 'd' {
			lines[i] = g[1] + " " + g[2] + "/" + g[3] + " * " + g[5] + " " + g[6]
		}
	}
	return strings.Join(lines, "\n")
}

// TestRoundTripRealImage is the opt-in check of the writer on a real image (the private APP of a camera): it
// reads the image with the reader, writes it again with the writer into a temporary file, and compares the
// two images (reader listing, `unsquashfs -lln` when installed, hashes of every file). Only a one line
// summary is printed. It never keeps the rewritten image.
//
//	AKAM_SQFS_ROUNDTRIP=/path/to/image go test -count=1 -v -run TestRoundTripRealImage ./internal/squashfs
//
// (from tools/assistant). The image may carry trailing padding after its bytes_used.
func TestRoundTripRealImage(t *testing.T) {
	img := os.Getenv("AKAM_SQFS_ROUNDTRIP")
	if img == "" {
		t.Skip("AKAM_SQFS_ROUNDTRIP is not set")
	}
	raw, err := os.ReadFile(img)
	if err != nil {
		t.Fatalf("cannot read the image: %v", err)
	}
	f, err := OpenLimits(bytes.NewReader(raw), int64(len(raw)), Limits{MaxImageSize: int64(len(raw))})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if err := f.Verify(); err != nil {
		t.Fatalf("Verify: %v", err)
	}
	want, err := f.Listing()
	if err != nil {
		t.Fatal(err)
	}
	tree, err := f.Tree()
	if err != nil {
		t.Fatalf("Tree: %v", err)
	}
	out, st, err := Build(tree, Options{Time: f.Info().MkfsTime})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	tmp := writeTemp(t, "rewritten.sqfs", out)
	g := openImage(t, out)
	if err := g.Verify(); err != nil {
		t.Fatalf("Verify of the rewritten image: %v", err)
	}
	got, err := g.Listing()
	if err != nil {
		t.Fatal(err)
	}
	diffListings(t, "rewritten listing", got, want)
	hashes := 0
	for _, e := range want {
		if e.Type == TypeFile {
			hashes++
		}
	}
	lln := "unsquashfs not installed"
	if _, err := exec.LookPath("unsquashfs"); err == nil {
		a := unsquashfsText(t, "-lln", "-full-precision", "-UTC", img)
		b := unsquashfsText(t, "-lln", "-full-precision", "-UTC", tmp)
		if maskDirSizes(a) != maskDirSizes(b) {
			t.Errorf("unsquashfs -lln differs between the original and the rewritten image")
		}
		lln = "unsquashfs -lln equal"
	}
	if st.Size > 5242880 {
		t.Errorf("rewritten image of %d bytes exceeds 5242880", st.Size)
	}
	t.Logf("roundtrip: original %d bytes (used %d), rewritten %d bytes, %d entries and %d file hashes equal, %s",
		len(raw), f.Info().BytesUsed, st.Size, len(got), hashes, lln)
}
