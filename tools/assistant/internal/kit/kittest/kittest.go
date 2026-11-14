// Package kittest builds small signed (or unsigned) kits for the tests. A kit made here has the
// layout of tools/release/make_kit.sh but tiny stand-in files; the signing key is a throw-away
// ed25519 key, never the maintainer's.
package kittest

import (
	"archive/tar"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/ulikunitz/xz"
	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// Available reports whether a kit can be written and read: always, the .xz codec is in process.
func Available() bool { return true }

// DefaultModel is the camera model of a kit that does not say another one.
const DefaultModel = "lsc-indoor-1080p-ak3918"

// Spec describes the kit to build. The zero value is a valid release kit.
type Spec struct {
	Version string // default 1.0.0
	Model   string // camera model id, default DefaultModel
	Grade   string // default release
	Format  string // default 1
	// Files overrides or adds payload files (path inside the kit -> content, mode 644 unless it
	// starts with "bin/" or ends in ".sh", which are 755). A nil content removes the default file.
	Files map[string][]byte
	// Mutate may change the archive entries before they are written.
	TarMutate func(entries *[]Entry)
	// ManifestExtra is appended to MANIFEST.txt (after the file lines).
	ManifestExtra string
	// Unsigned writes no SHA256SUMS.sig.
	Unsigned bool
	// BadSumsLine makes the outer SHA256SUMS list a wrong digest.
	BadSumsLine bool
	// Hash alg of the signature (sha256 default).
	HashAlg string
}

// Entry is one tar member.
type Entry struct {
	Name     string
	Type     byte
	Mode     int64
	Body     []byte
	Linkname string
}

// Built is the result.
type Built struct {
	Tarball, Sums, Sig string
	Dir                string // the folder holding the three files
	AllowedSigners     []byte // the allowed_signers text that trusts the test key
	Signer             gossh.Signer
	TarSHA             string
}

// NewSigner makes a throw-away ed25519 signer.
func NewSigner(t testing.TB) gossh.Signer {
	t.Helper()
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	s, err := gossh.NewSignerFromKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// AllowedLine is the allowed_signers line for signer.
func AllowedLine(s gossh.Signer) []byte {
	return []byte("akam-release namespaces=\"akam-release\" " + strings.TrimSpace(string(gossh.MarshalAuthorizedKey(s.PublicKey()))) + " test@example\n")
}

func shaHex(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// DefaultFiles is the payload a kit must carry.
func DefaultFiles() map[string][]byte { return DefaultFilesFor(DefaultModel) }

// DefaultFilesFor is DefaultFiles for a kit of the given model (its supported_hardware.txt carries the model header).
func DefaultFilesFor(model string) map[string][]byte {
	hw := []byte("# test list\nmodel=" + model + "\nvalidated|Test camera|soc|kernel|8 partitions|xz|" + strings.Repeat("ab", 32) + "\n")
	return map[string][]byte{
		"bin/ak_rtsp":                       []byte("ak_rtsp stand-in\n"),
		"bin/dropbearmulti":                 []byte("dropbearmulti stand-in\n"),
		"bin/flash_tool":                    []byte("#!/bin/sh\nflash_tool stand-in\n"),
		"patches/patch_service_sh.sh":       []byte("#!/bin/sh\n"),
		"supported_hardware.txt":            hw,
		"licenses/LICENSE.akam":             []byte("MIT\n"),
		"licenses/THIRD_PARTY_NOTICES.md":   []byte("notices\n"),
		"licenses/LICENSE.dropbear":         []byte("dropbear\n"),
		"scripts/flash/akam_reflash_ssh.sh": []byte("#!/bin/sh\n# camera-side reflash script (test copy)\n"),
		"scripts/flash/flash_guard.sh":      []byte("#!/bin/sh\n# flash guard (test copy)\n"),
		"scripts/flash/preflight_check.sh":  []byte("#!/bin/sh\n# preflight (test copy)\n"),
		"scripts/hwcheck/akam_hwcheck.sh":   []byte("#!/bin/sh\n# hwcheck (test copy)\n"),
	}
}

func modeFor(p string) int64 {
	if strings.HasPrefix(p, "bin/") || strings.HasSuffix(p, ".sh") {
		return 0o755
	}
	return 0o644
}

// XZ compresses data to a single-stream, single-block .xz with the library writer (no external xz).
func XZ(t testing.TB, data []byte) []byte {
	t.Helper()
	var out bytes.Buffer
	w, err := xz.WriterConfig{DictCap: 1 << 20}.NewWriter(&out)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write(data); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

// Build writes the kit into dir.
func Build(t testing.TB, dir string, sp Spec) Built {
	t.Helper()
	if sp.Version == "" {
		sp.Version = "1.0.0"
	}
	if sp.Model == "" {
		sp.Model = DefaultModel
	}
	if sp.Grade == "" {
		sp.Grade = "release"
	}
	if sp.Format == "" {
		sp.Format = "1"
	}
	if sp.HashAlg == "" {
		sp.HashAlg = "sha256"
	}
	files := DefaultFilesFor(sp.Model)
	for k, v := range sp.Files {
		if v == nil {
			delete(files, k)
		} else {
			files[k] = v
		}
	}
	top := kit.KitTop(sp.Model, sp.Version)
	var names []string
	for k := range files {
		names = append(names, k)
	}
	sort.Strings(names)
	hw := files["supported_hardware.txt"]
	var man strings.Builder
	fmt.Fprintf(&man, "kit_format=%s\nkit_grade=%s\nakam_version=%s\nmodel=%s\nsource_commit=0123456789abcdef0123\ntarget=arm-linux-musleabi\ntime64_gate=pass\nsupported_hardware_sha256=%s\n", sp.Format, sp.Grade, sp.Version, sp.Model, shaHex(hw))
	for _, n := range names {
		fmt.Fprintf(&man, "file %s %o %s\n", shaHex(files[n]), modeFor(n), n)
	}
	man.WriteString("link dropbearmulti 777 overlay/bin/dropbear\n")
	man.WriteString(sp.ManifestExtra)
	manifest := []byte(man.String())
	var inner strings.Builder
	all := map[string][]byte{"MANIFEST.txt": manifest}
	for k, v := range files {
		all[k] = v
	}
	var allNames []string
	for k := range all {
		allNames = append(allNames, k)
	}
	sort.Strings(allNames)
	for _, n := range allNames {
		fmt.Fprintf(&inner, "%s  %s\n", shaHex(all[n]), n)
	}
	all["SHA256SUMS"] = []byte(inner.String())

	entries := []Entry{{Name: top + "/", Type: tar.TypeDir, Mode: 0o755}}
	seenDir := map[string]bool{}
	var keys []string
	for k := range all {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		parts := strings.Split(k, "/")
		for i := 1; i < len(parts); i++ {
			d := top + "/" + strings.Join(parts[:i], "/") + "/"
			if !seenDir[d] {
				seenDir[d] = true
				entries = append(entries, Entry{Name: d, Type: tar.TypeDir, Mode: 0o755})
			}
		}
		m := int64(0o644)
		if k != "MANIFEST.txt" && k != "SHA256SUMS" {
			m = modeFor(k)
		}
		entries = append(entries, Entry{Name: top + "/" + k, Type: tar.TypeReg, Mode: m, Body: all[k]})
	}
	for _, d := range []string{"overlay/", "overlay/bin/"} {
		if !seenDir[top+"/"+d] {
			entries = append(entries, Entry{Name: top + "/" + d, Type: tar.TypeDir, Mode: 0o755})
		}
	}
	entries = append(entries, Entry{Name: top + "/overlay/bin/dropbear", Type: tar.TypeSymlink, Mode: 0o777, Linkname: "dropbearmulti"})
	if sp.TarMutate != nil {
		sp.TarMutate(&entries)
	}
	var tb bytes.Buffer
	tw := tar.NewWriter(&tb)
	for _, e := range entries {
		h := &tar.Header{Name: e.Name, Typeflag: e.Type, Mode: e.Mode, Size: int64(len(e.Body)), Linkname: e.Linkname, Format: tar.FormatPAX}
		if err := tw.WriteHeader(h); err != nil {
			t.Fatal(err)
		}
		if _, err := tw.Write(e.Body); err != nil {
			t.Fatal(err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	xzOut := XZ(t, tb.Bytes())
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	b := Built{Dir: dir, Tarball: filepath.Join(dir, kit.KitFile(sp.Model, sp.Version)), Sums: filepath.Join(dir, "SHA256SUMS"), Sig: filepath.Join(dir, "SHA256SUMS.sig")}
	if err := os.WriteFile(b.Tarball, xzOut, 0o644); err != nil {
		t.Fatal(err)
	}
	b.TarSHA = shaHex(xzOut)
	digest := b.TarSHA
	if sp.BadSumsLine {
		digest = strings.Repeat("0", 64)
	}
	sums := []byte(fmt.Sprintf("%s  %s\n", digest, filepath.Base(b.Tarball)))
	if err := os.WriteFile(b.Sums, sums, 0o644); err != nil {
		t.Fatal(err)
	}
	b.Signer = NewSigner(t)
	b.AllowedSigners = AllowedLine(b.Signer)
	if !sp.Unsigned {
		if err := os.WriteFile(b.Sig, kit.SignForTest(b.Signer, kit.SigNamespace, sp.HashAlg, sums), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return b
}

// ClientFor is the download client trusting the test certificate of srv (no proxy, whatever the environment).
func ClientFor(srv *httptest.Server) *kit.Client {
	pool := x509.NewCertPool()
	pool.AddCert(srv.Certificate())
	return &kit.Client{Timeout: time.Minute, TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: pool}}
}
