// Package image assembles the Akam APP image natively: the port of src/ak_rtsp/build_firmware.sh in kit
// mode (T11.16). It takes the stock APP partition of the user's backup and the verified kit, applies the
// additions, removals and patches IN MEMORY on the tree read by internal/squashfs (nothing is extracted to
// the host file system, so modes, symlinks and ownership survive on Windows and macOS), writes the squashfs
// with the writer of T11.14, pads it with 0xFF to the partition size and re-reads its own output before
// returning. It never runs anything from the kit and never talks to a camera.
//
// EXPERIMENTAL: until the human gate of spec 11 (4.8 step 4) is recorded, the bash builder stays the
// default on Linux; this builder is opt-in (--builder native).
package image

import (
	"bufio"
	_ "embed"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// PartitionSize is the size of the APP partition (mtd7) and of the final image.
const PartitionSize = 5242880

// imageFilesTxt is a copy of tools/release/image_files.txt, the single list of the Akam files of the image
// (a test fails when the two differ).
//
//go:embed image_files.txt
var imageFilesTxt string

type listEntry struct {
	kind    string // file | link
	a       string // file: the mode; link: the target
	kitPath string
	imgPath string
	source  string
}

func parseList() []listEntry {
	var out []listEntry
	sc := bufio.NewScanner(strings.NewReader(imageFilesTxt))
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		f := append(strings.Fields(l), "", "", "", "", "")
		out = append(out, listEntry{kind: f[0], a: f[1], kitPath: f[2], imgPath: f[3], source: f[4]})
	}
	return out
}

// licenseNames are the licence files of local/akam taken from the kit's licenses/ folder.
var licenseNames = []string{"LICENSE.akam", "THIRD_PARTY_NOTICES.md", "LICENSE.dropbear", "LICENSE.musl", "LICENSE.zig",
	"LICENSE.smolrtsp", "LICENSE.slice99", "LICENSE.metalang99", "LICENSE.datatype99", "LICENSE.interface99"}

// maxKitFile caps one kit file read into memory (the largest is dropbearmulti, a few hundred KiB).
const maxKitFile = 32 << 20

// Kit is the part of a verified kit the image needs, held in memory.
type Kit struct {
	files map[string][]byte
	// Manifest is the key=value part of MANIFEST.txt (first occurrence of each key).
	Manifest map[string]string
}

// requiredKitPaths are the files the assembly reads or insists on (build_firmware.sh dies without them).
func requiredKitPaths() []string {
	p := []string{"MANIFEST.txt", "supported_hardware.txt", "bin/ak_rtsp", "bin/dropbearmulti"}
	for _, e := range parseList() {
		if e.kind == "file" {
			p = append(p, e.kitPath)
		}
	}
	for _, n := range licenseNames {
		p = append(p, "licenses/"+n)
	}
	return p
}

// NewKit builds a Kit from kit-relative paths ("bin/ak_rtsp", "overlay/sbin/akam_ssh.sh", ...). A path with
// empty content still counts for the "unknown overlay file" check.
func NewKit(files map[string][]byte) (*Kit, error) {
	k := &Kit{files: files, Manifest: map[string]string{}}
	for _, p := range requiredKitPaths() {
		if _, ok := files[p]; !ok {
			if strings.HasPrefix(p, "patches/") || strings.HasPrefix(p, "overlay/") || strings.HasPrefix(p, "scripts/") {
				return nil, fmt.Errorf("image file source %s missing", p)
			}
			return nil, fmt.Errorf("the kit has no %s", p)
		}
	}
	// A kit newer than this program could carry overlay files the list does not know: they would be
	// silently left out of the image.
	listed := map[string]bool{}
	for _, e := range parseList() {
		listed[e.kitPath] = true
	}
	var extra []string
	for p := range files {
		if (strings.HasPrefix(p, "overlay/") || strings.HasPrefix(p, "patches/")) && !listed[p] {
			extra = append(extra, p)
		}
	}
	sort.Strings(extra)
	if len(extra) > 0 {
		return nil, fmt.Errorf("the kit has files this program does not install (kit newer than the program? update Akam): %s", strings.Join(extra, " "))
	}
	gates := 0
	for _, l := range strings.Split(string(files["MANIFEST.txt"]), "\n") {
		if key, v, ok := strings.Cut(l, "="); ok {
			if key == "time64_gate" {
				gates++
			}
			if _, dup := k.Manifest[key]; !dup {
				k.Manifest[key] = v
			}
		}
	}
	// The camera's kernel acknowledges musl's time64 clock syscalls without executing them: the machine-code
	// gate (tools/build/check_time32.sh, needs gdb) cannot be replayed in process, so the verdict the kit
	// builder recorded in the hashed MANIFEST is REQUIRED, exactly as verify_kit.sh does.
	if gates != 1 || k.Manifest["time64_gate"] != "pass" {
		return nil, errors.New("the kit MANIFEST.txt does not carry exactly one time64_gate=pass: its binaries are not cleared for the camera's kernel. Refusing")
	}
	return k, nil
}

// LoadKit reads the kit directory that the verification (internal/kit) accepted. It does not verify it.
func LoadKit(dir string) (*Kit, error) {
	files := map[string][]byte{}
	for _, p := range requiredKitPaths() {
		b, err := readSmall(filepath.Join(dir, filepath.FromSlash(p)))
		if err != nil {
			if os.IsNotExist(err) {
				if strings.HasPrefix(p, "patches/") || strings.HasPrefix(p, "overlay/") || strings.HasPrefix(p, "scripts/") {
					return nil, fmt.Errorf("image file source %s missing", p)
				}
				return nil, fmt.Errorf("the kit has no %s", p)
			}
			return nil, fmt.Errorf("kit file %s: %w", p, err)
		}
		files[p] = b
	}
	for _, top := range []string{"overlay", "patches"} {
		_ = filepath.WalkDir(filepath.Join(dir, top), func(p string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			if rel, e := filepath.Rel(dir, p); e == nil {
				if rel = filepath.ToSlash(rel); files[rel] == nil {
					files[rel] = []byte{} // known by name only: NewKit refuses it unless the list has it
				}
			}
			return nil
		})
	}
	// The link entries of the list (overlay/bin/dropbear...) are links or copies in the kit: never read.
	for _, e := range parseList() {
		if e.kind == "link" {
			delete(files, e.kitPath)
		}
	}
	return NewKit(files)
}

func readSmall(p string) ([]byte, error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() {
		return nil, fmt.Errorf("not a regular file")
	}
	b, err := io.ReadAll(io.LimitReader(f, maxKitFile+1))
	if err != nil {
		return nil, err
	}
	if len(b) > maxKitFile {
		return nil, fmt.Errorf("larger than %d bytes", maxKitFile)
	}
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// File returns a kit file (nil when absent).
func (k *Kit) File(p string) []byte { return k.files[p] }

// Version, Commit and Grade are the MANIFEST values the report shows.
func (k *Kit) Version() string { return k.Manifest["akam_version"] }
func (k *Kit) Commit() string  { return k.Manifest["source_commit"] }
func (k *Kit) Grade() string   { return k.Manifest["kit_grade"] }
