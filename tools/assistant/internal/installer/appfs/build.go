package appfs

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

// The init.sh branch build_appfs.sh changes (exactly one occurrence in the stock file) and its replacement.
const (
	initOld = "        normal)\n            /appfs/script/start.sh &\n            ;;\n"
	initNew = "        normal)\n" +
		"            # AKAM: no override card (no yy_debug.ini, or mode=normal): start Akam from THIS image. The\n" +
		"            # override modes below (debug, factory) are untouched stock code, so the Akam SD card\n" +
		"            # (mode=debug, start_sh=/mnt/sdcard/akam_boot.sh) always replaces the flashed app = recovery.\n" +
		"            # Without the launcher (a broken image) fall back to the plain stock hand-off.\n" +
		"            if [ -f /appfs/akam/akam_flash_boot.sh ]; then\n" +
		"                /bin/sh /appfs/akam/akam_flash_boot.sh &\n" +
		"            else\n" +
		"                /appfs/script/start.sh &\n" +
		"            fi\n" +
		"            ;;\n"
	yyCamLauncher = "#!/bin/sh\n" +
		"# Akam card-less image: stand-in for the stock yy_cam (see /appfs/akam/own_app_stub.sh).\n" +
		"exec /bin/sh /appfs/akam/own_app_stub.sh\n"
)

// busyBoxPin is BusyBoxSHA256; a variable only so that the tests, which cannot ship the real busybox, can pin a fake.
var busyBoxPin = BusyBoxSHA256

// Inputs of Build.
type Inputs struct {
	// Backup is the user's own mtd3 dump: a full 0x490000 byte squashfs.
	Backup []byte
	// Kit is the verified appfs kit (Akam code only).
	Kit *Kit
	// Say gets progress lines (may be nil).
	Say func(string)
}

// BuildResult of a successful Build.
type BuildResult struct {
	// Image is the padded image: exactly PartitionSize bytes (padding 0x00, like the stock dump).
	Image []byte
	// SHA256 is the sha256 of Image; Sign the appfs_sign nvram value; Used the squashfs bytes_used.
	SHA256, Sign string
	Used         uint64
	// Manifest is MANIFEST.txt of the output directory.
	Manifest string
	// Tree is the tree the image was written from.
	Tree *squashfs.Node
}

// Build is the port of appfs/build_appfs.sh: the stock appfs of the user's backup, with ONE branch of init.sh
// changed, the Akam payload added under /appfs/akam and bin/yy_cam replaced by a launcher of the stub. The whole
// tree is read and written in memory (no unsquashfs, no mksquashfs, no shell: the same on Linux, macOS and
// Windows). Every proof of the shell script is kept; the two that compared the kit's binaries with the sources of
// a checkout (source_sha256) are attested by the kit's hashed, signed MANIFEST instead. Nothing here touches a camera.
func Build(in Inputs) (*BuildResult, error) {
	say := in.Say
	if say == nil {
		say = func(string) {}
	}
	if in.Kit == nil {
		return nil, errors.New("appfs: a kit is required")
	}
	b := in.Backup
	if len(b) < 4 || string(b[:4]) != squashMagic {
		return nil, errors.New("the backup is not a squashfs (magic hsqs)")
	}
	if len(b) != PartitionSize {
		return nil, fmt.Errorf("the backup is %d bytes, not %d (a full mtd3 dump)", len(b), PartitionSize)
	}
	if err := checkKitProofs(in.Kit); err != nil {
		return nil, err
	}
	// the stock image's creation time (superblock bytes 8..11), reused so the output does not depend on the build date
	mkfsTime := binary.LittleEndian.Uint32(b[8:12])

	fsr, err := squashfs.Open(bytes.NewReader(b), int64(len(b)))
	if err != nil {
		return nil, fmt.Errorf("the backup is not a squashfs this builder can read: %w", err)
	}
	info := fsr.Info()
	if info.CompressionName() != "xz" {
		return nil, fmt.Errorf("the stock appfs uses squashfs compression %q, the builder writes xz only", info.CompressionName())
	}
	root, _, err := fsr.TreeWith(squashfs.TreeOptions{DropXattrs: true})
	if err != nil {
		return nil, fmt.Errorf("cannot read the stock appfs tree: %w", err)
	}
	if find(root, "script/init.sh") == nil || find(root, "script/start.sh") == nil || find(root, "bin/yy_cam") == nil {
		return nil, errors.New("the backup does not look like the stock appfs (script/init.sh, script/start.sh, bin/yy_cam)")
	}
	say("stock appfs read in memory")

	// init.sh: the stock file with the one branch changed
	initSh := find(root, "script/init.sh")
	if initSh.Type != squashfs.TypeFile {
		return nil, errors.New("script/init.sh is not a regular file")
	}
	if strings.Count(string(initSh.Data), initOld) != 1 {
		return nil, errors.New("init.sh: the stock 'normal)' branch was not found exactly once")
	}
	initSh.Data = []byte(strings.Replace(string(initSh.Data), initOld, initNew, 1))

	// the Akam payload
	if find(root, "akam") != nil {
		return nil, errors.New("the stock appfs already has /akam: not a stock image")
	}
	k := in.Kit
	ak := squashfs.NewDir("akam", 0o755)
	bin := squashfs.NewDir("bin", 0o755)
	ak.Add(bin)
	for _, f := range overlayScripts {
		ak.Add(squashfs.NewFile(f, 0o755, k.File(pathOverlay+f)))
	}
	ak.Add(squashfs.NewFile("dropbearmulti", 0o755, k.File(pathBin+"dropbearmulti")))
	ak.Add(squashfs.NewFile("busybox", 0o755, k.File(pathBin+"busybox")))
	bin.Add(squashfs.NewFile("akamd", 0o755, k.File(pathBin+"akamd")))
	bin.Add(squashfs.NewFile("akamd.BUILDINFO", 0o644, k.File(pathBin+"akamd.BUILDINFO")))
	bin.Add(squashfs.NewFile("akam-sntp", 0o755, k.File(pathBin+"akam-sntp")))
	sntpSHA := buildinfo(k.File(pathBin+"akamd.BUILDINFO"), "sntp_sha256")
	bin.Add(squashfs.NewFile("akam-sntp.BUILDINFO", 0o644, []byte("tool=akam-sntp\nsha256="+sntpSHA+"\n")))
	root.Add(ak)

	// yy_cam: a launcher of the stub (start.sh runs it; the SD overlay bind-mounts its own stub over it)
	yy := find(root, "bin/yy_cam")
	*yy = *squashfs.NewFile("yy_cam", 0o755, []byte(yyCamLauncher))

	// all-root, the stock times come from Options (-all-time)
	walk(root, func(n *squashfs.Node) { n.UID, n.GID = 0, 0 })

	sq, _, err := squashfs.Build(root, squashfs.Options{BlockSize: int(info.BlockSize), Time: mkfsTime, AllTime: true, MaxSize: PartitionSize, XattrsAllowed: true})
	if err != nil {
		if errors.Is(err, squashfs.ErrLimit) {
			return nil, fmt.Errorf("the image does not fit the %d byte partition: %w", PartitionSize, err)
		}
		return nil, fmt.Errorf("squashfs write failed: %w", err)
	}
	used := binary.LittleEndian.Uint64(sq[40:48])
	// pad with 0x00 to the full partition (the stock dump is padded the same way)
	img := make([]byte, PartitionSize)
	copy(img, sq)
	if err := verifyImage(img, b, root); err != nil {
		return nil, err
	}
	sign, err := Sign(img)
	if err != nil {
		return nil, err
	}
	res := &BuildResult{Image: img, SHA256: sum(img), Sign: sign, Used: used, Tree: root}
	res.Manifest = manifest(res, b, k, sum(initSh.Data), mkfsTime)
	say(fmt.Sprintf("image built: %d bytes used by the squashfs, padded to %d, sha256 %s", used, PartitionSize, res.SHA256))
	return res, nil
}

// checkKitProofs are the input proofs of build_appfs.sh that do not need a source checkout.
func checkKitProofs(k *Kit) error {
	if sum(k.File(pathBin+"busybox")) != busyBoxPin {
		return fmt.Errorf("busybox is not the pinned %s", busyBoxPin)
	}
	bi := k.File(pathBin + "akamd.BUILDINFO")
	if buildinfo(bi, "symcheck") != "passed" {
		return errors.New("akamd.BUILDINFO does not say symcheck=passed")
	}
	if buildinfo(bi, "sha256") != sum(k.File(pathBin+"akamd")) {
		return errors.New("akamd does not match its BUILDINFO")
	}
	if buildinfo(bi, "sntp_sha256") != sum(k.File(pathBin+"akam-sntp")) {
		return errors.New("akam-sntp does not match its BUILDINFO")
	}
	for _, n := range []string{"akamd", "akam-sntp"} {
		f := k.File(pathBin + n)
		head := f[:min(len(f), 4096)]
		if !bytes.Contains(head, []byte("/lib/ld-uClibc.so.0")) {
			return fmt.Errorf("%s does not request /lib/ld-uClibc.so.0", n)
		}
		if len(f) < 20 || f[18] != 40 || f[19] != 0 {
			return fmt.Errorf("%s is not an ARM executable", n)
		}
	}
	db := k.File(pathBin + "dropbearmulti.BUILDINFO")
	if buildinfo(db, "sha256_stripped") != sum(k.File(pathBin+"dropbearmulti")) {
		return errors.New("dropbearmulti does not match its BUILDINFO")
	}
	progs := " " + buildinfo(db, "programs") + " "
	for _, p := range []string{"dropbear", "dropbearkey"} {
		if !strings.Contains(progs, " "+p+" ") {
			return fmt.Errorf("dropbearmulti lacks %s", p)
		}
	}
	return nil
}

// buildinfo is the first value of key in a BUILDINFO file ("" when absent).
func buildinfo(b []byte, key string) string {
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimRight(l, "\r"), key+"="); ok {
			return v
		}
	}
	return ""
}

func find(root *squashfs.Node, p string) *squashfs.Node {
	n := root
	for _, part := range strings.Split(p, "/") {
		var next *squashfs.Node
		if n.Type == squashfs.TypeDir {
			for _, c := range n.Children {
				if c.Name == part {
					next = c
				}
			}
		}
		if next == nil {
			return nil
		}
		n = next
	}
	return n
}

func walk(n *squashfs.Node, fn func(*squashfs.Node)) {
	fn(n)
	for _, c := range n.Children {
		walk(c, fn)
	}
}

// verifyImage is the shell script's closing proofs: the padded image parses and decodes completely, its superblock
// equals the stock one in kind (the camera kernel cannot mount it before it is flashed: magic, block size, compressor,
// block log, flags, id count, version, xattr table start; only sizes, counts and table offsets may differ), and the
// tree read back from it equals the tree it was built from.
func verifyImage(img, stock []byte, want *squashfs.Node) error {
	fsr, err := squashfs.Open(bytes.NewReader(img), int64(len(img)))
	if err != nil {
		return fmt.Errorf("the built image does not parse: %w", err)
	}
	if err := fsr.Verify(); err != nil {
		return fmt.Errorf("the built image does not decode: %w", err)
	}
	for _, r := range [][2]int{{0, 4}, {12, 4}, {20, 12}, {56, 8}} {
		if !bytes.Equal(img[r[0]:r[0]+r[1]], stock[r[0]:r[0]+r[1]]) {
			return fmt.Errorf("superblock bytes %d..%d differ from the stock image", r[0], r[0]+r[1])
		}
	}
	got, _, err := fsr.TreeWith(squashfs.TreeOptions{DropXattrs: true})
	if err != nil {
		return fmt.Errorf("the built image cannot be listed: %w", err)
	}
	if d := diffTree("", want, got); d != "" {
		return fmt.Errorf("the extracted image differs from the tree it was built from: %s", d)
	}
	return nil
}

func diffTree(p string, a, b *squashfs.Node) string {
	if a.Type != b.Type || a.Mode != b.Mode || a.UID != b.UID || a.GID != b.GID || a.Target != b.Target ||
		a.Major != b.Major || a.Minor != b.Minor || !bytes.Equal(a.Data, b.Data) {
		return "/" + p
	}
	if len(a.Children) != len(b.Children) {
		return "/" + p + " (entries)"
	}
	ac := append([]*squashfs.Node(nil), a.Children...)
	bc := append([]*squashfs.Node(nil), b.Children...)
	sort.Slice(ac, func(i, j int) bool { return ac[i].Name < ac[j].Name })
	sort.Slice(bc, func(i, j int) bool { return bc[i].Name < bc[j].Name })
	for i := range ac {
		if ac[i].Name != bc[i].Name {
			return "/" + p + "/" + ac[i].Name
		}
		if d := diffTree(strings.TrimPrefix(p+"/"+ac[i].Name, "/"), ac[i], bc[i]); d != "" {
			return d
		}
	}
	return ""
}

func manifest(r *BuildResult, stock []byte, k *Kit, initSHA string, mkfs uint32) string {
	bi := k.File(pathBin + "akamd.BUILDINFO")
	db := k.File(pathBin + "dropbearmulti.BUILDINFO")
	var sb strings.Builder
	line := func(f string, a ...any) { fmt.Fprintf(&sb, f+"\n", a...) }
	line("image=appfs-akam.squashfs")
	line("size=%d (partition %d, padded with 0x00)", len(r.Image), PartitionSize)
	line("squashfs_bytes_used=%d (0x%x)", r.Used, r.Used)
	line("sha256=%s", r.SHA256)
	line("appfs_sign=%s", r.Sign)
	line("builder=akam assistant (in-process squashfs writer)")
	line("params=-comp xz -b 131072 -noappend -all-root -mkfs-time %d -all-time %d", mkfs, mkfs)
	line("stock_backup_sha256=%s", sum(stock))
	line("akamd_sha256=%s", buildinfo(bi, "sha256"))
	line("akam_sntp_sha256=%s", buildinfo(bi, "sntp_sha256"))
	line("app_source_sha256=%s", buildinfo(bi, "source_sha256"))
	line("dropbearmulti_sha256=%s", buildinfo(db, "sha256_stripped"))
	line("busybox_sha256=%s", busyBoxPin)
	line("init_sh_sha256=%s", initSHA)
	line("flash_boot_sha256=%s", sum(k.File(pathOverlay+"akam_flash_boot.sh")))
	line("own_app_stub_sha256=%s", sum(k.File(pathOverlay+"own_app_stub.sh")))
	line("kit_version=%s", k.Version())
	line("kit_commit=%s", k.Commit())
	return sb.String()
}
