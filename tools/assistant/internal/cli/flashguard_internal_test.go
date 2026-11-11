package cli

import (
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The P1 guard stays in force on every path that is not the audited flash function: it refuses the
// reflash commands themselves.
func TestGuardRefusesTheReflashCommands(t *testing.T) {
	for _, bad := range []string{
		"/tmp/akam_reflash/bin/busybox sh '/tmp/akam_reflash/akam_reflash_ssh.sh' start --md5 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' --i-understand-no-uart",
		"sh '/tmp/akam_reflash/akam_reflash_ssh.sh' prepare --md5 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"/tmp/akam_reflash/flash_tool /tmp/akam_reflash/image.squashfs /dev/mtd7",
		"sh x --i-understand-no-uart",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
	// the read-only commands of verify stay allowed
	for _, ok := range []string{"cat > /tmp/akam_hwcheck.sh", "sh /tmp/akam_hwcheck.sh", "cat > /tmp/akam_preflight.sh", "sh /tmp/akam_preflight.sh --ensure-wifi", "cat /etc/config/akam/onvif 2>/dev/null"} {
		if why := forbiddenRemote(ok); why != "" {
			t.Errorf("%s", why)
		}
	}
}

// Audit: the flash command and its confirmation flag exist in internal/flash only (the dry-run
// text of flashcmds.go and the guard in remote.go only mention them), and flash.Flash has exactly
// one caller.
func TestOnlyTheFlashPackageWritesTheFlash(t *testing.T) {
	root := filepath.Join("..")
	var callers int
	err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || !strings.HasSuffix(p, ".go") || strings.HasSuffix(p, "_test.go") {
			return err
		}
		rel, _ := filepath.Rel(root, p)
		b, _ := os.ReadFile(p)
		s := string(b)
		inFlash := strings.HasPrefix(rel, "flash"+string(filepath.Separator))
		inFake := strings.HasPrefix(rel, "testfake"+string(filepath.Separator)) || strings.HasPrefix(rel, filepath.Join("kit", "kittest")+string(filepath.Separator))
		if !inFlash && !inFake && rel != filepath.Join("cli", "flashcmds.go") && rel != filepath.Join("cli", "remote.go") {
			for _, w := range []string{"i-understand-no-uart", "akam_reflash_ssh.sh", "flash_guard.sh"} {
				if strings.Contains(s, w) {
					t.Errorf("%s mentions %q: only internal/flash may drive a flash write", rel, w)
				}
			}
		}
		if !inFlash {
			callers += strings.Count(s, "flash.Flash(")
		}
		if strings.Contains(s, "gossh.Session") && !strings.HasPrefix(rel, "camera"+string(filepath.Separator)) && !inFake {
			t.Errorf("%s opens raw SSH sessions", rel)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if callers != 1 {
		t.Errorf("flash.Flash is called %d times outside the flash package, want exactly 1 (flashSSH)", callers)
	}
}

// A rehearsal's automatic "yes" (Plain.Danger answers yes when DryRun) must never become a flash
// confirmation, even if a code path forgot to return before asking.
func TestRehearsalNeverConfirmsAFlash(t *testing.T) {
	a := &App{UI: &ui.Plain{DryRun: true, Out: io.Discard, ErrOut: io.Discard}}
	if ok, err := a.flashConfirm("Updating", "192.0.2.1", strings.Repeat("a", 32), "SSH"); ok || err == nil {
		t.Fatalf("a rehearsal confirmed a flash: %v %v", ok, err)
	}
}
