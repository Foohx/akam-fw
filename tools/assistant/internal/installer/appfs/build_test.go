package appfs

import (
	"bytes"
	"encoding/binary"
	"os"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

func build(t testing.TB, backup []byte, f map[string][]byte) (*BuildResult, error) {
	t.Helper()
	return Build(Inputs{Backup: backup, Kit: mustKit(t, f)})
}

func allZero(b []byte) bool {
	for _, c := range b {
		if c != 0 {
			return false
		}
	}
	return true
}

func treeOf(t testing.TB, img []byte) *squashfs.Node {
	t.Helper()
	fsr, err := squashfs.Open(bytes.NewReader(img), int64(len(img)))
	if err != nil {
		t.Fatal(err)
	}
	n, _, err := fsr.TreeWith(squashfs.TreeOptions{DropXattrs: true})
	if err != nil {
		t.Fatal(err)
	}
	return n
}

func TestBuildHappyPath(t *testing.T) {
	f := kitFiles()
	useBusyBox(t, f)
	stock := stockAppfs(t, stockInit)
	res, err := build(t, stock, f)
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Image) != PartitionSize || string(res.Image[:4]) != "hsqs" {
		t.Fatalf("image %d bytes", len(res.Image))
	}
	if !allZero(res.Image[res.Used:]) {
		t.Error("the padding is not 0x00")
	}
	tree := treeOf(t, res.Image)
	read := func(p string) *squashfs.Node { return find(tree, p) }
	// init.sh: the one branch changed, the override branches untouched
	initSh := string(read("script/init.sh").Data)
	if !strings.Contains(initSh, "/bin/sh /appfs/akam/akam_flash_boot.sh &") ||
		!strings.Contains(initSh, "debug)\n            /bin/sh /mnt/sdcard/akam_boot.sh &\n            ;;\n        factory)\n            /appfs/script/factory.sh &\n            ;;\n") {
		t.Errorf("init.sh not patched as specified:\n%s", initSh)
	}
	if got := string(read("bin/yy_cam").Data); got != yyCamLauncher || read("bin/yy_cam").Mode != 0o755 {
		t.Errorf("yy_cam is not the launcher: %q", got)
	}
	// the payload and its modes
	for _, s := range overlayScripts {
		if n := read("akam/" + s); n == nil || n.Mode != 0o755 || !bytes.Equal(n.Data, f[pathOverlay+s]) {
			t.Errorf("akam/%s missing, not 755 or changed", s)
		}
	}
	if n := read("akam/bin/akam2k_rtsp.BUILDINFO"); n == nil || n.Mode != 0o644 {
		t.Error("BUILDINFO missing or not 644")
	}
	if got, want := string(read("akam/bin/akam2k_sntp.BUILDINFO").Data), "tool=akam2k_sntp\nsha256="+shaHex(f[pathBin+"akam2k_sntp"])+"\n"; got != want {
		t.Errorf("sntp BUILDINFO %q", got)
	}
	// the vendor part is untouched and everything is root-owned
	if !bytes.Equal(read("lib/libvendor.so").Data, find(treeOf(t, stock), "lib/libvendor.so").Data) {
		t.Error("a vendor file changed")
	}
	walk(tree, func(n *squashfs.Node) {
		if n.UID != 0 || n.GID != 0 {
			t.Errorf("%s not root-owned", n.Name)
		}
	})
	// the superblock is the stock one in kind and the manifest records the inputs
	for _, r := range [][2]int{{0, 4}, {12, 4}, {20, 12}, {56, 8}} {
		if !bytes.Equal(res.Image[r[0]:r[0]+r[1]], stock[r[0]:r[0]+r[1]]) {
			t.Errorf("superblock %v differs", r)
		}
	}
	if binary.LittleEndian.Uint32(res.Image[8:12]) != fixtureTime {
		t.Error("the creation time is not the stock one")
	}
	for _, w := range []string{"sha256=" + res.SHA256, "appfs_sign=" + res.Sign, "stock_backup_sha256=" + shaHex(stock), "busybox_sha256=" + busyBoxPin} {
		if !strings.Contains(res.Manifest, w) {
			t.Errorf("manifest lacks %s", w)
		}
	}
	// reproducible: same inputs, same bytes
	again, err := build(t, stock, f)
	if err != nil || !bytes.Equal(again.Image, res.Image) {
		t.Error("two builds differ")
	}
}

// setBin replaces a kit binary and keeps its BUILDINFO consistent, so that only the gate under test can refuse it.
func setBin(f map[string][]byte, name string, data []byte) {
	old := shaHex(f[pathBin+name])
	f[pathBin+name] = data
	for _, bi := range []string{"akam2k_rtsp.BUILDINFO", "dropbearmulti.BUILDINFO"} {
		f[pathBin+bi] = bytes.ReplaceAll(f[pathBin+bi], []byte(old), []byte(shaHex(data)))
	}
}

// Every refusal of build_appfs.sh is a test: nothing is built from a doubtful input.
func TestBuildRefusals(t *testing.T) {
	stock := func() []byte { return stockAppfs(t, stockInit) }
	cases := []struct {
		name   string
		backup func() []byte
		kit    func(map[string][]byte)
		want   string
	}{
		{"backup not a squashfs", func() []byte { b := stock(); b[0] = 'x'; return b }, nil, "not a squashfs"},
		{"backup too short", func() []byte { return stock()[:PartitionSize-1] }, nil, "not 4784128"},
		{"backup empty", func() []byte { return nil }, nil, "not a squashfs"},
		{"init branch missing", func() []byte { return stockAppfs(t, strings.Replace(stockInit, "normal)", "other)", 1)) }, nil, "exactly once"},
		{"init branch twice", func() []byte { return stockAppfs(t, stockInit+stockInit[strings.Index(stockInit, "        normal)"):]) }, nil, "exactly once"},
		{"busybox not the pinned one", stock, func(f map[string][]byte) { f[pathBin+"busybox"] = []byte("other") }, "not the pinned"},
		{"symcheck failed", stock, func(f map[string][]byte) {
			f[pathBin+"akam2k_rtsp.BUILDINFO"] = bytes.Replace(f[pathBin+"akam2k_rtsp.BUILDINFO"], []byte("symcheck=passed"), []byte("symcheck=failed"), 1)
		}, "symcheck=passed"},
		{"rtsp differs from its BUILDINFO", stock, func(f map[string][]byte) { f[pathBin+"akam2k_rtsp"] = elf("other") }, "akam2k_rtsp does not match"},
		{"sntp differs from its BUILDINFO", stock, func(f map[string][]byte) { f[pathBin+"akam2k_sntp"] = elf("other") }, "akam2k_sntp does not match"},
		{"rtsp without uClibc interpreter", stock, func(f map[string][]byte) { setBin(f, "akam2k_rtsp", append([]byte("\x7fELF"), make([]byte, 100)...)) }, "ld-uClibc"},
		{"sntp not ARM", stock, func(f map[string][]byte) {
			b := elf("sntp")
			b[18] = 62 // x86-64
			setBin(f, "akam2k_sntp", b)
		}, "not an ARM executable"},
		{"dropbear differs from its BUILDINFO", stock, func(f map[string][]byte) { f[pathBin+"dropbearmulti"] = elf("x") }, "dropbearmulti does not match"},
		{"dropbear without dropbearkey", stock, func(f map[string][]byte) {
			f[pathBin+"dropbearmulti.BUILDINFO"] = []byte("sha256_stripped=" + shaHex(f[pathBin+"dropbearmulti"]) + "\nprograms=dropbear scp\n")
		}, "lacks dropbearkey"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			f := kitFiles()
			useBusyBox(t, f)
			if c.kit != nil {
				c.kit(f)
				sealKit(f)
			}
			res, err := Build(Inputs{Backup: c.backup(), Kit: mustKit(t, f)})
			if err == nil {
				t.Fatalf("built an image (%d bytes) from a refused input", len(res.Image))
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Errorf("error %q, want %q", err, c.want)
			}
		})
	}
	t.Run("no kit", func(t *testing.T) {
		if _, err := Build(Inputs{Backup: stock()}); err == nil {
			t.Fatal("built without a kit")
		}
	})
}

func TestBuildRefusesAnImageThatDoesNotFit(t *testing.T) {
	f := kitFiles()
	useBusyBox(t, f)
	big := make([]byte, 6<<20)
	x := uint32(1)
	for i := range big { // incompressible
		x = x*1664525 + 1013904223
		big[i] = byte(x >> 24)
	}
	setBin(f, "dropbearmulti", append(elf("dropbear"), big...))
	sealKit(f)
	if _, err := build(t, stockAppfs(t, stockInit), f); err == nil || !strings.Contains(err.Error(), "does not fit") {
		t.Fatalf("got %v", err)
	}
}

// AKAM_APPFS_REAL_BACKUP=/path/to/mtd3.bin go test -run TestRealBackup -v ./internal/installer/appfs
// builds from a real stock appfs dump (the user's own, never committed) with the fixture kit: the stock superblock
// invariants, the init.sh branch and the size budget must hold on the real image.
func TestRealBackup(t *testing.T) {
	p := os.Getenv("AKAM_APPFS_REAL_BACKUP")
	if p == "" {
		t.Skip("AKAM_APPFS_REAL_BACKUP not set")
	}
	stock, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	f := kitFiles()
	useBusyBox(t, f)
	res, err := build(t, stock, f)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("real stock %s -> image %s, %d bytes used of %d", shaHex(stock), res.SHA256, res.Used, PartitionSize)
}

func TestBusyBoxPin(t *testing.T) {
	if BusyBoxSHA256 != "367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290" || busyBoxPin != BusyBoxSHA256 {
		t.Fatal("the busybox pin of build_appfs.sh changed")
	}
	if _, err := build(t, stockAppfs(t, stockInit), kitFiles()); err == nil || !strings.Contains(err.Error(), "not the pinned") {
		t.Fatalf("a kit whose busybox is not the pinned one built: %v", err)
	}
}

func TestSign(t *testing.T) {
	img := make([]byte, PartitionSize)
	copy(img, "hsqs")
	binary.LittleEndian.PutUint64(img[40:], 100)
	a, err := Sign(img)
	if err != nil || len(a) != 32 {
		t.Fatalf("%q %v", a, err)
	}
	img[200] = 1 // inside the first 0x4000 bytes
	if b, _ := Sign(img); b == a {
		t.Error("the sign ignores the data")
	}
	img[200] = 0
	img[0x4000+5] = 9 // beyond bytes_used rounded up to 16 KiB: not part of the sign
	if b, _ := Sign(img); b != a {
		t.Error("the sign covers more than bytes_used rounded to 16 KiB")
	}
	if _, err := Sign([]byte("nope")); err == nil {
		t.Error("a non squashfs was signed")
	}
}
