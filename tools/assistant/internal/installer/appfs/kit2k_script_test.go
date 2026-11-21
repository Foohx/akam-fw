package appfs

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// TestKitFromMakeKit2K builds a kit with the real tools/release/make_kit_2k.sh (on stub binaries, via
// tests/kit2k_test.sh) and checks that this package and the generic kit verifier both accept it and that a
// tampered copy is refused. It is the contract between the producer and the consumer of the 2K kit.
func TestKitFromMakeKit2K(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("skip: the kit script is a bash tool")
	}
	for _, tool := range []string{"bash", "tar", "xz", "sha256sum"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip("skip: " + tool + " not installed")
		}
	}
	root, err := filepath.Abs(filepath.Join("..", "..", "..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(root, "tests", "kit2k_test.sh")
	if _, err := os.Stat(script); err != nil {
		t.Skip("skip: not a source checkout")
	}
	work := t.TempDir()
	emit := filepath.Join(work, "emit", "kit")
	if err := os.MkdirAll(filepath.Dir(emit), 0o755); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("bash", script)
	cmd.Env = append(os.Environ(), "TMPDIR="+work, "AKAM_KIT2K_EMIT="+emit)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("the kit script failed: %v\n%s", err, out)
	}
	k, err := LoadKit(emit, AllowTestGrade())
	if err != nil {
		t.Fatal(err)
	}
	if !k.HasCard() || k.Manifest["model"] != "lsc-rotatable-2k-ak3918av130" || k.Version() != "test1" {
		t.Fatalf("manifest %v", k.Manifest)
	}
	tarball := filepath.Join(filepath.Dir(emit), "akam-kit-lsc-rotatable-2k-ak3918av130-test1.tar.xz")
	if _, err := os.Stat(tarball); err != nil {
		t.Fatal(err)
	}
	res, err := kit.VerifyTarball(tarball, kit.Options{AllowUnsigned: true, AllowTest: true, Layout: KitLayout(),
		ExpectModel: "lsc-rotatable-2k-ak3918av130"})
	if err != nil {
		t.Fatal(err)
	}
	defer res.Close()
	if _, err := LoadKit(res.Dir, AllowTestGrade()); err != nil {
		t.Fatal(err)
	}
	if _, err := kit.VerifyTarball(tarball, kit.Options{AllowUnsigned: true, AllowTest: true}); err == nil {
		t.Fatal("the 1080p layout accepted a 2K kit")
	}
	if _, err := kit.VerifyTarball(tarball, kit.Options{AllowUnsigned: true, Layout: KitLayout()}); err == nil {
		t.Fatal("a test kit passed as a release")
	}
	if err := os.WriteFile(filepath.Join(emit, "appfs", "overlay", "common.sh"), []byte("tampered"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadKit(emit, AllowTestGrade()); err == nil {
		t.Fatal("tampered kit accepted")
	}
}
