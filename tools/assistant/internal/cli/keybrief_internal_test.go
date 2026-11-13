package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

// The chooser note of a camera: the kind in words ("this camera only", never "own") and a short fingerprint.
func TestKeyBriefForTheChooser(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	base := t.TempDir()
	a := &App{Cams: &state.Cameras{Base: base}}
	write := func(camera, pub string) {
		dir := a.Cams.DirOf(camera)
		if err := os.MkdirAll(dir, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "state"), []byte("ssh_pubkey="+pub+"\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	for _, p := range []string{camssh.SharedKeyPath(), camssh.OwnKeyPath("salon"), filepath.Join(home, ".ssh", "id_work")} {
		if err := camssh.GenerateKey(p, "t", nil); err != nil {
			t.Fatal(err)
		}
	}
	write("a", camssh.SharedKeyPath()+".pub")
	write("salon", camssh.OwnKeyPath("salon")+".pub")
	write("c", filepath.Join(home, ".ssh", "id_work.pub"))
	for camera, want := range map[string]string{"a": "shared SHA256:", "salon": "this camera only SHA256:", "c": "custom SHA256:"} {
		got := a.keyBrief(camera)
		if !strings.HasPrefix(got, want) || len(got) != len(want)+8 {
			t.Errorf("keyBrief(%s) = %q, want %q + 8 characters", camera, got, want)
		}
	}
	if got := a.keyBrief("nobody"); got != "" {
		t.Errorf("a camera without a key: %q", got)
	}
}
