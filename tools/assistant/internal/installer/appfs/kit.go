// Package appfs is the installer of the models whose firmware kind is "cardless-appfs" (the LSC Rotatable 2K):
// Akam runs from the camera's flash with no SD card. The appfs partition (mtd3, squashfs xz, 0x490000 bytes) is
// replaced by the stock appfs of the USER'S OWN backup plus the Akam payload of the kit.
//
// It is the Go port of two shell tools of the model: appfs/build_appfs.sh (Build) and tools/flash_appfs.sh (Flash).
// The kit holds only Akam code, never a vendor file: the vendor part of the image comes from the user's backup.
//
// The recovery of every state is the Akam SD card: inserted at power-up it overrides the flashed boot (the stock
// init.sh branches for the debug and factory modes are left untouched). A half-written mtd3 is the one state the
// card cannot recover (UART or a CH341A programmer is needed): Flash therefore refuses to write without the
// user's explicit recovery acknowledgement and a typed confirmation bound to the image.
package appfs

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// Kind is the registry firmware kind this installer serves (models.KindCardlessAppfs).
const Kind = "cardless-appfs"

// Kit paths. The kit of the 2K is a directory (or the tarball unpacked by internal/kit after its signature check):
//
//	MANIFEST.txt      key=value: model, kind=cardless-appfs, akam_version, source_commit, kit_grade, time64_gate=pass
//	SHA256SUMS        sha256 of every other file (checked here: the kit is refused on any mismatch or extra file)
//	appfs/overlay/*.sh        the Akam scripts of the image (the payload list below)
//	appfs/bin/akamd, akam-sntp, akamd.BUILDINFO, dropbearmulti, dropbearmulti.BUILDINFO, busybox
const (
	pathManifest = "MANIFEST.txt"
	pathSums     = "SHA256SUMS"
	pathOverlay  = "appfs/overlay/"
	pathBin      = "appfs/bin/"
)

// overlayScripts are the shell files of /appfs/akam, in the order build_appfs.sh installs them.
var overlayScripts = []string{"own_app_stub.sh", "own_app_run.sh", "common.sh", "decloud.sh", "start_ssh.sh",
	"akam_net.sh", "akam_api_key.sh", "akam_flash_boot.sh", "akam_ntp.sh", "akam_hostname.sh"}

var binFiles = []string{"akamd", "akamd.BUILDINFO", "akam-sntp", "dropbearmulti", "dropbearmulti.BUILDINFO", "busybox"}

// legacyBinNames maps the names of kit_format 2 to those of kit_format 1 (firmware 0.1.x): the same files, renamed
// when the camera program became akamd on every camera. A format 1 kit is read through this table.
var legacyBinNames = map[string]string{"akamd": "akam2k_rtsp", "akamd.BUILDINFO": "akam2k_rtsp.BUILDINFO", "akam-sntp": "akam2k_sntp"}

// BusyBoxName and BusyBoxSHA256 pin the static busybox of the card (BB_SHA256 of build_appfs.sh).
const (
	BusyBoxName   = "busybox-1.36.1-ak3918-armv5te-static"
	BusyBoxSHA256 = "367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290"
)

// cardFiles are the SD card files that are not in appfs/ (optional: a kit without them can build the appfs image but
// not the first-install card). They are all present or all absent. Other optional files: supported_hardware.txt and
// licenses/* (documentation that travels with the binaries).
var cardFiles = []string{"card/yy_debug.ini", "card/akam_boot.sh", "card/akam/enable_local.sh", "card/akam/restore_param.sh"}

func optionalKitFile(p string) bool {
	return p == "supported_hardware.txt" || strings.HasPrefix(p, "licenses/") && !strings.Contains(p[len("licenses/"):], "/")
}

// maxKitFile caps one kit file read into memory.
const maxKitFile = 32 << 20

// RequiredKitPaths lists every file the appfs build reads (the description of the 2K kit, see docs/assistant.md).
func RequiredKitPaths() []string {
	p := []string{pathManifest, pathSums}
	for _, s := range overlayScripts {
		p = append(p, pathOverlay+s)
	}
	for _, b := range binFiles {
		p = append(p, pathBin+b)
	}
	return p
}

// Kit is the verified content of an appfs kit, held in memory.
type Kit struct {
	files    map[string][]byte
	Manifest map[string]string // first occurrence of each key=value line of MANIFEST.txt
}

// KitOption relaxes one check of NewKit and LoadKit.
type KitOption func(*kitOpts)

type kitOpts struct{ allowTest bool }

// AllowTestGrade accepts a kit with kit_grade=test (a development kit built from a dirty tree or with stub inputs).
// Only for a local kit folder the operator explicitly chose to trust (--kit DIR --allow-unsigned): a release kit
// is never test grade, and a downloaded kit never gets this option.
func AllowTestGrade() KitOption { return func(o *kitOpts) { o.allowTest = true } }

// IsTestGrade reports whether the kit says kit_grade=test (or anything but release).
func (k *Kit) IsTestGrade() bool { return k.Manifest["kit_grade"] != "release" }

// NewKit checks and wraps kit files (kit-relative slash paths). It refuses a kit that lacks a required file, has
// an unknown file, does not match its SHA256SUMS, is not of kind cardless-appfs or is not cleared for the camera's
// kernel (exactly one time64_gate=pass in the hashed MANIFEST, as verify_kit.sh requires for the 1080p kit).
func NewKit(files map[string][]byte, opts ...KitOption) (*Kit, error) {
	var o kitOpts
	for _, f := range opts {
		f(&o)
	}
	files = normaliseLegacy(files)
	req := map[string]bool{}
	for _, p := range RequiredKitPaths() {
		req[p] = true
		if _, ok := files[p]; !ok {
			return nil, fmt.Errorf("the appfs kit has no %s", p)
		}
	}
	card := 0
	for _, p := range cardFiles {
		if _, ok := files[p]; ok {
			card++
			req[p] = true
		}
	}
	if card != 0 && card != len(cardFiles) {
		return nil, fmt.Errorf("the appfs kit has only some of the SD card files (%s)", strings.Join(cardFiles, " "))
	}
	var extra []string
	for p := range files {
		if !req[p] && !optionalKitFile(p) {
			extra = append(extra, p)
		}
	}
	sort.Strings(extra)
	if len(extra) > 0 {
		return nil, fmt.Errorf("the appfs kit has files this program does not install (kit newer than the program? update Akam): %s", strings.Join(extra, " "))
	}
	sums, err := parseSums(files[pathSums])
	if err != nil {
		return nil, err
	}
	for p := range files {
		if p == pathSums || (!req[p] && !optionalKitFile(p)) {
			continue
		}
		want, ok := sums[p]
		if !ok {
			return nil, fmt.Errorf("the appfs kit SHA256SUMS does not list %s", p)
		}
		if got := sum(files[p]); got != want {
			return nil, fmt.Errorf("the appfs kit file %s does not match SHA256SUMS", p)
		}
		delete(sums, p)
	}
	for p := range sums {
		return nil, fmt.Errorf("the appfs kit SHA256SUMS lists %s, which is not in the kit", p)
	}
	k := &Kit{files: files, Manifest: map[string]string{}}
	gates := 0
	for _, l := range strings.Split(string(files[pathManifest]), "\n") {
		if key, v, ok := strings.Cut(strings.TrimRight(l, "\r"), "="); ok {
			if key == "time64_gate" {
				gates++
			}
			if _, dup := k.Manifest[key]; !dup {
				k.Manifest[key] = v
			}
		}
	}
	if k.Manifest["kind"] != Kind {
		return nil, fmt.Errorf("the kit MANIFEST.txt says kind=%q, want %s: not an appfs kit", k.Manifest["kind"], Kind)
	}
	if k.IsTestGrade() && !o.allowTest {
		return nil, fmt.Errorf("the kit MANIFEST.txt says kit_grade=%q: a development kit is not a release (built from a dirty tree or with stub inputs). Refusing; only a kit folder you chose yourself with --kit DIR --allow-unsigned can be used", k.Manifest["kit_grade"])
	}
	if gates != 1 || k.Manifest["time64_gate"] != "pass" {
		return nil, errors.New("the kit MANIFEST.txt does not carry exactly one time64_gate=pass: its binaries are not cleared for the camera's kernel. Refusing")
	}
	return k, nil
}

// LoadKit reads a kit directory (already signature-checked by internal/kit when it came from a download) and
// verifies it with NewKit.
func LoadKit(dir string, opts ...KitOption) (*Kit, error) {
	files := map[string][]byte{}
	for _, p := range RequiredKitPaths() {
		b, err := readSmall(filepath.Join(dir, filepath.FromSlash(p)))
		if old, ok := legacyBinNames[strings.TrimPrefix(p, pathBin)]; ok && err != nil && os.IsNotExist(err) && strings.HasPrefix(p, pathBin) {
			b, err = readSmall(filepath.Join(dir, filepath.FromSlash(pathBin+old))) // a kit_format 1 kit
			if err == nil {
				files[pathBin+old] = b // normaliseLegacy renames it
				continue
			}
		}
		if err != nil {
			if os.IsNotExist(err) {
				return nil, fmt.Errorf("the appfs kit has no %s", p)
			}
			return nil, fmt.Errorf("kit file %s: %w", p, err)
		}
		files[p] = b
	}
	// A file under appfs/ that is not required is a kit newer than this program: NewKit refuses it.
	_ = filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if rel, e := filepath.Rel(dir, p); e == nil {
			rel = filepath.ToSlash(rel)
			if files[rel] != nil {
				return nil
			}
			files[rel] = []byte{} // unknown: NewKit refuses it
			if optionalKitFile(rel) || slices.Contains(cardFiles, rel) {
				if b, e := readSmall(p); e == nil {
					files[rel] = b
				}
			}
		}
		return nil
	})
	return NewKit(files, opts...)
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
		return nil, errors.New("not a regular file")
	}
	b, err := io.ReadAll(io.LimitReader(f, maxKitFile+1))
	if err != nil {
		return nil, err
	}
	if len(b) > maxKitFile {
		return nil, fmt.Errorf("larger than %d bytes", maxKitFile)
	}
	return b, nil
}

// normaliseLegacy renames the files of a kit_format 1 kit to the names of format 2 (file map and SHA256SUMS lines), so
// that everything after it deals with one set of names. A kit that already has the new names is returned unchanged.
func normaliseLegacy(files map[string][]byte) map[string][]byte {
	renamed := map[string]string{}
	for nw, old := range legacyBinNames {
		if _, has := files[pathBin+nw]; has {
			continue
		}
		if b, ok := files[pathBin+old]; ok {
			files[pathBin+nw] = b
			delete(files, pathBin+old)
			renamed[pathBin+old] = pathBin + nw
		}
	}
	if len(renamed) > 0 {
		lines := strings.Split(string(files[pathSums]), "\n")
		for i, l := range lines {
			if h, p, ok := strings.Cut(l, " "); ok {
				bare := strings.TrimPrefix(strings.TrimPrefix(p, " "), "*")
				if nw, ok := renamed[bare]; ok {
					lines[i] = h + "  " + nw
				}
			}
		}
		files[pathSums] = []byte(strings.Join(lines, "\n"))
	}
	return files
}

// parseSums reads sha256sum output ("<64 hex>  path", a "*" binary marker tolerated).
func parseSums(b []byte) (map[string]string, error) {
	out := map[string]string{}
	sc := bufio.NewScanner(bytes.NewReader(b))
	for sc.Scan() {
		l := strings.TrimRight(sc.Text(), "\r")
		if l == "" {
			continue
		}
		h, p, ok := strings.Cut(l, " ")
		p = strings.TrimPrefix(strings.TrimPrefix(p, " "), "*")
		if !ok || len(h) != 64 || p == "" || strings.ToLower(h) != h {
			return nil, fmt.Errorf("the appfs kit SHA256SUMS has a malformed line %q", l)
		}
		if _, dup := out[p]; dup {
			return nil, fmt.Errorf("the appfs kit SHA256SUMS lists %s twice", p)
		}
		out[p] = h
	}
	return out, sc.Err()
}

// File returns a kit file (nil when absent).
func (k *Kit) File(p string) []byte { return k.files[p] }

// HasCard reports whether the kit carries the files of the first-install SD card.
func (k *Kit) HasCard() bool { _, ok := k.files[cardFiles[0]]; return ok }

// Version and Commit are the MANIFEST values the report shows.
func (k *Kit) Version() string { return k.Manifest["akam_version"] }
func (k *Kit) Commit() string  { return k.Manifest["source_commit"] }

func sum(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

// KitLayout is how internal/kit verifies a signed 2K kit tarball (archive safety, signature, MANIFEST against the
// files, SHA256SUMS): the same code as the 1080p kit with this payload. LoadKit then applies the checks of this
// package (kind, time64 gate, exact file set).
func KitLayout() *kit.Layout {
	pay := []string{"supported_hardware.txt", "licenses/LICENSE.akam", "licenses/THIRD_PARTY_NOTICES.md", "licenses/LICENSE.dropbear"}
	for _, p := range RequiredKitPaths() {
		if p != pathManifest && p != pathSums {
			pay = append(pay, p)
		}
	}
	legacy := map[string]string{}
	for nw, old := range legacyBinNames {
		legacy[pathBin+nw] = pathBin + old
	}
	return &kit.Layout{Target: "arm-linux", Payload: pay, LegacyPayload: legacy, HardwareFile: "supported_hardware.txt"}
}
