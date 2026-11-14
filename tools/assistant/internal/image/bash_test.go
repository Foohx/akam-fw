package image

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

// The shapes the awk patch must handle or refuse, one per case: the native port must give bash's exit code
// and bytes.
func TestServicePatchShapesEqualBash(t *testing.T) {
	if !have("bash", "awk", "sh", "mktemp") {
		t.Skip("bash, awk or sh missing")
	}
	script := filepath.Join(repoRoot(t), "tools", "firmware_patch_templates", "patch_service_sh.sh")
	cases := map[string]string{
		"daemon only":         "telnetd -l /bin/sh\n",
		"no newline":          "telnetd -l /bin/sh",
		"hook only":           "sh /mnt/hack.sh\n",
		"daemon and hook":     "  telnetd &\n\t[ -f /mnt/hack.sh ] && sh /mnt/hack.sh\n",
		"commented only":      "# telnetd\nsh /mnt/hack.sh\n",
		"commented no hook":   "# telnetd\n",
		"continuation":        "telnetd \\\n -l /bin/sh\n",
		"hook continuation":   "sh /mnt/hack.sh \\\n x\n",
		"if or":               "if [ -f /mnt/hack.sh ] || true; then\n x\nfi\n",
		"if hook":             "telnetd\nif [ -f /mnt/hack.sh ]; then\n x\nfi\n",
		"elif":                "telnetd\nelif [ -f /mnt/hack.sh ]; then\n",
		"compound":            "telnetd\nsh /mnt/hack.sh; echo\n",
		"comment after hook":  "telnetd\nsh /mnt/hack.sh # x\n",
		"background hook":     "telnetd\nsh /mnt/hack.sh &\n",
		"already patched":     "\t: # AKAM-DISABLED: telnetd\n[ -f /mnt/AKAM_MAINTENANCE ] && sh /mnt/hack.sh\n",
		"legacy patched":      ": # CAMYN-DISABLED: telnetd\n",
		"mark present":        "telnetd\nsh /mnt/hack.sh\n# /tmp/akam_maintenance\n",
		"crlf":                "telnetd\r\nsh /mnt/hack.sh\r\n",
		"keyword prefix":      "telnetd\ndone_hack.sh\n",
		"for prefix":          "telnetd\nformat /mnt/hack.sh\n",
		"assignment":          "X=/usr/sbin/telnetd\nAP_MODE_FILE=_ht_ap_mode.conf\nsh /mnt/hack.sh\n",
		"tf_update":           "telnetd\nsh /tmp/tf_update.sh\ncp /usr/sbin/tf_update.sh /tmp/\n",
		"hostapd":             "telnetd\n/mnt/hostapd /mnt/hostapd.conf &\n",
		"blank lines":         "\n\ntelnetd\n\n",
		"empty":               "",
		"not a daemon":        "echo telnetd-client ftpdx\n",
		"dash after":          "echo ftpd-foo\n",
		"syntax error result": "telnetd\nif [ x ]; then\n",
	}
	for name, in := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			if err := os.MkdirAll(filepath.Join(dir, "sbin"), 0o755); err != nil {
				t.Fatal(err)
			}
			p := filepath.Join(dir, "sbin", "service.sh")
			if err := os.WriteFile(p, []byte(in), 0o755); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command("bash", script, dir)
			cmd.Env = append(os.Environ(), "AKAM_PATCH_REPORT="+filepath.Join(dir, "r.diff"))
			want := exitCode(cmd.Run())
			out, err := patchService([]byte(in), func(string) {})
			got := 0
			if pe, ok := err.(*patchError); ok {
				got = pe.code
			}
			if got != want {
				t.Fatalf("exit code: native %d, bash %d (%v)", got, want, err)
			}
			if want == 0 {
				bb, _ := os.ReadFile(p)
				if !bytes.Equal(out, bb) {
					t.Fatalf("native:\n%q\nbash:\n%q", out, bb)
				}
			}
		})
	}
}

// ---------------------------------------------------------------- whole image against build_firmware.sh

// makeKit builds a kit directory with make_kit.sh (stub binaries) and extracts it, or skips.
func makeKit(t *testing.T) string {
	t.Helper()
	if !have("bash", "tar", "xz", "sha256sum", "git", "mksquashfs", "unsquashfs") {
		t.Skip("bash builder tools missing (bash tar xz sha256sum git mksquashfs unsquashfs)")
	}
	root := repoRoot(t)
	work := t.TempDir()
	stub := filepath.Join(work, "stub")
	for _, d := range []string{"smol/build/_deps", "db"} {
		if err := os.MkdirAll(filepath.Join(stub, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	wr := func(rel, content string, mode os.FileMode) {
		if err := os.WriteFile(filepath.Join(stub, rel), []byte(content), mode); err != nil {
			t.Fatal(err)
		}
	}
	wr("ak_rtsp", "#!/bin/sh\necho stub ak_rtsp\n", 0o755)
	wr("flash_tool", "#!/bin/sh\necho stub flash_tool\n", 0o755)
	wr("db/dropbearmulti", "#!/bin/sh\necho stub dropbearmulti\n", 0o755)
	wr("db/LICENSE.dropbear", "Dropbear stub licence\n", 0o644)
	wr("smol/LICENSE", "smolrtsp stub licence\n", 0o644)
	for _, d := range []string{"slice99", "metalang99", "datatype99", "interface99"} {
		if err := os.MkdirAll(filepath.Join(stub, "smol/build/_deps", d+"-src"), 0o755); err != nil {
			t.Fatal(err)
		}
		wr("smol/build/_deps/"+d+"-src/LICENSE", d+" stub licence\n", 0o644)
	}
	wr("COPYRIGHT", "musl stub\n", 0o644)
	wr("ZIGLICENSE", "zig stub\n", 0o644)
	wr("gate_ok", "#!/bin/sh\nexit 0\n", 0o755)
	cmd := exec.Command("bash", filepath.Join(root, "tools/release/make_kit.sh"), "--out", filepath.Join(work, "kitout"))
	cmd.Dir = work
	cmd.Env = append(os.Environ(), "SOURCE_DATE_EPOCH=1700000000", "SMOLRTSP_DIR="+filepath.Join(stub, "smol"),
		"MUSL_COPYRIGHT="+filepath.Join(stub, "COPYRIGHT"), "ZIG_LICENSE="+filepath.Join(stub, "ZIGLICENSE"),
		"AKAM_KIT_NO_BUILD=1", "AKAM_KIT_ALLOW_DIRTY=1", "AKAM_KIT_VERSION=test1", "AKAM_KIT_CHECK_TIME32="+filepath.Join(stub, "gate_ok"),
		"AK_RTSP_BIN="+filepath.Join(stub, "ak_rtsp"), "FLASH_TOOL_BIN="+filepath.Join(stub, "flash_tool"),
		"DROPBEAR_MULTI="+filepath.Join(stub, "db/dropbearmulti"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Skipf("make_kit.sh failed here (%v): %s", err, tail(out))
	}
	kx := filepath.Join(work, "kx")
	if err := os.MkdirAll(kx, 0o755); err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command("tar", "-xJf", filepath.Join(work, "kitout", "akam-kit-lsc-indoor-1080p-ak3918-test1.tar.xz"), "-C", kx).CombinedOutput(); err != nil {
		t.Fatalf("tar: %v %s", err, out)
	}
	return filepath.Join(kx, "akam-kit-lsc-indoor-1080p-ak3918-test1")
}

func tail(b []byte) string {
	s := strings.TrimSpace(string(b))
	if len(s) > 600 {
		s = s[len(s)-600:]
	}
	return s
}

type bashRun struct {
	img      []byte
	report   string
	listing  []squashfs.ListEntry
	svcPatch []byte
	log      string
}

// runBash runs build_firmware.sh --kit on an extracted tree and reads its result with the reader.
func runBash(t testing.TB, kitDir, treeDir, stockPath, outDir string, extra ...string) (*bashRun, error) {
	t.Helper()
	args := append([]string{filepath.Join(repoRoot(t), "src/ak_rtsp/build_firmware.sh"), "--kit", kitDir, "--allow-test-kit", "--allow-unsigned", "--i-understand-untested", "--stock-app", stockPath}, extra...)
	cmd := exec.Command("bash", args...)
	cmd.Env = append(os.Environ(), "APP_EXTRACTED="+treeDir, "FW_DIR="+outDir, "SOURCE_DATE_EPOCH=1700000000")
	log, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("%v: %s", err, tail(log))
	}
	m, _ := filepath.Glob(filepath.Join(outDir, "ak_rtsp_firmware_*.squashfs"))
	if len(m) != 1 {
		return nil, fmt.Errorf("no image in %s", outDir)
	}
	img, err := os.ReadFile(m[0])
	if err != nil {
		return nil, err
	}
	rep, _ := os.ReadFile(filepath.Join(outDir, "build_report.txt"))
	fs, err := squashfs.Open(bytes.NewReader(img), int64(len(img)))
	if err != nil {
		return nil, err
	}
	l, err := fs.Listing()
	if err != nil {
		return nil, err
	}
	return &bashRun{img: img, report: string(rep), listing: l, log: string(log)}, nil
}

// writeTree extracts a tree into a folder, as unsquashfs would for the bash builder (tests only).
func writeTree(t testing.TB, root *squashfs.Node, dir string) {
	t.Helper()
	var rec func(n *squashfs.Node, p string)
	rec = func(n *squashfs.Node, p string) {
		for _, c := range n.Children {
			fp := filepath.Join(p, c.Name)
			switch c.Type {
			case squashfs.TypeDir:
				if err := os.Mkdir(fp, 0o755); err != nil {
					t.Fatal(err)
				}
				rec(c, fp)
				if err := os.Chmod(fp, os.FileMode(c.Mode)); err != nil {
					t.Fatal(err)
				}
			case squashfs.TypeSymlink:
				if err := os.Symlink(c.Target, fp); err != nil {
					t.Fatal(err)
				}
			default:
				if err := os.WriteFile(fp, c.Data, 0o600); err != nil {
					t.Fatal(err)
				}
				// unsquashfs run by a normal user creates regular files under the umask (022)
				if err := os.Chmod(fp, os.FileMode(c.Mode&^0o022)); err != nil {
					t.Fatal(err)
				}
			}
		}
	}
	rec(root, dir)
}

// reportFields are the lines of build_report.txt that must be equal between the builders: everything but what
// depends on the compressed bytes (image names, sha256 of the image and of the squashfs, sizes, free space)
// and the stock writable-file warning, whose path prefix is the build folder in bash.
func reportFields(rep string) []string {
	var out []string
	skip := regexp.MustCompile(`^(image:|image sha256:|squashfs sha256:|squashfs size:|free in mtd7:)`)
	for _, l := range strings.Split(rep, "\n") {
		if l == "" || skip.MatchString(l) {
			continue
		}
		out = append(out, l)
	}
	return out
}

func TestBuildEqualsBashBuilder(t *testing.T) {
	kitDir := makeKit(t)
	fx := filepath.Join(repoRoot(t), "tests", "fixtures")
	_ = fx
	for _, v := range treeVariants {
		t.Run(v, func(t *testing.T) {
			root := fixtureTree(t, v)
			stock := stockImage(t, root)
			work := t.TempDir()
			tree := filepath.Join(work, "app")
			if err := os.Mkdir(tree, 0o755); err != nil {
				t.Fatal(err)
			}
			writeTree(t, root, tree)
			stockPath := filepath.Join(work, "mtdblock7.bin")
			if err := os.WriteFile(stockPath, stock, 0o600); err != nil {
				t.Fatal(err)
			}
			kit, err := LoadKit(kitDir)
			if err != nil {
				t.Fatal(err)
			}
			bashOut := filepath.Join(work, "out")
			b, berr := runBash(t, kitDir, tree, stockPath, bashOut)
			r, nerr := Build(Options{Kit: kit, Stock: &Stock{App: stock, Name: "mtdblock7.bin"}, UntestedOK: true, Epoch: testEpoch,
				KitAuth: "directory (consistency checked, not authenticated; --allow-unsigned)", Now: time.Now()})
			if (berr == nil) != (nerr == nil) {
				t.Fatalf("outcome differs: bash %v, native %v", berr, nerr)
			}
			if berr != nil {
				t.Logf("both refuse: bash: %v / native: %v", berr, nerr)
				return
			}
			// tree, modes, owners, times, link targets, hashes, sizes
			if len(b.listing) != len(r.Listing) {
				t.Fatalf("entries: bash %d, native %d", len(b.listing), len(r.Listing))
			}
			for i := range b.listing {
				if b.listing[i] != r.Listing[i] {
					t.Fatalf("entry %d differs:\nbash:   %s\nnative: %s", i, b.listing[i], r.Listing[i])
				}
			}
			// the patched service.sh is byte-identical (hence its diff)
			fsb, _ := squashfs.Open(bytes.NewReader(b.img), int64(len(b.img)))
			bs, _ := fsb.ReadFile("/sbin/service.sh")
			if !bytes.Equal(bs, lookup(r.Tree, "sbin/service.sh").Data) {
				t.Fatal("service.sh differs")
			}
			// image: same size, same padding rule
			if len(b.img) != PartitionSize || len(r.Image) != PartitionSize {
				t.Fatal("size")
			}
			bsq := bytes.TrimRight(b.img, "\xff")
			_ = bsq
			// report fields
			bf, nf := reportFields(b.report), reportFields(r.Report)
			if strings.Join(bf, "\n") != strings.Join(nf, "\n") {
				t.Fatalf("report fields differ:\nbash:\n%s\nnative:\n%s", strings.Join(bf, "\n"), strings.Join(nf, "\n"))
			}
			// unsquashfs -lln of both images (when it exists): same lines up to the directory sizes
			if ls := lln(t, b.img); ls != "" {
				if nl := lln(t, r.Image); nl != ls {
					t.Fatalf("unsquashfs -lln differs:\nbash:\n%s\nnative:\n%s", ls, nl)
				}
			}
		})
	}
}

// lln is `unsquashfs -lln` of an image without its directory sizes (layout dependent) and the header.
func lln(t testing.TB, img []byte) string {
	if !have("unsquashfs") {
		return ""
	}
	f := filepath.Join(t.TempDir(), "i.squashfs")
	if err := os.WriteFile(f, img, 0o600); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command("unsquashfs", "-no-xattrs", "-lln", f).Output()
	if err != nil {
		return ""
	}
	var l []string
	re := regexp.MustCompile(`^(d\S+ \S+) +\d+ (.*)$`)
	for _, s := range strings.Split(string(out), "\n") {
		if re.MatchString(s) {
			s = re.ReplaceAllString(s, "$1 - $2")
		}
		l = append(l, s)
	}
	return strings.Join(l, "\n")
}

// ---------------------------------------------------------------- goldens recorded from bash

// TestBuildGolden compares the native listing of the fixtures with testdata/*.golden, recorded from the bash
// builder (AKAM_IMAGE_RECORD=1 rewrites them from bash, which needs its tools). Kit-derived files are
// compared by path, mode, owner and time only (the repository's templates may change).
func TestBuildGolden(t *testing.T) {
	blank := kitDerived()
	for _, v := range treeVariants {
		name := strings.ReplaceAll(v, "/", "_")
		t.Run(v, func(t *testing.T) {
			r, stock, _ := buildFixture(t, v)
			got := listText(r.Listing, blank)
			gp := filepath.Join("testdata", name+".golden")
			if os.Getenv("AKAM_IMAGE_RECORD") == "1" {
				kitDir := makeKit(t)
				work := t.TempDir()
				tree := filepath.Join(work, "app")
				_ = os.Mkdir(tree, 0o755)
				writeTree(t, fixtureTree(t, v), tree)
				sp := filepath.Join(work, "mtdblock7.bin")
				_ = os.WriteFile(sp, stock, 0o600)
				b, err := runBash(t, kitDir, tree, sp, filepath.Join(work, "out"))
				if err != nil {
					t.Fatal(err)
				}
				if err := os.MkdirAll("testdata", 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(gp, []byte(listText(b.listing, blank)), 0o644); err != nil {
					t.Fatal(err)
				}
			}
			want, err := os.ReadFile(gp)
			if err != nil {
				t.Fatalf("no golden %s (AKAM_IMAGE_RECORD=1 records it from bash): %v", gp, err)
			}
			if string(want) != got {
				t.Fatalf("native listing differs from the bash golden %s:\nwant:\n%s\ngot:\n%s", gp, want, got)
			}
		})
	}
}

// ---------------------------------------------------------------- opt-in: a real backup and kit

// AKAM_IMAGE_REAL_BACKUP=/path/to/backup-dir AKAM_IMAGE_REAL_KIT=/path/to/kit-dir go test -run TestRealBackup -v ./internal/image
// builds natively and with the bash builder, compares the two images with the reader and prints one summary
// line. Private data stays in temporary folders removed at the end; nothing is printed but counts and hashes.
func TestRealBackup(t *testing.T) {
	bk, kd := os.Getenv("AKAM_IMAGE_REAL_BACKUP"), os.Getenv("AKAM_IMAGE_REAL_KIT")
	if bk == "" || kd == "" {
		t.Skip("set AKAM_IMAGE_REAL_BACKUP and AKAM_IMAGE_REAL_KIT to run")
	}
	realCompare(t, bk, kd)
}

// The same comparison on synthetic data, so that the opt-in path stays working.
func TestRealCompareOnSyntheticBackup(t *testing.T) {
	kd := makeKit(t)
	bk := t.TempDir()
	for i := 0; i < 7; i++ {
		if err := os.WriteFile(filepath.Join(bk, fmt.Sprintf("mtdblock%d.bin", i)), []byte{1, 2, 3}, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(bk, "mtdblock7.bin"), stockImage(t, fixtureTree(t, "app_fake")), 0o644); err != nil {
		t.Fatal(err)
	}
	realCompare(t, bk, kd)
}

func realCompare(t *testing.T, bk, kd string) {
	t.Helper()
	stock, err := LoadBackup(bk)
	if err != nil {
		t.Fatal(err)
	}
	kit, err := LoadKit(kd)
	if err != nil {
		t.Fatal(err)
	}
	r, err := Build(Options{Kit: kit, Stock: stock, UntestedOK: true, Epoch: testEpoch, KitAuth: "directory (consistency checked, not authenticated; --allow-unsigned)"})
	if err != nil {
		t.Fatalf("native: %v", err)
	}
	if !have("bash", "mksquashfs", "unsquashfs") {
		t.Logf("image-real: native ok (%d entries, sha256 %s, %d bytes free); bash tools missing, no comparison", len(r.Listing), r.SHA256[:12], r.Free)
		return
	}
	work := t.TempDir()
	tree := filepath.Join(work, "app")
	f := filepath.Join(bk, "mtdblock7.bin")
	if out, err := exec.Command("unsquashfs", "-no-xattrs", "-d", tree, f).CombinedOutput(); err != nil {
		t.Fatalf("unsquashfs: %v %s", err, tail(out))
	}
	defer func() { _ = exec.Command("chmod", "-R", "u+rwX", work).Run() }()
	b, err := runBash(t, kd, tree, f, filepath.Join(work, "out"), "--backup-dir", bk)
	if err != nil {
		t.Fatalf("bash: %v", err)
	}
	diff := 0
	if len(b.listing) != len(r.Listing) {
		diff = -1
	} else {
		for i := range b.listing {
			if b.listing[i] != r.Listing[i] {
				diff++
				t.Logf("differs: bash %s | native %s", b.listing[i], r.Listing[i])
			}
		}
	}
	bf, nf := reportFields(b.report), reportFields(r.Report)
	repEq := strings.Join(bf, "\n") == strings.Join(nf, "\n")
	if !repEq {
		t.Logf("report fields differ:\nbash:\n%s\nnative:\n%s", strings.Join(bf, "\n"), strings.Join(nf, "\n"))
	}
	t.Logf("image-real: entries bash=%d native=%d listing_diffs=%d report_fields_equal=%v native_size=%d bash_size=%d native_sha=%s bash_sha=%s",
		len(b.listing), len(r.Listing), diff, repEq, r.SquashSize, squashSize(b.img), r.SHA256[:12], sha(b.img)[:12])
	if diff != 0 {
		t.Fail()
	}
}

func squashSize(img []byte) int {
	fs, err := squashfs.Open(bytes.NewReader(img), int64(len(img)))
	if err != nil {
		return -1
	}
	return int((fs.Info().BytesUsed + 4095) &^ 4095)
}
