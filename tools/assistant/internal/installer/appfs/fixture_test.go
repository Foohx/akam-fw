package appfs

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

const (
	fixtureTime = 1700000000
	time32Shim  = "/* fake time32 shim */\n"
)

func shaHex(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// stockInit is a stand-in of the stock init.sh: only the shape build_appfs.sh patches (one 'normal)' branch) matters.
const stockInit = `#!/bin/sh
# stand-in of the stock init.sh
run() {
    mode=normal
    if [ -f /mnt/sdcard/yy_debug.ini ]; then mode=debug; fi
    case "$mode" in
        debug)
            /bin/sh /mnt/sdcard/akam_boot.sh &
            ;;
        factory)
            /appfs/script/factory.sh &
            ;;
        normal)
            /appfs/script/start.sh &
            ;;
    esac
}
run
`

// stockAppfs builds a stand-in of the user's mtd3 backup: a padded xz squashfs of 128 KiB blocks with the files
// build_appfs.sh requires (and some vendor-looking content that must come out unchanged).
func stockAppfs(t testing.TB, init string) []byte {
	t.Helper()
	lib := make([]byte, 300000)
	for i := range lib {
		lib[i] = byte(i*7 + i>>9)
	}
	root := squashfs.NewDir("", 0o755)
	root.Add(
		squashfs.NewDir("script", 0o755).Add(
			squashfs.NewFile("init.sh", 0o755, []byte(init)),
			squashfs.NewFile("start.sh", 0o755, []byte("#!/bin/sh\necho stock start\n")),
		),
		squashfs.NewDir("bin", 0o755).Add(
			squashfs.NewFile("yy_cam", 0o755, bytes.Repeat([]byte("STOCKAPP"), 5000)),
			squashfs.NewSymlink("sh", "yy_cam"),
		),
		squashfs.NewDir("lib", 0o755).Add(squashfs.NewFile("libvendor.so", 0o644, lib)),
		squashfs.NewDir("conf", 0o700).Add(squashfs.NewFile("a.ini", 0o600, []byte("k=v\n"))),
	)
	sq, _, err := squashfs.Build(root, squashfs.Options{BlockSize: 131072, Time: fixtureTime, AllTime: true, XattrsAllowed: true})
	if err != nil {
		t.Fatal(err)
	}
	img := make([]byte, PartitionSize)
	copy(img, sq)
	return img
}

// elf is a fake ARM uClibc executable: just what the kit proofs look at.
func elf(tag string) []byte {
	b := make([]byte, 600)
	copy(b, "\x7fELF")
	b[18], b[19] = 40, 0
	copy(b[100:], "/lib/ld-uClibc.so.0\x00")
	copy(b[200:], tag)
	return b
}

// kitFiles is a valid fixture kit. busybox is not the pinned one: tests that build use useBusyBox to pin it.
func kitFiles() map[string][]byte {
	f := map[string][]byte{}
	for _, s := range overlayScripts {
		f[pathOverlay+s] = []byte("#!/bin/sh\n# " + s + "\n:\n")
	}
	rtsp, sntp, db, bb := elf("rtsp"), elf("sntp"), elf("dropbear"), []byte("fake busybox for tests\n")
	f[pathBin+"akamd"] = rtsp
	f[pathBin+"akam-sntp"] = sntp
	f[pathBin+"dropbearmulti"] = db
	f[pathBin+"busybox"] = bb
	f[pathBin+"akamd.BUILDINFO"] = []byte(fmt.Sprintf("symcheck=passed\nsha256=%s\nsource_sha256=%s\nsntp_sha256=%s\nsntp_source_sha256=%s\n",
		shaHex(rtsp), "1111", shaHex(sntp), "2222"))
	f[pathBin+"dropbearmulti.BUILDINFO"] = []byte(fmt.Sprintf("sha256_stripped=%s\ntime32_shim_sha256=%s\nprograms=dropbear dropbearkey scp\n", shaHex(db), shaHex([]byte(time32Shim))))
	f[pathManifest] = []byte("model=lsc-rotatable-2k-ak3918av130\nkind=cardless-appfs\nakam_version=0.2.0\nsource_commit=abc123\nkit_grade=release\ntime64_gate=pass\n")
	return sealKit(f)
}

// sealKit (re)writes SHA256SUMS for the files.
func sealKit(f map[string][]byte) map[string][]byte {
	var sums bytes.Buffer
	paths := append(RequiredKitPaths(), cardFiles...)
	for p := range f {
		if optionalKitFile(p) {
			paths = append(paths, p)
		}
	}
	for _, p := range paths {
		if p != pathSums {
			if b, ok := f[p]; ok {
				fmt.Fprintf(&sums, "%s  %s\n", shaHex(b), p)
			}
		}
	}
	f[pathSums] = sums.Bytes()
	return f
}

// useBusyBox pins the kit's fake busybox for the duration of a test (the real pin is checked by TestBusyBoxPin).
func useBusyBox(t testing.TB, f map[string][]byte) {
	t.Helper()
	old := busyBoxPin
	busyBoxPin = shaHex(f[pathBin+"busybox"])
	t.Cleanup(func() { busyBoxPin = old })
}

func mustKit(t testing.TB, f map[string][]byte) *Kit {
	t.Helper()
	k, err := NewKit(f)
	if err != nil {
		t.Fatal(err)
	}
	return k
}
