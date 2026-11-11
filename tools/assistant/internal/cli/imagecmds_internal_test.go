package cli

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

func TestDefaultBuilderPolicy(t *testing.T) {
	// the default stays tools on Linux and import elsewhere until the hardware gate (spec 11, 4.8 step 4)
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
	put("MANIFEST.txt", []byte("kit_format=1\nkit_grade=test\nakam_version=t\nsource_commit=abc\nbuilt_utc=2023-11-14T22:13:20Z\n"))
	put("supported_hardware.txt", []byte("supported_hardware_format=1\n"))
	put("bin/ak_rtsp", []byte("ak"))
	put("bin/dropbearmulti", []byte("db"))
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
	if a.St.Get("image_md5") == "" || !strings.Contains(out.String(), "EXPERIMENTAL") || !strings.Contains(out.String(), "warnings: 2") {
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
	if err := a.buildNativeImage(); err == nil || !strings.Contains(out.String(), "not a directory") {
		t.Fatalf("after yes, a missing backup is reported: %v\n%s", err, out)
	}
}
