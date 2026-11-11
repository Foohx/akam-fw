package kit

import (
	"archive/tar"
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
)

// SupportedKitFormats are the kit_format values this program reads (verify_kit.sh SUPPORTED_KIT_FORMATS).
var SupportedKitFormats = []string{"1"}

const (
	maxTotalSize = 209715200 // 200 MiB of file content, as verify_kit.sh
	maxStream    = 1 << 30   // decompressed bytes read at all (a decompression bomb stops here)
	maxEntries   = 10000     // archive entries (a flood of empty entries is not content)
)

// Fail is a verification failure (verify_kit.sh "FAIL:", exit status 1).
type Fail struct{ Msg string }

func (f *Fail) Error() string { return "verify_kit: FAIL: " + f.Msg }

// Usage is a missing tool or bad option (exit status 2).
type Usage struct{ Msg string }

func (u *Usage) Error() string { return "verify_kit: " + u.Msg }

// ErrUnsigned: everything else was fine but the kit carries no signature and --allow-unsigned was not
// given (exit status 3).
var ErrUnsigned = errors.New("checksums OK but the kit is UNSIGNED: refusing (use --allow-unsigned to accept checksum-only trust)")

func fail(format string, a ...any) error { return &Fail{Msg: fmt.Sprintf(format, a...)} }

// Options of one verification.
type Options struct {
	// AllowedSigners is the content of the allowed_signers file.
	AllowedSigners []byte
	// Sums is the SHA256SUMS file (default: next to the tarball); Sig the signature (default Sums+".sig").
	Sums, Sig string
	SigGiven  bool
	// AllowUnsigned accepts a kit with no signature; AllowTest a kit_grade=test kit.
	AllowUnsigned, AllowTest bool
	// Expect: the tarball sha256 must start with it (>= 8 hex digits).
	Expect string
	// ExtractTo: copy the verified tree here (must not exist yet).
	ExtractTo string
	// Say and Warn print the "ok:" and WARNING lines (nil: discarded).
	Say, Warn func(string)
	// Decompress opens an .xz file as a stream (nil: XZReader, in process, with DefaultXZLimits).
	Decompress func(file string) (io.ReadCloser, error)

	// Test seams (nil: the real ones): symlink replaces os.Symlink, isLocal replaces filepath.IsLocal.
	symlink func(oldname, newname string) error
	isLocal func(string) bool
}

// modesMatter reports whether the OS has Unix permission bits (Windows has not: directory-mode
// verification then skips the mode comparison; the tar-mode check against the archive headers is the authority).
var modesMatter = runtime.GOOS != "windows"

// linksSidecar lists, one "path target" per line, the links written as regular-file copies because the
// OS refused os.Symlink (no privilege on Windows). It is read by VerifyDir.
const linksSidecar = ".akam-links"

func (o *Options) say(s string) {
	if o.Say != nil {
		o.Say(s)
	}
}
func (o *Options) warn(s string) {
	if o.Warn != nil {
		o.Warn(s)
	}
}

// Result of a successful verification.
type Result struct {
	Signed   bool
	SignedBy string
	Dir      string // the verified tree (a private temporary directory unless ExtractTo was given)
	Manifest map[string]string
	cleanup  func()
}

// Close removes the temporary tree (a no-op after ExtractTo copied it).
func (r *Result) Close() {
	if r != nil && r.cleanup != nil {
		r.cleanup()
	}
}

func sha256File(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// VerifyTarball is verify_kit.sh in tarball mode: sha256 against SHA256SUMS, signature, safe
// listing, extraction into a private directory, then the directory checks.
func VerifyTarball(tarball string, o Options) (*Result, error) {
	exp := strings.ToLower(o.Expect)
	if o.Expect != "" {
		if !regexp.MustCompile(`^[0-9a-f]+$`).MatchString(exp) {
			return nil, &Usage{"--expect-sha256 needs hex digits"}
		}
		if len(exp) < 8 {
			return nil, &Usage{"--expect-sha256 needs at least 8 hex digits"}
		}
	}
	if o.Decompress == nil {
		o.Decompress = xzDecompress
	}
	tmp, err := os.MkdirTemp("", "akam-verify-")
	if err != nil {
		return nil, err
	}
	res := &Result{cleanup: func() { os.RemoveAll(tmp) }}
	ok := false
	defer func() {
		if !ok {
			res.Close()
		}
	}()

	base := filepath.Base(tarball)
	sums := o.Sums
	if sums == "" {
		sums = filepath.Join(filepath.Dir(tarball), "SHA256SUMS")
	}
	sumsData, err := os.ReadFile(sums)
	if err != nil {
		return nil, fail("%s not found (download SHA256SUMS next to the kit)", sums)
	}
	// The tarball is copied to a private directory first: what is hashed, listed and extracted is one file.
	copyPath := filepath.Join(tmp, "kit.tar.xz")
	if err := copyFile(tarball, copyPath); err != nil {
		return nil, fail("cannot read %s", tarball)
	}
	got, err := sha256File(copyPath)
	if err != nil {
		return nil, err
	}
	var wants []string
	for _, l := range strings.Split(string(sumsData), "\n") {
		f := strings.Fields(l)
		if len(f) >= 2 && strings.TrimPrefix(f[1], "*") == base {
			wants = append(wants, f[0])
		}
	}
	switch {
	case len(wants) == 0:
		return nil, fail("%s is not listed in %s", base, sums)
	case len(wants) > 1:
		return nil, fail("%s is listed more than once in %s", base, sums)
	case got != wants[0]:
		return nil, fail("sha256 mismatch for %s: expected %s, got %s", base, wants[0], got)
	}
	o.say(fmt.Sprintf("ok: %s sha256 %s matches %s", base, got, filepath.Base(sums)))
	if o.Expect != "" {
		if !strings.HasPrefix(got, exp) {
			return nil, fail("sha256 %s does not start with --expect-sha256 %s", got, exp)
		}
		o.say(fmt.Sprintf("ok: sha256 starts with the expected %s", exp))
	}

	if err := o.checkSignature(res, sums, sumsData); err != nil {
		return nil, err
	}
	tree := filepath.Join(tmp, "tree")
	top, err := o.listAndExtract(copyPath, tree)
	if err != nil {
		return nil, err
	}
	if o.ExtractTo != "" {
		if _, err := os.Stat(o.ExtractTo); err == nil {
			return nil, fail("--extract-to %s already exists", o.ExtractTo)
		}
	}
	kitDir := filepath.Join(tree, top)
	if st, err := os.Stat(kitDir); err != nil || !st.IsDir() {
		return nil, fail("extraction produced no %s directory", top)
	}
	man, err := o.VerifyDir(kitDir, top)
	if err != nil {
		return nil, err
	}
	res.Manifest = man
	res.Dir = kitDir
	if o.ExtractTo != "" {
		if err := CopyTree(kitDir, o.ExtractTo); err != nil {
			return nil, err
		}
		res.Dir = o.ExtractTo
		o.say("extracted to " + o.ExtractTo)
	}
	ok = true
	if res.Signed {
		return res, nil
	}
	if o.AllowUnsigned {
		return res, nil
	}
	res.Close()
	return nil, ErrUnsigned
}

func (o *Options) checkSignature(res *Result, sums string, sumsData []byte) error {
	sigPath := o.Sig
	if sigPath == "" {
		sigPath = sums + ".sig"
	}
	sigData, err := os.ReadFile(sigPath)
	if err != nil {
		if o.SigGiven {
			return fail("--sig %s: no such file (an explicitly named signature must exist)", sigPath)
		}
		o.warn(fmt.Sprintf("no signature (%s not found). This kit is NOT authenticated: only its checksum", sigPath))
		o.warn("was compared, and a checksum from the same download place proves nothing.")
		return nil
	}
	if o.AllowedSigners == nil {
		return fail("signature present but allowed_signers not found")
	}
	if !HasKeys(o.AllowedSigners) {
		return fail("signature present but allowed_signers contains no public key (placeholder not replaced): cannot authenticate")
	}
	allowed, err := ParseAllowedSigners(o.AllowedSigners)
	if err != nil {
		return fail("INVALID signature on %s: %v", filepath.Base(sums), err)
	}
	sig, err := ParseSignature(sigData)
	if err != nil {
		return fail("INVALID signature on %s: %v", filepath.Base(sums), err)
	}
	out, err := Verify(allowed, SigIdentity, SigNamespace, sig, sumsData)
	if err != nil {
		return fail("INVALID signature on %s: %v", filepath.Base(sums), err)
	}
	res.Signed, res.SignedBy = true, out
	o.say(fmt.Sprintf("ok: signature valid (%s)", out))
	return nil
}

var nameRe = regexp.MustCompile(`^[A-Za-z0-9._+/-]*$`)

// readArchive lists the archive and validates it (names, types, modes, sizes), as the awk part of
// verify_kit.sh; it returns the top-level directory.
func (o *Options) readArchive(file string, visit func(*tar.Header, *tar.Reader, string) error) (string, error) {
	rc, err := o.Decompress(file)
	if err != nil {
		var u *Usage
		if errors.As(err, &u) {
			return "", err
		}
		return "", fail("cannot read the archive: %v", err)
	}
	defer rc.Close()
	lr := &limitReader{r: rc, left: maxStream}
	tr := tar.NewReader(lr)
	seen := map[string]bool{}
	top := ""
	var total int64
	n := 0
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return "", fail("cannot read the archive: %v", err)
		}
		n++
		if n > maxEntries {
			return "", fail("archive has more than %d entries", maxEntries)
		}
		name := h.Name
		if !nameRe.MatchString(name) {
			return "", fail("archive entry with a character outside A-Z a-z 0-9 . _ + - /")
		}
		if strings.HasPrefix(name, "/") || hasDotDot(name) {
			return "", fail("archive contains absolute or '..' paths")
		}
		clean := strings.TrimSuffix(name, "/")
		if !o.local(clean) {
			return "", fail("archive entry name is not a safe local path on this system: %s", name)
		}
		for _, part := range strings.Split(clean, "/") {
			// Windows drops a trailing dot, so "x." and "x" are one file there; "." and "" are no name.
			if part == "" || part == "." || strings.HasSuffix(part, ".") {
				return "", fail("archive entry with an empty, '.' or dot-terminated path element: %s", name)
			}
		}
		if top == "" {
			top = strings.SplitN(name, "/", 2)[0]
			if !strings.HasPrefix(top, "akam-kit-") || len(top) == len("akam-kit-") {
				return "", fail("archive top-level directory '%s' is not akam-kit-*", top)
			}
		}
		if clean != top && !strings.HasPrefix(clean, top+"/") {
			return "", fail("archive has entries outside %s/", top)
		}
		if clean == top+"/"+linksSidecar {
			return "", fail("archive contains %s, which only the extraction itself writes", linksSidecar)
		}
		// Case-folded: on Windows and macOS "A" and "a" are the same file.
		key := strings.ToLower(clean)
		if seen[key] {
			return "", fail("archive has duplicate entries (names that differ only in case collide on Windows and macOS)")
		}
		seen[key] = true
		perm := h.Mode & 0o7777
		switch h.Typeflag {
		case tar.TypeReg:
			if perm != 0o644 && perm != 0o755 {
				return "", fail("unexpected file mode in archive: %s (%04o)", name, perm)
			}
			total += h.Size
		case tar.TypeDir:
			if perm != 0o755 {
				return "", fail("unexpected directory mode in archive: %s (%04o)", name, perm)
			}
		case tar.TypeSymlink:
			if h.Linkname != "dropbearmulti" || (clean != top+"/overlay/bin/dropbear" && clean != top+"/overlay/bin/dropbearkey") {
				return "", fail("unexpected archive entry: %s -> %s", name, h.Linkname)
			}
		default:
			return "", fail("unexpected archive entry: %s (type %c)", name, h.Typeflag)
		}
		if total > maxTotalSize {
			return "", fail("archive content larger than 200 MiB")
		}
		if visit != nil {
			if err := visit(h, tr, clean); err != nil {
				return "", err
			}
		}
	}
	if n == 0 {
		return "", fail("the archive is empty")
	}
	// Read the stream to its end: this is where a truncated .xz (xz exits non-zero) or a
	// decompression bomb (over maxStream) is noticed even when the tar part looked complete.
	if _, err := io.Copy(io.Discard, lr); err != nil {
		return "", fail("cannot read the archive: %v", err)
	}
	return top, nil
}

// limitReader fails (instead of silently ending) once more than left bytes are read.
type limitReader struct {
	r    io.Reader
	left int64
}

func (l *limitReader) Read(b []byte) (int, error) {
	if l.left < 0 {
		return 0, errors.New("the decompressed archive is larger than 1 GiB")
	}
	if int64(len(b)) > l.left+1 {
		b = b[:l.left+1]
	}
	n, err := l.r.Read(b)
	l.left -= int64(n)
	if l.left < 0 {
		return n, errors.New("the decompressed archive is larger than 1 GiB")
	}
	return n, err
}

func hasDotDot(name string) bool {
	for _, p := range strings.Split(name, "/") {
		if p == ".." {
			return true
		}
	}
	return false
}

func (o *Options) local(name string) bool {
	if o.isLocal != nil {
		return o.isLocal(name)
	}
	return filepath.IsLocal(filepath.FromSlash(name))
}

func (o *Options) listAndExtract(file, target string) (string, error) {
	// First pass: validate everything without writing a byte.
	top, err := o.readArchive(file, nil)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(target, 0o700); err != nil {
		return "", err
	}
	// Second pass: extract (no owner, modes as archived, no overwrite). The stream is the same
	// private copy; names were validated by the first pass and are re-validated here.
	type pendingLink struct{ rel, target, dst string }
	var links []pendingLink
	_, err = o.readArchive(file, func(h *tar.Header, tr *tar.Reader, clean string) error {
		dst := filepath.Join(target, filepath.FromSlash(clean))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(dst, 0o755); err != nil {
				return err
			}
			return os.Chmod(dst, 0o755)
		case tar.TypeSymlink:
			// Created after the walk: the link may come before its target in the archive.
			links = append(links, pendingLink{rel: strings.TrimPrefix(clean, top+"/"), target: h.Linkname, dst: dst})
			return nil
		default:
			f, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
			if err != nil {
				return err
			}
			if _, err := io.Copy(f, tr); err != nil {
				f.Close()
				return err
			}
			if err := f.Close(); err != nil {
				return err
			}
			return os.Chmod(dst, fs.FileMode(h.Mode&0o777))
		}
	})
	if err != nil {
		return "", err
	}
	o.say(fmt.Sprintf("ok: archive layout is safe (%d entries)", countEntries(target)))
	// Links: os.Symlink when the OS allows it; otherwise (no privilege on Windows) a regular-file copy
	// of the target plus a line in the sidecar, which VerifyDir accepts as the same thing.
	var sidecar strings.Builder
	for _, l := range links {
		symlink := o.symlink
		if symlink == nil {
			symlink = os.Symlink
		}
		if err := symlink(l.target, l.dst); err == nil {
			continue
		}
		if err := copyFile(linkCopySource(filepath.Join(target, top), l.rel, l.target), l.dst); err != nil {
			return "", fmt.Errorf("cannot create the link %s (no symlink, no copy): %w", l.rel, err)
		}
		if err := os.Chmod(l.dst, 0o755); err != nil {
			return "", err
		}
		sidecar.WriteString(l.rel + " " + l.target + "\n")
	}
	if sidecar.Len() > 0 {
		if err := os.WriteFile(filepath.Join(target, top, linksSidecar), []byte(sidecar.String()), 0o644); err != nil {
			return "", err
		}
	}
	return top, nil
}

func countEntries(dir string) int {
	n := 0
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if p != dir {
			n++
		}
		return nil
	})
	return n
}

func contains1(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}

func manifestValues(manifest, key string) []string {
	var out []string
	for _, l := range strings.Split(manifest, "\n") {
		if v, ok := strings.CutPrefix(l, key+"="); ok {
			out = append(out, v)
		}
	}
	return out
}

var versionRe = regexp.MustCompile(`^[A-Za-z0-9._+-]+$`)

// VerifyDir is verify_dir of verify_kit.sh. topExpected is the archive's top directory ("" in
// directory mode). It returns the MANIFEST's key=value pairs (first occurrence).
func (o *Options) VerifyDir(d, topExpected string) (map[string]string, error) {
	mb, err := os.ReadFile(filepath.Join(d, "MANIFEST.txt"))
	if err != nil {
		return nil, fail("MANIFEST.txt missing in %s", d)
	}
	m := string(mb)
	if _, err := os.Stat(filepath.Join(d, "SHA256SUMS")); err != nil {
		return nil, fail("SHA256SUMS missing in %s", d)
	}
	copies, err := readLinksSidecar(d)
	if err != nil {
		return nil, err
	}
	bad := false
	_ = filepath.WalkDir(d, func(p string, de fs.DirEntry, err error) error {
		if err == nil && !de.IsDir() && !de.Type().IsRegular() && de.Type()&fs.ModeSymlink == 0 {
			bad = true
		}
		return nil
	})
	if bad {
		return nil, fail("unexpected file type in the kit")
	}
	kf := manifestValues(m, "kit_format")
	if len(kf) != 1 || kf[0] == "" {
		return nil, fail("MANIFEST.txt has no (single) kit_format")
	}
	if !contains1(SupportedKitFormats, kf[0]) {
		return nil, fail("kit_format %s is not supported by this verifier (supports: %s); use a matching Akam version", kf[0], strings.Join(SupportedKitFormats, " "))
	}
	o.say(fmt.Sprintf("ok: kit_format %s is supported", kf[0]))
	ver := manifestValues(m, "akam_version")
	if len(ver) != 1 || ver[0] == "" {
		return nil, fail("MANIFEST.txt has no (single) akam_version")
	}
	if !versionRe.MatchString(ver[0]) {
		return nil, fail("MANIFEST.txt akam_version is not a plain version string")
	}
	if topExpected != "" && topExpected != "akam-kit-"+ver[0] {
		return nil, fail("archive directory %s does not match MANIFEST akam_version %s", topExpected, ver[0])
	}
	for _, kv := range [][2]string{{"time64_gate", "pass"}, {"target", "arm-linux-musleabi"}} {
		v := manifestValues(m, kv[0])
		if len(v) != 1 || v[0] != kv[1] {
			return nil, fail("MANIFEST.txt must contain exactly one %s line, equal to %s", kv[0], kv[1])
		}
	}
	grade := manifestValues(m, "kit_grade")
	if len(grade) != 1 {
		return nil, fail("MANIFEST.txt has no (single) kit_grade")
	}
	if grade[0] != "release" {
		if !o.AllowTest || grade[0] != "test" {
			return nil, fail("this is not a release kit (kit_grade is not 'release'): development kits need --allow-test-kit")
		}
		o.warn("kit_grade=test: development kit, NOT a release.")
	}

	// 1. inner SHA256SUMS: every listed file matches, nothing missing or extra.
	sb, _ := os.ReadFile(filepath.Join(d, "SHA256SUMS"))
	var listed []string
	sc := bufio.NewScanner(bytes.NewReader(sb))
	var firstBad []string
	for sc.Scan() {
		l := sc.Text()
		if strings.TrimSpace(l) == "" {
			continue
		}
		if len(l) < 67 {
			firstBad = append(firstBad, l+": improperly formatted line")
			continue
		}
		want, name := l[:64], strings.TrimPrefix(l[64:], " ")
		name = strings.TrimPrefix(name, " ")
		name = strings.TrimPrefix(name, "*")
		listed = append(listed, name)
		if !safeRel(name) {
			firstBad = append(firstBad, name+": unsafe path")
			continue
		}
		got, err := sha256File(filepath.Join(d, filepath.FromSlash(name)))
		if err != nil {
			firstBad = append(firstBad, name+": FAILED open or read")
		} else if got != want {
			firstBad = append(firstBad, name+": FAILED")
		}
	}
	if len(firstBad) > 0 {
		if len(firstBad) > 3 {
			firstBad = firstBad[:3]
		}
		return nil, fail("SHA256SUMS check failed: %s", strings.Join(firstBad, " "))
	}
	var have []string
	_ = filepath.WalkDir(d, func(p string, de fs.DirEntry, err error) error {
		if err == nil && de.Type().IsRegular() {
			rel, _ := filepath.Rel(d, p)
			rel = filepath.ToSlash(rel)
			if rel != "SHA256SUMS" && rel != linksSidecar && copies[rel] == "" {
				have = append(have, rel)
			}
		}
		return nil
	})
	sort.Strings(have)
	sort.Strings(listed)
	if strings.Join(have, "\n") != strings.Join(listed, "\n") {
		return nil, fail("files differ from SHA256SUMS: %s", diffHint(have, listed))
	}
	if !contains1(listed, "MANIFEST.txt") {
		return nil, fail("SHA256SUMS does not cover MANIFEST.txt")
	}

	// 2. MANIFEST file/link lines.
	var manFiles []string
	n := 0
	links := 0
	usedCopies := 0
	for _, l := range strings.Split(m, "\n") {
		if !strings.HasPrefix(l, "file ") && !strings.HasPrefix(l, "link ") {
			continue
		}
		f := strings.Fields(l)
		if len(f) < 4 {
			return nil, fail("MANIFEST has an unknown entry type '%s'", l)
		}
		typ, a, mode, p := f[0], f[1], f[2], f[3]
		if !safeRel(p) {
			return nil, fail("MANIFEST path '%s' is not safe", p)
		}
		full := filepath.Join(d, filepath.FromSlash(p))
		switch typ {
		case "file":
			if mode != "644" && mode != "755" {
				return nil, fail("MANIFEST mode %s for %s is not 644 or 755", mode, p)
			}
			st, err := os.Lstat(full)
			if err != nil || !st.Mode().IsRegular() {
				return nil, fail("MANIFEST lists %s but it is not a regular file", p)
			}
			got, _ := sha256File(full)
			if got != a {
				return nil, fail("sha256 mismatch for %s: MANIFEST %s, actual %s", p, a, got)
			}
			if modesMatter && fmt.Sprintf("%o", st.Mode().Perm()) != mode {
				return nil, fail("mode mismatch for %s: MANIFEST %s, actual %o", p, mode, st.Mode().Perm())
			}
			manFiles = append(manFiles, p)
		case "link":
			if !((p == "overlay/bin/dropbear" || p == "overlay/bin/dropbearkey") && a == "dropbearmulti") {
				return nil, fail("MANIFEST lists an unexpected symlink %s -> %s", p, a)
			}
			if c, isCopy := copies[p]; isCopy {
				// A regular-file copy recorded in the sidecar (no symlink on this OS): same bytes as its target.
				if c != a {
					return nil, fail("link target mismatch for %s", p)
				}
				st, err := os.Lstat(full)
				if err != nil || !st.Mode().IsRegular() {
					return nil, fail("%s is recorded as a link copy but is not a regular file", p)
				}
				got, _ := sha256File(full)
				want, _ := sha256File(linkCopySource(d, p, a))
				if want == "" || got != want {
					return nil, fail("link copy %s differs from its target %s", p, a)
				}
				usedCopies++
			} else {
				t, err := os.Readlink(full)
				if err != nil {
					return nil, fail("MANIFEST lists %s as a link but it is not", p)
				}
				if t != a {
					return nil, fail("link target mismatch for %s", p)
				}
			}
			links++
		}
		n++
	}
	sort.Strings(manFiles)
	var haveNoMF []string
	for _, h := range have {
		if h != "MANIFEST.txt" {
			haveNoMF = append(haveNoMF, h)
		}
	}
	if strings.Join(manFiles, "\n") != strings.Join(haveNoMF, "\n") {
		return nil, fail("MANIFEST and the kit's files differ: %s", diffHint(manFiles, haveNoMF))
	}
	actualLinks := 0
	_ = filepath.WalkDir(d, func(p string, de fs.DirEntry, err error) error {
		if err == nil && de.Type()&fs.ModeSymlink != 0 {
			actualLinks++
		}
		return nil
	})
	if usedCopies != len(copies) {
		return nil, fail("%s lists a link that MANIFEST.txt does not", linksSidecar)
	}
	if actualLinks+usedCopies != links {
		return nil, fail("symlinks in the kit are not all listed in MANIFEST.txt")
	}

	// 3. the payload we promise.
	for _, p := range []string{"bin/ak_rtsp", "bin/dropbearmulti", "bin/flash_tool", "patches/patch_service_sh.sh", "supported_hardware.txt", "licenses/LICENSE.akam", "licenses/THIRD_PARTY_NOTICES.md", "licenses/LICENSE.dropbear"} {
		if !contains1(manFiles, p) {
			return nil, fail("required file %s is not in the kit", p)
		}
	}
	hw, _ := os.ReadFile(filepath.Join(d, "supported_hardware.txt"))
	hs := sha256.Sum256(hw)
	if w := manifestValues(m, "supported_hardware_sha256"); len(w) == 0 || w[0] != hex.EncodeToString(hs[:]) {
		return nil, fail("supported_hardware_sha256 in MANIFEST.txt does not match supported_hardware.txt")
	}
	if !regexp.MustCompile(`(?m)^validated\|`).Match(hw) {
		return nil, fail("supported_hardware.txt lists no validated target")
	}
	src := ""
	if v := manifestValues(m, "source_commit"); len(v) > 0 {
		src = v[0]
		if len(src) > 12 {
			src = src[:12]
		}
	}
	o.say(fmt.Sprintf("ok: %d MANIFEST entries match (akam_version %s, source %s)", n, ver[0], src))
	out := map[string]string{}
	for _, l := range strings.Split(m, "\n") {
		if k, v, ok := strings.Cut(l, "="); ok {
			if _, dup := out[k]; !dup {
				out[k] = v
			}
		}
	}
	return out, nil
}

// linkCopySource is the file a link copy is made from. The link's target (dropbearmulti) names a file
// that lives next to the link on the camera; in the kit it is bin/dropbearmulti.
func linkCopySource(kitDir, linkRel, target string) string {
	next := filepath.Join(kitDir, filepath.FromSlash(path.Dir(linkRel)), filepath.FromSlash(target))
	if _, err := os.Stat(next); err == nil {
		return next
	}
	return filepath.Join(kitDir, "bin", filepath.FromSlash(target))
}

// readLinksSidecar reads the optional .akam-links file: "path target" per line, only the two known links.
func readLinksSidecar(d string) (map[string]string, error) {
	b, err := os.ReadFile(filepath.Join(d, linksSidecar))
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, fail("cannot read %s: %v", linksSidecar, err)
	}
	out := map[string]string{}
	for _, l := range strings.Split(string(b), "\n") {
		if l == "" {
			continue
		}
		f := strings.Split(l, " ")
		if len(f) != 2 || !((f[0] == "overlay/bin/dropbear" || f[0] == "overlay/bin/dropbearkey") && f[1] == "dropbearmulti") {
			return nil, fail("%s has an unexpected line", linksSidecar)
		}
		if _, dup := out[f[0]]; dup {
			return nil, fail("%s has a duplicate line", linksSidecar)
		}
		out[f[0]] = f[1]
	}
	return out, nil
}

// VerifyExtracted is directory mode: the tarball signature is not checked here.
func (o *Options) VerifyExtracted(dir string) (map[string]string, error) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	o.say("note: directory mode: the tarball signature is not checked here; verify the .tar.xz it came from.")
	return o.VerifyDir(abs, "")
}

func safeRel(p string) bool {
	if p == "" || strings.HasPrefix(p, "/") {
		return false
	}
	for _, s := range strings.Split(p, "/") {
		if s == ".." {
			return false
		}
	}
	return path.Clean(p) != "." || p == "."
}

func diffHint(a, b []string) string {
	in := func(l []string, s string) bool { return contains1(l, s) }
	var d []string
	for _, x := range a {
		if !in(b, x) {
			d = append(d, "< "+x)
		}
	}
	for _, x := range b {
		if !in(a, x) {
			d = append(d, "> "+x)
		}
	}
	if len(d) > 3 {
		d = d[:3]
	}
	return strings.Join(d, " ")
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// CopyTree is `cp -a src/. dst/` (modes and the two symlinks kept).
func CopyTree(src, dst string) error {
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}
	return filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		if rel == "." {
			return nil
		}
		to := filepath.Join(dst, rel)
		switch {
		case d.IsDir():
			return os.MkdirAll(to, 0o755)
		case d.Type()&fs.ModeSymlink != 0:
			t, err := os.Readlink(p)
			if err != nil {
				return err
			}
			return os.Symlink(t, to)
		default:
			st, err := d.Info()
			if err != nil {
				return err
			}
			if err := copyFile(p, to); err != nil {
				return err
			}
			return os.Chmod(to, st.Mode().Perm())
		}
	})
}
