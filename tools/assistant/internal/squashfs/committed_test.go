package squashfs

import (
	"bytes"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var updateGolden = flag.Bool("update-golden", false, "rewrite testdata/*.listing from the reader (after checking it against unsquashfs)")

func testdataImages() ([]string, error) { return filepath.Glob(filepath.Join("testdata", "*.sqfs")) }

func readTestdata() ([][]byte, error) {
	names, err := testdataImages()
	if err != nil {
		return nil, err
	}
	var out [][]byte
	for _, n := range names {
		b, err := os.ReadFile(n)
		if err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, nil
}

// TestCommittedImages needs no external tool: it compares the reader with the committed golden listings.
func TestCommittedImages(t *testing.T) {
	names, err := testdataImages()
	if err != nil || len(names) < 3 {
		t.Fatalf("committed images: %v %v", names, err)
	}
	for _, name := range names {
		t.Run(filepath.Base(name), func(t *testing.T) {
			raw, err := os.ReadFile(name)
			if err != nil {
				t.Fatal(err)
			}
			f, err := Open(bytes.NewReader(raw), int64(len(raw)))
			if err != nil {
				t.Fatal(err)
			}
			if err := f.Verify(); err != nil {
				t.Fatal(err)
			}
			var got bytes.Buffer
			if err := f.WriteListing(&got); err != nil {
				t.Fatal(err)
			}
			golden := strings.TrimSuffix(name, ".sqfs") + ".listing"
			if *updateGolden {
				if _, err := compareIfTools(t, name, f); err != nil {
					t.Skipf("cannot check against unsquashfs, golden not rewritten: %v", err)
				}
				if err := os.WriteFile(golden, got.Bytes(), 0o644); err != nil {
					t.Fatal(err)
				}
			}
			want, err := os.ReadFile(golden)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got.Bytes(), want) {
				t.Fatalf("listing differs from %s:\n%s", golden, got.String())
			}
			if info := f.Info(); info.Inodes < 300 {
				t.Fatalf("unexpected fixture: %+v", info)
			}
			// the tool comparison also runs on the committed images when it is there
			if _, err := compareIfTools(t, name, f); err != nil {
				t.Logf("unsquashfs comparison skipped: %v", err)
			}
		})
	}
}

// compareIfTools runs the comparison with unsquashfs when it is installed.
func compareIfTools(t *testing.T, img string, f *FS) (bool, error) {
	t.Helper()
	if err := lookTools(); err != nil {
		return false, err
	}
	compareToUnsquashfs(t, img, f)
	return true, nil
}
