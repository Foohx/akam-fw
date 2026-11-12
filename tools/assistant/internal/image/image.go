package image

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

// Options of Build.
type Options struct {
	Kit   *Kit
	Stock *Stock
	// KitAuth and KitInfo are the two strings of the "kit authentication" report line (the caller knows
	// how the kit was verified: signature, directory, unsigned).
	KitAuth, KitInfo string
	// Epoch is the squashfs creation time and the modification time of every inode (bash:
	// SOURCE_DATE_EPOCH, else the build time).
	Epoch uint32
	// Now names the image file (ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs); zero means time.Now().
	Now time.Time
	// UntestedOK is --i-understand-untested; AllowTight is AKAM_ALLOW_TIGHT; AllowUnpatchedService is
	// AKAM_ALLOW_UNPATCHED_SERVICE.
	UntestedOK, AllowTight, AllowUnpatchedService bool
	// Say gets the progress lines, Note what the patch scripts print on stderr, Warn the warnings as they
	// are raised (they are also kept in Result.Warnings). Any of them may be nil.
	Say, Note, Warn func(string)
}

// Result of a successful Build.
type Result struct {
	// Image is the padded image: exactly PartitionSize bytes.
	Image []byte
	// Name is the file name of the image.
	Name string
	// SHA256 is the sha256 of Image.
	SHA256 string
	// SquashSize is the size of the squashfs before the 0xFF padding (it includes the zero padding to 4096).
	SquashSize int
	// SquashSHA is the sha256 of that squashfs.
	SquashSHA string
	// Free is the room left in the partition.
	Free int
	// Warnings are the warnings raised during the build.
	Warnings []string
	// Report is build_report.txt.
	Report string
	// ServiceDiff is the unified diff of sbin/service.sh ("" when it was not patched).
	ServiceDiff string
	// Listing is the canonical listing of the image as re-read with the reader (gate step 9).
	Listing []squashfs.ListEntry
	// DroppedXattrs is the number of stock inodes whose extended attributes were dropped (-no-xattrs).
	DroppedXattrs int
	// Tree is the planned tree the image was written from.
	Tree *squashfs.Node
}

type builder struct {
	o     Options
	warns []string
}

func (b *builder) say(s string) {
	if b.o.Say != nil {
		b.o.Say(s)
	}
}
func (b *builder) note(s string) {
	if b.o.Note != nil {
		b.o.Note(s)
	}
}
func (b *builder) warn(s string) {
	b.warns = append(b.warns, s)
	if b.o.Warn != nil {
		b.o.Warn(s)
	}
}

func sha(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// Build assembles, checks, writes, pads and re-reads the image. Every refusal of build_firmware.sh is an error.
func Build(o Options) (*Result, error) {
	b := &builder{o: o}
	if o.Kit == nil || o.Stock == nil {
		return nil, errors.New("image: a kit and the stock APP dump are required")
	}
	k, stock := o.Kit, o.Stock
	if len(stock.App) != PartitionSize {
		return nil, fmt.Errorf("MTD layout differs: %s is %d bytes, the APP partition (mtd7) is %d. Refusing.", stock.Name, len(stock.App), PartitionSize)
	}
	stockSHA := sha(stock.App)
	fsr, err := squashfs.Open(bytes.NewReader(stock.App), int64(len(stock.App)))
	if err != nil {
		return nil, fmt.Errorf("%s is not a squashfs this builder can read: %w", stock.Name, err)
	}
	info := fsr.Info()
	stockComp, stockBlock, stockBCJ := info.CompressionName(), int(info.BlockSize), "none"
	stockFmt := fmt.Sprintf("squashfs %s block=%d bcj=%s", stockComp, stockBlock, stockBCJ)

	// hardware verdict
	hv, hd := verdict(string(k.File("supported_hardware.txt")), stockSHA, stockFmt, stock)
	b.say(fmt.Sprintf("Hardware check: %s (%s)", hv, hd))
	if hv != "validated" {
		if !o.UntestedOK {
			return nil, fmt.Errorf("hardware/firmware is not validated (%s: %s). Akam is only validated for the targets in tools/release/supported_hardware.txt. If you accept the risk, repeat with --i-understand-untested", hv, hd)
		}
		b.warn(fmt.Sprintf("*** UNTESTED hardware/firmware (%s: %s). Building anyway (--i-understand-untested). Akam makes no promise that the result boots. ***", hv, hd))
	}
	// compression: the native writer produces xz only, like the stock APP of the supported camera.
	if stockComp != "xz" {
		return nil, fmt.Errorf("the stock APP uses squashfs compression '%s', which the native builder cannot write (xz only). Use --builder tools (Linux) or an image built elsewhere (--image FILE)", stockComp)
	}
	if stockBlock < 4096 || stockBlock > 1<<20 || stockBlock&(stockBlock-1) != 0 {
		return nil, fmt.Errorf("the stock APP has an unsupported block size %d", stockBlock)
	}

	// 1. the stock tree, read into memory (-no-xattrs: the extended attributes are dropped and counted)
	b.say("Reading the stock APP tree in memory...")
	root, rep, err := fsr.TreeWith(squashfs.TreeOptions{DropXattrs: true})
	if err != nil {
		return nil, fmt.Errorf("cannot read the stock APP tree: %w", err)
	}
	for _, d := range []string{"bin", "sbin", "local"} {
		if n := child(root, d); n == nil || n.Type != squashfs.TypeDir {
			return nil, fmt.Errorf("the stock APP has no %s/ -- not an APP tree", d)
		}
	}
	// The bash builder works on a tree extracted by the user with unsquashfs: as a normal user, unsquashfs
	// creates regular files under the umask (022), so a stock 0775 script or a 0777 file comes out 0755 /
	// 0755 and the image carries that; directories keep their stored mode. Same here, or the safety
	// checks below (nothing group/other-writable) would refuse a real stock APP, which has 0775 scripts.
	masked := 0
	walk(root, "", func(_ string, n *squashfs.Node) {
		if n.Type == squashfs.TypeFile && n.Mode&0o022 != 0 {
			n.Mode &^= 0o022
			masked++
		}
	})
	if masked > 0 {
		b.say(fmt.Sprintf("Removed group/other write permission from %d stock file(s) (umask 022, like unsquashfs run by a normal user).", masked))
	}
	if rep.DroppedXattrs > 0 {
		b.say(fmt.Sprintf("Dropped the extended attributes of %d inode(s) (-no-xattrs, like mksquashfs).", rep.DroppedXattrs))
	}

	// 2. de-Tuyafication + 3. Akam
	b.say("Applying de-Tuyafication patch...")
	removeChild(child(root, "bin"), "anyka_ipc")
	b.say("Installing Akam components (kit mode)...")
	for _, n := range []string{"ak_rtsp", "dropbearmulti", "dropbear", "dropbearkey"} {
		removeChild(child(root, "bin"), n)
	}
	if err := putFile(root, "bin/ak_rtsp", 0o755, k.File("bin/ak_rtsp")); err != nil {
		return nil, err
	}
	if err := putFile(root, "bin/dropbearmulti", 0o755, k.File("bin/dropbearmulti")); err != nil {
		return nil, err
	}
	for _, e := range parseList() {
		switch e.kind {
		case "file":
			if e.imgPath == "-" {
				continue
			}
			m, err := strconv.ParseUint(e.a, 8, 16)
			if err != nil {
				return nil, fmt.Errorf("image_files.txt: bad mode in '%s'", e.imgPath)
			}
			if err := putFile(root, e.imgPath, uint16(m), k.File(e.kitPath)); err != nil {
				return nil, err
			}
		case "link":
			if err := putLink(root, e.imgPath, e.a); err != nil {
				return nil, err
			}
		}
	}
	la, err := mkdirAll(root, "local/akam")
	if err != nil {
		return nil, err
	}
	la.Mode = 0o755
	for _, n := range licenseNames {
		if err := putFile(root, "local/akam/"+n, 0o644, k.File("licenses/"+n)); err != nil {
			return nil, err
		}
	}

	b.say("Patching sbin/service.sh (telnet/ftp off, SD hook guarded)...")
	svc := lookup(root, "sbin/service.sh")
	if svc == nil || svc.Type != squashfs.TypeFile {
		return nil, errors.New("sbin/service.sh not found in the stock APP")
	}
	var serviceDiff string
	patched, perr := patchService(svc.Data, b.note)
	var pe *patchError
	switch {
	case perr == nil:
		serviceDiff = unifiedDiff("a/sbin/service.sh", "b/sbin/service.sh", string(svc.Data), string(patched))
		svc.Data = patched
	case errors.As(perr, &pe) && (pe.code == 2 || pe.code == 3):
		if !o.AllowUnpatchedService {
			return nil, fmt.Errorf("sbin/service.sh was not patched (%s): the image would not be locked down. Fix the patterns, or allow an unpatched service.sh explicitly", pe.msg)
		}
		b.warn(fmt.Sprintf("*** service.sh NOT patched (exit %d): Telnet/FTP may still be launched by the stock boot script. AKAM_ALLOW_UNPATCHED_SERVICE=1 set, continuing. ***", pe.code))
	default:
		return nil, perr
	}

	b.say("Patching sbin/ethernet.sh (DHCP hostname from akam_hostname.sh)...")
	eth := lookup(root, "sbin/ethernet.sh")
	if eth == nil || eth.Type != squashfs.TypeFile {
		return nil, errors.New("patch_ethernet_hostname failed: sbin/ethernet.sh not found")
	}
	ep, err := patchEthernet(eth.Data, b.note)
	if err != nil {
		return nil, fmt.Errorf("patch_ethernet_hostname failed: the DHCP hostname of ethernet.sh would stay hard-coded (%w)", err)
	}
	eth.Data = ep

	b.say("Slowing sbin/drop_caches.sh (5 s -> 60 s)...")
	if dc := lookup(root, "sbin/drop_caches.sh"); dc == nil || dc.Type != squashfs.TypeFile {
		b.warn("sbin/drop_caches.sh not found in the APP tree: not patched")
	} else if nd, _, reason := patchDropCaches(dc.Data); reason != "" {
		b.warn(reason)
	} else {
		dc.Data = nd
	}

	b.say("Replacing klogd in sbin/main.sh with the kernel log filter...")
	if ms := lookup(root, "sbin/main.sh"); ms == nil || ms.Type != squashfs.TypeFile {
		b.warn("sbin/main.sh not found in the APP tree: kernel log filter not installed")
	} else if nd, _, reason := patchKlogd(ms.Data); reason != "" {
		b.warn(reason)
	} else {
		if err := shSyntax(nd); err != nil && !errors.Is(err, errNoSh) {
			return nil, fmt.Errorf("sbin/main.sh fails sh -n after the klogd patch: %w", err)
		}
		ms.Data = nd
	}

	// MANIFEST of the kit travels with the image
	if err := putFile(root, "local/akam/MANIFEST.txt", 0o644, k.File("MANIFEST.txt")); err != nil {
		return nil, err
	}

	// 4. safety checks
	b.say("Checking tree...")
	if err := b.safety(root); err != nil {
		return nil, err
	}

	// all-root, no extended attributes; the times come from Options (-all-time)
	walk(root, "", func(_ string, n *squashfs.Node) { n.UID, n.GID = 0, 0 })

	// 5. squashfs
	plan := plannedListing(root, o.Epoch)
	sq, _, err := squashfs.Build(root, squashfs.Options{BlockSize: stockBlock, Time: o.Epoch, AllTime: true, MaxSize: PartitionSize})
	if err != nil {
		if errors.Is(err, squashfs.ErrLimit) {
			return nil, fmt.Errorf("the squashfs does not fit in the %d byte APP partition (%v). Nothing padded", PartitionSize, err)
		}
		return nil, fmt.Errorf("squashfs write failed: %w", err)
	}

	// 6. size guard (before padding)
	sqSize := len(sq)
	free := PartitionSize - sqSize
	if free < stockBlock {
		if o.AllowTight {
			b.warn(fmt.Sprintf("*** only %d bytes free in the APP partition (< one block); AKAM_ALLOW_TIGHT=1: building anyway, an image this full may not mount or leave no room for the camera. ***", free))
		} else {
			return nil, fmt.Errorf("only %d bytes free in the APP partition (< %d, one block of margin). Set AKAM_ALLOW_TIGHT=1 to accept", free, stockBlock)
		}
	}

	// 7. pad with 0xFF (the erased-flash value) like pad_build.sh
	img := make([]byte, PartitionSize)
	copy(img, sq)
	for i := sqSize; i < len(img); i++ {
		img[i] = 0xFF
	}
	if !bytes.Equal(img[:sqSize], sq) || len(bytes.Trim(img[sqSize:], "\xff")) != 0 {
		return nil, errors.New("internal error: the padded image does not start with the squashfs or its padding is not all 0xFF")
	}

	// 8. round-trip (gate step 9): the reader of this program re-reads the PADDED image, the very bytes
	// that will be flashed, decodes every block (Verify) and the whole tree must equal the plan
	b.say("Round-trip verifying...")
	got, err := roundTrip(img, stockBlock, plan)
	if err != nil {
		return nil, err
	}
	b.say(fmt.Sprintf("Round-trip verify OK (%d entries).", len(got)))
	now := o.Now
	if now.IsZero() {
		now = time.Now()
	}
	res := &Result{Image: img, Name: "ak_rtsp_firmware_" + now.Format("20060102_150405") + ".squashfs",
		SHA256: sha(img), SquashSize: sqSize, SquashSHA: sha(sq), Free: free, Warnings: b.warns,
		ServiceDiff: serviceDiff, Listing: got, DroppedXattrs: rep.DroppedXattrs, Tree: root}

	// 9. report
	res.Report = b.report(res, root, stockSHA, stockComp, stockBlock, stockBCJ, hv, hd)
	return res, nil
}

// ------------------------------------------------------------------ safety checks

var privKeyRe = regexp.MustCompile(`BEGIN [A-Z0-9 ]*PRIVATE KEY`)

func hasPrivateKeyPEM(data []byte) bool {
	if bytes.IndexByte(data, 0) >= 0 { // grep -I: a binary file is skipped
		return false
	}
	return privKeyRe.Match(data)
}

// requiredModes are the files that must be mode 755 in the image.
var requiredModes = []string{"bin/ak_rtsp", "bin/dropbearmulti", "sbin/ak_rtsp_wrapper.sh", "sbin/akam_ssh.sh", "sbin/akam_lockdown.sh",
	"sbin/akam_rtsp_auth.sh", "sbin/akam_hostname.sh", "sbin/akam_debug.sh", "sbin/akam_onvif.sh", "sbin/akam_net.sh", "sbin/akam_ntp.sh", "sbin/akam_mqtt.sh"}

func (b *builder) safety(root *squashfs.Node) error {
	var bad []string
	walk(root, "", func(p string, n *squashfs.Node) {
		base := path.Base(p)
		if strings.Contains(base, "_host_key") || strings.HasPrefix(base, "id_rsa") || strings.HasPrefix(base, "id_ed25519") {
			bad = append(bad, "./"+p)
		} else if n.Type == squashfs.TypeFile && hasPrivateKeyPEM(n.Data) {
			bad = append(bad, "./"+p)
		}
	})
	if len(bad) > 0 {
		sort.Strings(bad)
		return fmt.Errorf("private key material found in the image tree (O-5: none allowed):\n%s", strings.Join(bad, "\n"))
	}
	for _, f := range requiredModes {
		n := lookup(root, f)
		if n == nil {
			return fmt.Errorf("cannot stat %s", f)
		}
		if n.Mode != 0o755 {
			return fmt.Errorf("%s is not mode 755", f)
		}
	}
	var badw []string
	if la := lookup(root, "local/akam"); la != nil {
		walk(la, "local/akam", func(p string, n *squashfs.Node) {
			if (n.Type == squashfs.TypeFile || n.Type == squashfs.TypeDir) && n.Mode&0o022 != 0 {
				badw = append(badw, p)
			}
		})
	}
	if len(badw) > 0 {
		return fmt.Errorf("group/other-writable files in local/akam: %s", strings.Join(badw, " "))
	}
	for _, f := range append(append([]string{}, requiredModes...), "sbin/service.sh") {
		if n := lookup(root, f); n == nil || n.Mode&0o022 != 0 {
			return fmt.Errorf("%s is group/other-writable or missing", f)
		}
	}
	var w []string
	for _, d := range []string{"sbin", "bin"} {
		if dn := child(root, d); dn != nil {
			walk(dn, d, func(p string, n *squashfs.Node) {
				if n.Type == squashfs.TypeFile && n.Mode&0o022 != 0 {
					w = append(w, p)
				}
			})
		}
	}
	if len(w) > 0 {
		if len(w) > 5 {
			w = w[:5]
		}
		b.warn("stock files writable by group/others in bin/ or sbin/ (not changed): " + strings.Join(w, " ") + " ")
	}
	return nil
}

// ------------------------------------------------------------------ plan and round-trip

type planEntry struct {
	Type         squashfs.FileType
	Mode         uint16
	UID, GID     uint32
	MTime        int64
	Size         int64
	Target       string
	Major, Minor uint32
	SHA          string
}

func plannedListing(root *squashfs.Node, epoch uint32) map[string]planEntry {
	m := map[string]planEntry{}
	walk(root, "", func(p string, n *squashfs.Node) {
		e := planEntry{Type: n.Type, Mode: n.Mode, MTime: int64(epoch), Major: n.Major, Minor: n.Minor}
		switch n.Type {
		case squashfs.TypeFile:
			e.Size, e.SHA = int64(len(n.Data)), sha(n.Data)
		case squashfs.TypeSymlink:
			e.Size, e.Target = int64(len(n.Target)), n.Target
		}
		m["/"+p] = e
	})
	return m
}

// roundTrip re-reads the squashfs with the reader, decodes every block (Verify) and compares the whole
// listing with the plan: paths, types, modes, owners (all root), times (all the epoch), link targets,
// sizes and sha256 of every file.
func roundTrip(sq []byte, block int, plan map[string]planEntry) ([]squashfs.ListEntry, error) {
	r, err := squashfs.Open(bytes.NewReader(sq), int64(len(sq)))
	if err != nil {
		return nil, fmt.Errorf("round-trip verification failed: the image cannot be read back: %w", err)
	}
	inf := r.Info()
	if inf.CompressionName() != "xz" {
		return nil, errors.New("squashfs compression is not xz")
	}
	if int(inf.BlockSize) != block {
		return nil, fmt.Errorf("squashfs block size is not %d", block)
	}
	if err := r.Verify(); err != nil {
		return nil, fmt.Errorf("round-trip verification failed: %w", err)
	}
	list, err := r.Listing()
	if err != nil {
		return nil, fmt.Errorf("round-trip verification failed: %w", err)
	}
	seen := map[string]bool{}
	for _, e := range list {
		p := e.Path
		want, ok := plan[p]
		if !ok {
			return nil, fmt.Errorf("round-trip verification mismatch (types/modes/links/contents), aborting: unexpected entry %s", e.Path)
		}
		seen[p] = true
		got := planEntry{Type: e.Type, Mode: e.Mode, UID: e.UID, GID: e.GID, MTime: e.MTime, Size: e.Size, Target: e.Target,
			Major: e.Major, Minor: e.Minor, SHA: e.SHA256}
		if got != want {
			return nil, fmt.Errorf("round-trip verification mismatch (types/modes/links/contents), aborting: %s: image %+v, plan %+v", e.Path, got, want)
		}
	}
	if len(seen) != len(plan) {
		return nil, fmt.Errorf("round-trip verification mismatch (types/modes/links/contents), aborting: %d entries planned, %d read back", len(plan), len(seen))
	}
	return list, nil
}

// ------------------------------------------------------------------ report

func (b *builder) report(r *Result, root *squashfs.Node, stockSHA, stockComp string, stockBlock int, stockBCJ, hv, hd string) string {
	o := b.o
	var w strings.Builder
	p := func(f string, a ...any) { fmt.Fprintf(&w, f+"\n", a...) }
	free := r.Free
	p("Akam firmware build report")
	p("image:              %s", r.Name)
	p("image sha256:       %s", r.SHA256)
	p("squashfs sha256:    %s  (content, before padding)", r.SquashSHA)
	p("compression:        xz, block %d, bcj none", stockBlock)
	p("build mode:         kit")
	p("time64 gate:        %s", Time64Note)
	p("kit:                akam_version=%s source_commit=%s kit_grade=%s", o.Kit.Version(), o.Kit.Commit(), o.Kit.Grade())
	if o.KitInfo != "" {
		p("kit authentication: %s; %s", o.KitAuth, o.KitInfo)
	} else {
		p("kit authentication: %s", o.KitAuth)
	}
	p("stock APP:          squashfs %s, block %d, bcj %s (from %s)", stockComp, stockBlock, stockBCJ, o.Stock.Name)
	p("stock APP sha256:   %s", stockSHA)
	p("compression vs stock: SAME as the stock APP")
	kern := o.Stock.Kernel
	if kern == "" {
		kern = "unknown (no mtdblock4.bin given or not a uImage)"
	}
	p("stock kernel:       %s", kern)
	p("hardware verdict:   %s (%s)", hv, hd)
	p("squashfs size:      %d bytes", r.SquashSize)
	p("free in mtd7:       %d bytes (%d%% of %d)", free, free*100/PartitionSize, PartitionSize)
	p("added components (uncompressed file size):")
	for _, f := range append(append([]string{}, requiredModes...), "local/akam/LICENSE.dropbear", "local/akam/LICENSE.musl", "local/akam/LICENSE.smolrtsp", "local/akam/MANIFEST.txt") {
		n := lookup(root, f)
		sz := 0
		if n != nil {
			sz = len(n.Data)
		}
		p("  %-36s %9d bytes", f, sz)
	}
	p("ak_rtsp sha256:     %s", sha(lookup(root, "bin/ak_rtsp").Data))
	p("dropbearmulti sha256: %s", sha(lookup(root, "bin/dropbearmulti").Data))
	p("10 largest files in the image:")
	var big []string
	walk(root, "", func(pth string, n *squashfs.Node) {
		if n.Type == squashfs.TypeFile {
			big = append(big, fmt.Sprintf("%d ./%s", len(n.Data), pth))
		}
	})
	// sort -rn: by size, then (last resort) the whole line, both descending
	sort.Slice(big, func(i, j int) bool {
		si, _ := strconv.Atoi(big[i][:strings.IndexByte(big[i], ' ')])
		sj, _ := strconv.Atoi(big[j][:strings.IndexByte(big[j], ' ')])
		if si != sj {
			return si > sj
		}
		return big[i] > big[j]
	})
	if len(big) > 10 {
		big = big[:10]
	}
	for _, l := range big {
		p("  %s", l)
	}
	p("service.sh patch diff: service_sh.diff")
	p("warnings: %d", len(b.warns))
	for _, x := range b.warns {
		p("  - %s", x)
	}
	return w.String()
}

// Time64Note is the "time64 gate" line of the report. The native builder cannot replay the machine-code gate
// (tools/build/check_time32.sh needs gdb): like the bash builder without gdb, it relies on the verified
// kit's MANIFEST (time64_gate=pass) and the sha256 of its binaries.
const Time64Note = "NOT replayed (no gdb, or a test kit): relies on the kit MANIFEST + sha256"

// ------------------------------------------------------------------ in-memory tree helpers

func child(d *squashfs.Node, name string) *squashfs.Node {
	if d == nil {
		return nil
	}
	for _, c := range d.Children {
		if c.Name == name {
			return c
		}
	}
	return nil
}

func removeChild(d *squashfs.Node, name string) {
	if d == nil {
		return
	}
	for i, c := range d.Children {
		if c.Name == name {
			d.Children = append(d.Children[:i], d.Children[i+1:]...)
			return
		}
	}
}

func lookup(root *squashfs.Node, p string) *squashfs.Node {
	n := root
	for _, part := range strings.Split(p, "/") {
		if n == nil || n.Type != squashfs.TypeDir {
			return nil
		}
		n = child(n, part)
	}
	return n
}

// mkdirAll is mkdir -p: missing folders are made 0755.
func mkdirAll(root *squashfs.Node, p string) (*squashfs.Node, error) {
	n := root
	for _, part := range strings.Split(p, "/") {
		c := child(n, part)
		if c == nil {
			c = squashfs.NewDir(part, 0o755)
			n.Add(c)
		} else if c.Type != squashfs.TypeDir {
			return nil, fmt.Errorf("%s: not a directory", part)
		}
		n = c
	}
	return n, nil
}

func parentOf(root *squashfs.Node, p string) (*squashfs.Node, string, error) {
	dir, name := path.Split(p)
	d := root
	if dir != "" {
		var err error
		if d, err = mkdirAll(root, strings.TrimSuffix(dir, "/")); err != nil {
			return nil, "", err
		}
	}
	if old := child(d, name); old != nil && old.Type == squashfs.TypeDir {
		return nil, "", fmt.Errorf("cannot replace the directory %s", p)
	}
	removeChild(d, name)
	return d, name, nil
}

// putFile is `rm -f; install -m`.
func putFile(root *squashfs.Node, p string, mode uint16, data []byte) error {
	d, name, err := parentOf(root, p)
	if err != nil {
		return err
	}
	d.Add(squashfs.NewFile(name, mode, append([]byte(nil), data...)))
	return nil
}

// putLink is `rm -f; ln -s`.
func putLink(root *squashfs.Node, p, target string) error {
	d, name, err := parentOf(root, p)
	if err != nil {
		return err
	}
	d.Add(squashfs.NewSymlink(name, target))
	return nil
}

// walk visits every node below n (n itself first, with the path rel), in name order.
func walk(n *squashfs.Node, rel string, fn func(string, *squashfs.Node)) {
	fn(rel, n)
	kids := append([]*squashfs.Node(nil), n.Children...)
	sort.Slice(kids, func(i, j int) bool { return kids[i].Name < kids[j].Name })
	for _, c := range kids {
		walk(c, path.Join(rel, c.Name), fn)
	}
}
