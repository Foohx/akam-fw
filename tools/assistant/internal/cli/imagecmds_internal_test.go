package cli

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

func TestDefaultBuilderPolicy(t *testing.T) {
	// the default stays tools on Linux and import elsewhere until the hardware gate (specs/installer.md)
	for goos, want := range map[string]string{"linux": "tools", "windows": "import", "darwin": "import", "freebsd": "import"} {
		if got := defaultBuilder(goos); got != want {
			t.Errorf("%s: %s, want %s", goos, got, want)
		}
	}
	a := &App{Opt: &Options{}}
	a.Opt.Image = "x.img"
	if a.imageBuilder() != "import" {
		t.Fatal("--image imports")
	}
	a.Opt = &Options{Builder: "native"}
	if a.imageBuilder() != "native" {
		t.Fatal("--builder native is honoured")
	}
	if _, err := ParseArgs([]string{"--builder", "quick", "image"}); err == nil {
		t.Fatal("bad builder accepted")
	}
	o, err := ParseArgs([]string{"--builder", "native", "--i-understand-untested", "image"})
	if err != nil || o.Builder != "native" || !o.UntestedOK {
		t.Fatalf("%v %+v", err, o)
	}
}

func nativeWorld(t *testing.T, answer string) (*App, *bytes.Buffer, string, string) {
	t.Helper()
	t.Setenv("AKAM_FORCE_TTY", "1")
	repo, _ := filepath.Abs(filepath.Join("..", "..", "..", ".."))
	// stock APP: a synthetic tree
	root := squashfs.NewDir("", 0o755)
	for _, d := range []string{"bin", "sbin", "local"} {
		root.Add(squashfs.NewDir(d, 0o755))
	}
	root.Children[0].Add(squashfs.NewFile("anyka_ipc", 0o755, []byte("vendor")))
	svc, _ := os.ReadFile(filepath.Join(repo, "tests/fixtures/app_fake/sbin/service.sh"))
	eth, _ := os.ReadFile(filepath.Join(repo, "tests/fixtures/app_fake/sbin/ethernet.sh"))
	root.Children[1].Add(squashfs.NewFile("service.sh", 0o755, svc), squashfs.NewFile("ethernet.sh", 0o755, eth))
	sq, _, err := squashfs.Build(root, squashfs.Options{Time: 1, AllTime: true})
	if err != nil {
		t.Fatal(err)
	}
	stock := bytes.Repeat([]byte{0xFF}, 5242880)
	copy(stock, sq)
	work := t.TempDir()
	bk := filepath.Join(work, "backup")
	_ = os.Mkdir(bk, 0o755)
	for i := 0; i < 7; i++ {
		_ = os.WriteFile(filepath.Join(bk, "mtdblock"+string(rune('0'+i))+".bin"), []byte{1, 2, 3}, 0o644)
	}
	_ = os.WriteFile(filepath.Join(bk, "mtdblock7.bin"), stock, 0o644)
	writeSums(t, bk, nil)
	// kit folder: stand-ins plus the repository's overlay files
	base := filepath.Join(work, "state")
	_ = os.Mkdir(base, 0o700)
	kd := filepath.Join(base, "kit")
	put := func(rel string, b []byte) {
		p := filepath.Join(kd, filepath.FromSlash(rel))
		_ = os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	hwTxt := []byte("supported_hardware_format=1\nvalidated|Other camera|soc|Linux-x|8 partitions|squashfs xz block=131072 bcj=none|" + strings.Repeat("ab", 32) + "|1\n")
	put("supported_hardware.txt", hwTxt)
	put("bin/ak_rtsp", []byte("ak"))
	put("bin/dropbearmulti", []byte("db"))
	put("bin/flash_tool", []byte("ft"))
	for _, n := range []string{"LICENSE.akam", "THIRD_PARTY_NOTICES.md", "LICENSE.dropbear", "LICENSE.musl", "LICENSE.zig", "LICENSE.smolrtsp", "LICENSE.slice99", "LICENSE.metalang99", "LICENSE.datatype99", "LICENSE.interface99"} {
		put("licenses/"+n, []byte(n))
	}
	list, _ := os.ReadFile(filepath.Join(repo, "tools/release/image_files.txt"))
	for _, l := range strings.Split(string(list), "\n") {
		f := strings.Fields(l)
		if len(f) == 5 && f[0] == "file" {
			b, _ := os.ReadFile(filepath.Join(repo, f[4]))
			put(f[2], b)
		}
	}
	writeKitManifest(t, kd, hwTxt, "")
	var out bytes.Buffer
	secrets := &state.Secrets{}
	opt := &Options{}
	st := Streams{In: strings.NewReader(answer), Out: &out, Err: &out}
	a := &App{Opt: opt, Streams: st, Secrets: secrets, Cams: &state.Cameras{Base: base, Name: "default"}}
	a.UI = NewPlainUI(opt, st, nil, secrets, false)
	a.St, err = state.Open(base, false, secrets)
	if err != nil {
		t.Fatal(err)
	}
	return a, &out, bk, kd
}

func TestNativeImageBuildsWritesAndRecords(t *testing.T) {
	a, out, bk, kd := nativeWorld(t, "")
	a.Opt.UntestedOK = true
	if err := a.runNativeImage(kd, bk); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	p := a.St.Get("image_path")
	st, err := os.Stat(p)
	if err != nil || st.Size() != 5242880 {
		t.Fatalf("image %q: %v", p, err)
	}
	dir := filepath.Dir(p)
	for _, f := range []string{filepath.Base(p) + ".sha256", "build_report.txt", "service_sh.diff"} {
		if _, err := os.Stat(filepath.Join(dir, f)); err != nil {
			t.Errorf("missing %s", f)
		}
	}
	if a.St.Get("image_md5") == "" || !strings.Contains(out.String(), "EXPERIMENTAL") || !strings.Contains(out.String(), "warnings: 3") {
		t.Fatalf("output:\n%s", out)
	}
	// without --i-understand-untested the unknown hardware is refused and nothing is written
	b, out2, bk2, kd2 := nativeWorld(t, "")
	if err := b.runNativeImage(kd2, bk2); err == nil || !strings.Contains(out2.String(), "not validated") {
		t.Fatalf("expected a refusal:\n%s", out2)
	}
	if _, err := os.Stat(filepath.Join(b.Cams.Base, "image")); err == nil {
		t.Fatal("a refused build wrote an image folder")
	}
}

func TestNativeImageNeedsAYes(t *testing.T) {
	for _, answer := range []string{"", "n\n", "\n"} {
		a, out, _, _ := nativeWorld(t, answer)
		if err := a.buildNativeImage(); err == nil || !strings.Contains(out.String(), "EXPERIMENTAL") || !strings.Contains(out.String(), "not building") {
			t.Fatalf("answer %q: %v\n%s", answer, err, out)
		}
		if a.St.Get("image_path") != "" {
			t.Fatal("recorded without a yes")
		}
	}
	a, out, _, _ := nativeWorld(t, "y\n")
	a.Opt.BackupDir = "/nonexistent-backup"
	if err := a.buildNativeImage(); err == nil || !strings.Contains(out.String(), "does not verify") {
		t.Fatalf("after yes, a missing backup is reported: %v\n%s", err, out)
	}
}

func sumHex(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// writeSums writes the SHA256SUMS of a backup folder (every file but SHA256SUMS and the extra names).
func writeSums(t *testing.T, dir string, skip map[string]bool) {
	t.Helper()
	ents, _ := os.ReadDir(dir)
	var l []string
	for _, e := range ents {
		if e.Name() == "SHA256SUMS" || skip[e.Name()] {
			continue
		}
		b, _ := os.ReadFile(filepath.Join(dir, e.Name()))
		l = append(l, sumHex(b)+"  "+e.Name())
	}
	sort.Strings(l)
	if err := os.WriteFile(filepath.Join(dir, "SHA256SUMS"), []byte(strings.Join(l, "\n")+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}

// writeKitManifest completes a synthetic kit folder with a MANIFEST.txt and SHA256SUMS that verify
// (extra is appended to the MANIFEST header, e.g. to change time64_gate).
func writeKitManifest(t *testing.T, kd string, hw []byte, extra string) {
	t.Helper()
	var files []string
	_ = filepath.WalkDir(kd, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			rel, _ := filepath.Rel(kd, p)
			if rel = filepath.ToSlash(rel); rel != "MANIFEST.txt" && rel != "SHA256SUMS" {
				files = append(files, rel)
			}
		}
		return nil
	})
	sort.Strings(files)
	man := "kit_format=1\nkit_grade=release\nakam_version=t\nsource_commit=abc\nbuilt_utc=2023-11-14T22:13:20Z\ntarget=arm-linux-musleabi\n" +
		"time64_gate=pass\n" + extra + "supported_hardware_sha256=" + sumHex(hw) + "\n"
	for _, f := range files {
		b, _ := os.ReadFile(filepath.Join(kd, filepath.FromSlash(f)))
		mode := 0o644
		if strings.HasPrefix(f, "bin/") || strings.HasSuffix(f, ".sh") {
			mode = 0o755
		}
		_ = os.Chmod(filepath.Join(kd, filepath.FromSlash(f)), os.FileMode(mode))
		man += fmt.Sprintf("file %s %o %s\n", sumHex(b), mode, f)
	}
	if err := os.WriteFile(filepath.Join(kd, "MANIFEST.txt"), []byte(man), 0o644); err != nil {
		t.Fatal(err)
	}
	var sums []string
	for _, f := range append(files, "MANIFEST.txt") {
		b, _ := os.ReadFile(filepath.Join(kd, filepath.FromSlash(f)))
		sums = append(sums, sumHex(b)+"  "+f)
	}
	sort.Strings(sums)
	if err := os.WriteFile(filepath.Join(kd, "SHA256SUMS"), []byte(strings.Join(sums, "\n")+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestNativeImageRefusesUnverifiedInputs(t *testing.T) {
	// a kit edited after get-kit (a binary swapped, MANIFEST untouched)
	a, out, bk, kd := nativeWorld(t, "")
	a.Opt.UntestedOK = true
	if err := os.WriteFile(filepath.Join(kd, "bin", "ak_rtsp"), []byte("evil"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := a.runNativeImage(kd, bk); err == nil || !strings.Contains(out.String(), "kit failed verification") {
		t.Fatalf("tampered kit accepted: %v\n%s", err, out)
	}
	// a kit whose MANIFEST lacks time64_gate=pass (even with consistent sums)
	b, out2, bk2, kd2 := nativeWorld(t, "")
	b.Opt.UntestedOK = true
	m, _ := os.ReadFile(filepath.Join(kd2, "MANIFEST.txt"))
	if err := os.WriteFile(filepath.Join(kd2, "MANIFEST.txt"), bytes.ReplaceAll(m, []byte("time64_gate=pass"), []byte("time64_gate=fail")), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := b.runNativeImage(kd2, bk2); err == nil || !strings.Contains(out2.String(), "kit failed verification") {
		t.Fatalf("kit without time64_gate=pass accepted: %v\n%s", err, out2)
	}
	// a backup file changed after SHA256SUMS was written
	c, out3, bk3, kd3 := nativeWorld(t, "")
	c.Opt.UntestedOK = true
	if err := os.WriteFile(filepath.Join(bk3, "mtdblock1.bin"), []byte{9, 9, 9}, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := c.runNativeImage(kd3, bk3); err == nil || !strings.Contains(out3.String(), "does not verify") {
		t.Fatalf("modified backup accepted: %v\n%s", err, out3)
	}
	// a backup made over SSH from an Akam camera is not the stock APP
	d, out4, bk4, kd4 := nativeWorld(t, "")
	d.Opt.UntestedOK = true
	if err := os.WriteFile(filepath.Join(bk4, "camera_info.txt"), []byte("kind=akam\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	writeSums(t, bk4, nil)
	if err := d.runNativeImage(kd4, bk4); err == nil || !strings.Contains(out4.String(), "kind=akam") {
		t.Fatalf("Akam backup accepted as the stock source: %v\n%s", err, out4)
	}
	for _, w := range []*App{a, b, c, d} {
		if _, err := os.Stat(filepath.Join(w.Cams.Base, "image")); err == nil {
			t.Fatal("a refused build wrote an image folder")
		}
	}
}

func TestNativeImageOverridesAreConfirmed(t *testing.T) {
	for _, env := range []string{"AKAM_ALLOW_UNPATCHED_SERVICE", "AKAM_ALLOW_TIGHT"} {
		t.Setenv(env, "1")
		a, out, bk, kd := nativeWorld(t, "n\n")
		a.Opt.UntestedOK = true
		if err := a.runNativeImage(kd, bk); err == nil || !strings.Contains(out.String(), "was not confirmed") {
			t.Fatalf("%s: not asked or accepted: %v\n%s", env, err, out)
		}
		if _, err := os.Stat(filepath.Join(a.Cams.Base, "image")); err == nil {
			t.Fatalf("%s: wrote an image", env)
		}
		t.Setenv(env, "")
	}
	t.Setenv("SOURCE_DATE_EPOCH", "tomorrow")
	a, out, bk, kd := nativeWorld(t, "")
	a.Opt.UntestedOK = true
	if err := a.runNativeImage(kd, bk); err == nil || !strings.Contains(out.String(), "SOURCE_DATE_EPOCH") {
		t.Fatalf("bad epoch accepted: %v\n%s", err, out)
	}
}
