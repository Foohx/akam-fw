package image

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

func TestImageFilesCopyEqualsRepository(t *testing.T) {
	b, err := os.ReadFile(filepath.Join(repoRoot(t), "tools", "release", "image_files.txt"))
	if err != nil {
		t.Skip("no repository checkout:", err)
	}
	if string(b) != imageFilesTxt {
		t.Fatal("internal/image/image_files.txt differs from tools/release/image_files.txt: copy it again")
	}
}

// ---------------------------------------------------------------- patches against the bash scripts

var variants = []string{"app_fake", "app_fake_variants/a", "app_fake_variants/b", "app_fake_variants/c", "app_fake_variants/d"}

func fixtureTree(t testing.TB, variant string) *squashfs.Node {
	t.Helper()
	fx := filepath.Join(repoRoot(t), "tests", "fixtures")
	root := nodeFromDir(t, filepath.Join(fx, "app_fake"))
	if variant != "app_fake" {
		b, err := os.ReadFile(filepath.Join(fx, variant, "sbin", "service.sh"))
		if err != nil {
			t.Fatal(err)
		}
		lookup(root, "sbin/service.sh").Data = b
	}
	return root
}

func exitCode(err error) int {
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		return ee.ExitCode()
	}
	if err != nil {
		return -1
	}
	return 0
}

func TestServicePatchEqualsBash(t *testing.T) {
	if !have("bash", "awk", "sh", "diff", "mktemp") {
		t.Skip("bash, awk or sh missing")
	}
	script := filepath.Join(repoRoot(t), "tools", "firmware_patch_templates", "patch_service_sh.sh")
	for _, v := range variants {
		t.Run(v, func(t *testing.T) {
			in := lookup(fixtureTree(t, v), "sbin/service.sh").Data
			dir := t.TempDir()
			if err := os.MkdirAll(filepath.Join(dir, "sbin"), 0o755); err != nil {
				t.Fatal(err)
			}
			p := filepath.Join(dir, "sbin", "service.sh")
			if err := os.WriteFile(p, in, 0o755); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command("bash", script, dir)
			cmd.Env = append(os.Environ(), "AKAM_PATCH_REPORT="+filepath.Join(dir, "r.diff"))
			want := exitCode(cmd.Run())
			out, err := patchService(in, func(string) {})
			got := 0
			var pe *patchError
			if errors.As(err, &pe) {
				got = pe.code
			}
			if got != want {
				t.Fatalf("exit code: native %d, bash %d (%v)", got, want, err)
			}
			if want == 0 {
				bb, _ := os.ReadFile(p)
				if !bytes.Equal(out, bb) {
					t.Fatalf("patched service.sh differs from bash:\nnative:\n%s\nbash:\n%s", out, bb)
				}
			}
		})
	}
}

func TestEthernetPatchEqualsBash(t *testing.T) {
	if !have("bash", "awk", "sh", "mktemp") {
		t.Skip("bash, awk or sh missing")
	}
	script := filepath.Join(repoRoot(t), "tools", "firmware_patch_templates", "patch_ethernet_hostname.sh")
	ins := map[string]string{
		"fixture":      string(lookup(fixtureTree(t, "app_fake"), "sbin/ethernet.sh").Data),
		"patched":      "udhcpc -i wlan0 -x \"hostname:$(/usr/sbin/akam_hostname.sh show 2>/dev/null || echo akam)\" &\n",
		"unexpected":   "udhcpc -i wlan0 -x hostname:Other &\n",
		"nothing":      "echo hi\n",
		"no newline":   "udhcpc -i wlan0 -x hostname:IPCamera &",
		"commented":    "# udhcpc -i wlan0 -x hostname:IPCamera &\n",
		"continuation": "udhcpc -i wlan0 -x hostname:IPCamera \\\n  &\n",
		"crlf":         "udhcpc -i wlan0 -x hostname:IPCamera &\r\nexit 0\r\n",
	}
	for name, in := range ins {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			if err := os.MkdirAll(filepath.Join(dir, "sbin"), 0o755); err != nil {
				t.Fatal(err)
			}
			p := filepath.Join(dir, "sbin", "ethernet.sh")
			if err := os.WriteFile(p, []byte(in), 0o755); err != nil {
				t.Fatal(err)
			}
			want := exitCode(exec.Command("bash", script, dir).Run())
			out, err := patchEthernet([]byte(in), func(string) {})
			got := 0
			var pe *patchError
			if errors.As(err, &pe) {
				got = pe.code
			}
			if got != want {
				t.Fatalf("exit code: native %d, bash %d (%v)", got, want, err)
			}
			if want == 0 {
				bb, _ := os.ReadFile(p)
				if !bytes.Equal(out, bb) {
					t.Fatalf("patched ethernet.sh differs from bash:\nnative:\n%q\nbash:\n%q", out, bb)
				}
			}
		})
	}
}

func TestDropCachesPatchEqualsSed(t *testing.T) {
	ins := []string{"while true; do\n\tsleep 5\n\tsync\ndone\n", "sleep 5", "sleep 5  \n sleep 5\t\n", "sleep 60\n", "sleep 50\n", "", "  sleep 5\r\n"}
	for _, in := range ins {
		out, changed, reason := patchDropCaches([]byte(in))
		if have("sed", "grep") {
			f := filepath.Join(t.TempDir(), "d.sh")
			if err := os.WriteFile(f, []byte(in), 0o644); err != nil {
				t.Fatal(err)
			}
			if exec.Command("grep", "-q", `^[[:space:]]*sleep 5[[:space:]]*$`, f).Run() == nil {
				if err := exec.Command("sed", "-i", `s/^\([[:space:]]*\)sleep 5[[:space:]]*$/\1sleep 60/`, f).Run(); err != nil {
					t.Fatal(err)
				}
				want, _ := os.ReadFile(f)
				if !changed || string(out) != string(want) {
					t.Fatalf("%q: native %q (changed %v), sed %q", in, out, changed, want)
				}
				continue
			}
		}
		if changed {
			t.Fatalf("%q: changed unexpectedly: %q", in, out)
		}
		_ = reason
	}
}

func TestDiffHunks(t *testing.T) {
	a := "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n"
	b := "1\n2\n3\nX\n5\n6\n7\n8\n9\n10\n11\n12\nY\n"
	d := unifiedDiff("a", "b", a, b)
	if !strings.Contains(d, "-4\n+X\n") || !strings.Contains(d, "+Y\n") || strings.Count(d, "@@ -") != 2 {
		t.Fatalf("unexpected diff:\n%s", d)
	}
	if have("diff") {
		dir := t.TempDir()
		_ = os.WriteFile(filepath.Join(dir, "a"), []byte(a), 0o644)
		_ = os.WriteFile(filepath.Join(dir, "b"), []byte(b), 0o644)
		out, _ := exec.Command("diff", "-u", filepath.Join(dir, "a"), filepath.Join(dir, "b")).Output()
		strip := func(s string) string { return s[strings.Index(s, "@@"):] }
		if strip(string(out)) != strip(d) {
			t.Fatalf("diff -u:\n%s\nnative:\n%s", out, d)
		}
	}
}

// ---------------------------------------------------------------- Build

func buildFixture(t testing.TB, variant string) (*Result, []byte, *Kit) {
	t.Helper()
	root := fixtureTree(t, variant)
	stock := stockImage(t, root)
	kit, err := NewKit(stubKitFiles(t, hwFor(sha(stock))))
	if err != nil {
		t.Fatal(err)
	}
	r, err := Build(Options{Kit: kit, Stock: &Stock{App: stock, Name: "mtdblock7.bin"}, KitAuth: "directory (consistency checked, not authenticated; --allow-unsigned)",
		Epoch: testEpoch, Now: time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)})
	if err != nil {
		t.Fatalf("%s: %v", variant, err)
	}
	return r, stock, kit
}

func TestBuildFixtureProperties(t *testing.T) {
	r, _, _ := buildFixture(t, "app_fake")
	if len(r.Image) != PartitionSize {
		t.Fatalf("image is %d bytes", len(r.Image))
	}
	if r.Name != "ak_rtsp_firmware_20260102_030405.squashfs" {
		t.Fatal(r.Name)
	}
	if r.SquashSize%4096 != 0 || r.SquashSize >= PartitionSize {
		t.Fatalf("squashfs size %d", r.SquashSize)
	}
	// padding: 0xFF from the end of the 4096-padded squashfs, zeros inside
	for _, c := range r.Image[r.SquashSize:] {
		if c != 0xFF {
			t.Fatal("padding is not 0xFF")
		}
	}
	m := map[string]squashfs.ListEntry{}
	for _, e := range r.Listing {
		m[e.Path] = e
		if e.UID != 0 || e.GID != 0 || e.MTime != testEpoch {
			t.Fatalf("%s: uid %d gid %d mtime %d", e.Path, e.UID, e.GID, e.MTime)
		}
	}
	for _, p := range []string{"/bin/ak_rtsp", "/bin/dropbearmulti", "/sbin/akam_ssh.sh", "/local/_ht_process.ini", "/local/akam/MANIFEST.txt", "/local/akam/LICENSE.akam"} {
		if _, ok := m[p]; !ok {
			t.Fatalf("missing %s", p)
		}
	}
	if _, ok := m["/bin/anyka_ipc"]; ok {
		t.Fatal("anyka_ipc still there")
	}
	if e := m["/bin/dropbear"]; e.Type != squashfs.TypeSymlink || e.Target != "dropbearmulti" {
		t.Fatalf("dropbear: %+v", e)
	}
	if e := m["/lib/libfake.so"]; e.Type != squashfs.TypeSymlink || e.Target != "libfake.so.1" {
		t.Fatalf("stock symlink lost: %+v", e)
	}
	if e := m["/sbin/akam_ssh.sh"]; e.Mode != 0o755 {
		t.Fatalf("mode %o", e.Mode)
	}
	if e := m["/local/akam"]; e.Mode != 0o755 || e.Type != squashfs.TypeDir {
		t.Fatalf("%+v", e)
	}
	if !strings.Contains(r.ServiceDiff, "AKAM-DISABLED") {
		t.Fatalf("diff: %s", r.ServiceDiff)
	}
	if !strings.Contains(r.Report, "image sha256:       "+r.SHA256) || !strings.Contains(r.Report, "warnings: 0\n") {
		t.Fatalf("report:\n%s", r.Report)
	}
	// deterministic
	r2, _, _ := buildFixture(t, "app_fake")
	if r2.SHA256 != r.SHA256 {
		t.Fatal("two builds differ")
	}
}

func TestBuildRefusals(t *testing.T) {
	root := fixtureTree(t, "app_fake")
	stock := stockImage(t, root)
	files := stubKitFiles(t, hwFor(sha(stock)))
	opts := func(mod func(*Options)) Options {
		kit, _ := NewKit(files)
		o := Options{Kit: kit, Stock: &Stock{App: stock, Name: "mtdblock7.bin"}, Epoch: testEpoch}
		if mod != nil {
			mod(&o)
		}
		return o
	}
	if _, err := Build(opts(nil)); err != nil {
		t.Fatal(err)
	}
	// unknown hardware needs the explicit yes
	other := stockImage(t, func() *squashfs.Node {
		r := fixtureTree(t, "app_fake")
		r.Children = append(r.Children, squashfs.NewFile("zz", 0o644, []byte("x")))
		return r
	}())
	if _, err := Build(opts(func(o *Options) { o.Stock = &Stock{App: other, Name: "mtdblock7.bin"} })); err == nil || !strings.Contains(err.Error(), "not validated") {
		t.Fatalf("want an untested refusal, got %v", err)
	}
	r, err := Build(opts(func(o *Options) { o.Stock = &Stock{App: other, Name: "mtdblock7.bin"}; o.UntestedOK = true }))
	if err != nil || len(r.Warnings) != 1 || !strings.Contains(r.Warnings[0], "UNTESTED") {
		t.Fatalf("untested build: %v %v", err, r)
	}
	// wrong size
	if _, err := Build(opts(func(o *Options) { o.Stock = &Stock{App: stock[:1000], Name: "x"} })); err == nil || !strings.Contains(err.Error(), "MTD layout differs") {
		t.Fatalf("size: %v", err)
	}
	// not a squashfs
	if _, err := Build(opts(func(o *Options) { o.Stock = &Stock{App: make([]byte, PartitionSize), Name: "x"} })); err == nil {
		t.Fatal("zeros accepted")
	}
	// service.sh without a telnet/ftp line
	bare := fixtureTree(t, "app_fake")
	lookup(bare, "sbin/service.sh").Data = []byte("#!/bin/sh\necho hi\n")
	bs := stockImage(t, bare)
	bf := stubKitFiles(t, hwFor(sha(bs)))
	kit, _ := NewKit(bf)
	if _, err := Build(Options{Kit: kit, Stock: &Stock{App: bs}, Epoch: testEpoch}); err == nil || !strings.Contains(err.Error(), "not patched") {
		t.Fatalf("unpatched service.sh: %v", err)
	}
	r, err = Build(Options{Kit: kit, Stock: &Stock{App: bs}, Epoch: testEpoch, AllowUnpatchedService: true})
	if err != nil || len(r.Warnings) != 1 || r.ServiceDiff != "" {
		t.Fatalf("allowed unpatched: %v", err)
	}
	// private key in the tree
	key := fixtureTree(t, "app_fake")
	key.Children = append(key.Children, squashfs.NewFile("note", 0o644, []byte("-----BEGIN OPENSSH "+"PRIVATE KEY-----\n")))
	ks := stockImage(t, key)
	kkit, _ := NewKit(stubKitFiles(t, hwFor(sha(ks))))
	if _, err := Build(Options{Kit: kkit, Stock: &Stock{App: ks}, Epoch: testEpoch}); err == nil || !strings.Contains(err.Error(), "private key") {
		t.Fatalf("private key: %v", err)
	}
	// a stock without bin/
	nb := fixtureTree(t, "app_fake")
	removeChild(nb, "bin")
	ns := stockImage(t, nb)
	nkit, _ := NewKit(stubKitFiles(t, hwFor(sha(ns))))
	if _, err := Build(Options{Kit: nkit, Stock: &Stock{App: ns}, Epoch: testEpoch}); err == nil || !strings.Contains(err.Error(), "not an APP tree") {
		t.Fatalf("no bin: %v", err)
	}
}

func TestKitChecks(t *testing.T) {
	f := stubKitFiles(t, hwFor("00"))
	f["overlay/sbin/newer.sh"] = []byte("x")
	if _, err := NewKit(f); err == nil || !strings.Contains(err.Error(), "newer.sh") {
		t.Fatalf("unknown overlay file: %v", err)
	}
	g := stubKitFiles(t, hwFor("00"))
	delete(g, "licenses/LICENSE.zig")
	if _, err := NewKit(g); err == nil || !strings.Contains(err.Error(), "LICENSE.zig") {
		t.Fatalf("missing licence: %v", err)
	}
}

func TestKernelAndVerdict(t *testing.T) {
	h := make([]byte, 64)
	copy(h, []byte{0x27, 0x05, 0x19, 0x56})
	copy(h[32:], "Linux-4.4.302-cip94")
	if k := kernelName(h); k != "4.4.302-cip94" {
		t.Fatal(k)
	}
	h[0] = 0
	if kernelName(h) != "" {
		t.Fatal("bad magic accepted")
	}
	hw := "validated|Cam|soc|4.4.302-cip94 #1 x|8|squashfs xz block=131072 bcj=none|abc|1\n"
	st := &Stock{Kernel: "4.4.302-cip94", KernelExpected: true}
	if v, _ := verdict(hw, "abc", "squashfs xz block=131072 bcj=none", st); v != "validated" {
		t.Fatal(v)
	}
	if v, d := verdict(hw, "abc", "squashfs gzip block=131072 bcj=none", st); v != "unknown" || !strings.Contains(d, "format differs") {
		t.Fatal(v, d)
	}
	st.Kernel = "9.9"
	if v, d := verdict(hw, "abc", "squashfs xz block=131072 bcj=none", st); v != "unknown" || !strings.Contains(d, "kernel differs") {
		t.Fatal(v, d)
	}
	st.Kernel = ""
	if v, d := verdict(hw, "abc", "squashfs xz block=131072 bcj=none", st); v != "unknown" || !strings.Contains(d, "could not be read") {
		t.Fatal(v, d)
	}
}
