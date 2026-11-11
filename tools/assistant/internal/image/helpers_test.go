package image

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

const testEpoch = 1700000000

func repoRoot(t testing.TB) string {
	t.Helper()
	d, err := filepath.Abs(filepath.Join("..", "..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return d
}

func have(tools ...string) bool {
	for _, t := range tools {
		if _, err := exec.LookPath(t); err != nil {
			return false
		}
	}
	return true
}

// nodeFromDir reads a fixture folder into a tree (symlinks kept, owner root). Tests only.
func nodeFromDir(t testing.TB, dir string) *squashfs.Node {
	t.Helper()
	var rec func(p string, n *squashfs.Node)
	rec = func(p string, n *squashfs.Node) {
		ents, err := os.ReadDir(p)
		if err != nil {
			t.Fatal(err)
		}
		for _, e := range ents {
			fp := filepath.Join(p, e.Name())
			st, err := os.Lstat(fp)
			if err != nil {
				t.Fatal(err)
			}
			switch {
			case st.Mode()&os.ModeSymlink != 0:
				tg, _ := os.Readlink(fp)
				n.Add(squashfs.NewSymlink(e.Name(), tg))
			case st.IsDir():
				d := squashfs.NewDir(e.Name(), uint16(st.Mode().Perm()))
				n.Add(d)
				rec(fp, d)
			default:
				b, err := os.ReadFile(fp)
				if err != nil {
					t.Fatal(err)
				}
				n.Add(squashfs.NewFile(e.Name(), uint16(st.Mode().Perm()), b))
			}
		}
	}
	root := squashfs.NewDir("", 0o755)
	rec(dir, root)
	return root
}

// stockImage makes a stock-like APP partition (xz, 128 KiB, padded with 0xFF) from a tree.
func stockImage(t testing.TB, root *squashfs.Node) []byte {
	t.Helper()
	sq, _, err := squashfs.Build(root, squashfs.Options{Time: 1, AllTime: true, MaxSize: PartitionSize})
	if err != nil {
		t.Fatal(err)
	}
	img := bytes.Repeat([]byte{0xFF}, PartitionSize)
	copy(img, sq)
	return img
}

// stubKitFiles is an in-memory kit: stand-in binaries and licences, the overlay and scripts of the repository.
func stubKitFiles(t testing.TB, hw string) map[string][]byte {
	t.Helper()
	root := repoRoot(t)
	f := map[string][]byte{
		"bin/ak_rtsp":            []byte("#!/bin/sh\necho stub ak_rtsp\n"),
		"bin/dropbearmulti":      []byte("#!/bin/sh\necho stub dropbearmulti\n"),
		"MANIFEST.txt":           []byte("kit_format=1\nkit_grade=test\nakam_version=test1\nsource_commit=0123456789ab\ntime64_gate=pass\n"),
		"supported_hardware.txt": []byte(hw),
	}
	for _, n := range licenseNames {
		f["licenses/"+n] = []byte(n + " stub licence\n")
	}
	for _, e := range parseList() {
		if e.kind != "file" {
			continue
		}
		b, err := os.ReadFile(filepath.Join(root, e.source))
		if err != nil {
			t.Fatal(err)
		}
		f[e.kitPath] = b
	}
	return f
}

func hwFor(sha string) string {
	return "supported_hardware_format=1\nvalidated|Test camera|soc|Linux-test|8 partitions; mtd7=APP 00500000|squashfs xz block=131072 bcj=none|" + sha + "|1\n"
}

// listText renders a listing with the canonical String form, optionally blanking size and sha of some paths.
func listText(l []squashfs.ListEntry, blank map[string]bool) string {
	var out []string
	for _, e := range l {
		if blank[e.Path] {
			e.Size, e.SHA256 = 0, ""
			if e.SHA256 == "" && e.Type == squashfs.TypeFile {
				e.SHA256 = "(kit)"
			}
		}
		out = append(out, e.String())
	}
	sort.Strings(out)
	return strings.Join(out, "\n") + "\n"
}

// kitDerived is the set of paths of the image whose content comes from the kit (golden files must not depend
// on the templates of the repository).
func kitDerived() map[string]bool {
	m := map[string]bool{"/bin/ak_rtsp": true, "/bin/dropbearmulti": true, "/local/akam/MANIFEST.txt": true}
	for _, e := range parseList() {
		if e.imgPath != "-" {
			m["/"+e.imgPath] = true
		}
	}
	for _, n := range licenseNames {
		m["/local/akam/"+n] = true
	}
	return m
}
