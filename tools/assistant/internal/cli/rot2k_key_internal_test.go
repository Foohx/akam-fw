package cli

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

const keyFileSecret = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

// SEC4: a key read from a file is registered for redaction (the one from the environment already was).
func TestAPIKeyReadFromAFileIsRedacted(t *testing.T) {
	dir := t.TempDir()
	f := filepath.Join(dir, "key")
	if err := os.WriteFile(f, []byte("3fa91c07:"+keyFileSecret+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	a := &App{UI: &ui.Plain{Out: &out, ErrOut: &out}, Opt: &Options{APIKeyFile: f}, St: &state.Store{Dir: dir}, Secrets: &state.Secrets{}}
	k, ok, err := a.apiKey2K()
	if err != nil || !ok || k.ID != "3fa91c07" {
		t.Fatalf("%v %v %+v", ok, err, k)
	}
	line := "request failed for key 3fa91c07 secret " + keyFileSecret + " end"
	if got := a.Secrets.Redact(line); strings.Contains(got, keyFileSecret) {
		t.Fatalf("the secret of a key file is not redacted: %s", got)
	}
	if strings.Contains(out.String(), keyFileSecret) {
		t.Fatal("the secret was printed")
	}
}

func TestKeyFileNoteOnWindowsOnly(t *testing.T) {
	if n := keyFileNote("windows"); !strings.Contains(n, "not checked") {
		t.Fatalf("%q", n)
	}
	for _, goos := range []string{"linux", "darwin"} {
		if n := keyFileNote(goos); n != "" {
			t.Errorf("%s: %q", goos, n)
		}
	}
	// said once per run, and only on Windows
	dir := t.TempDir()
	f := filepath.Join(dir, "key")
	_ = os.WriteFile(f, []byte("3fa91c07:"+keyFileSecret+"\n"), 0o600)
	var out bytes.Buffer
	a := &App{UI: &ui.Plain{Out: &out, ErrOut: &out}, Opt: &Options{APIKeyFile: f}, St: &state.Store{Dir: dir}, Secrets: &state.Secrets{}}
	for i := 0; i < 3; i++ {
		if _, _, err := a.apiKey2K(); err != nil {
			t.Fatal(err)
		}
	}
	want := 0
	if runtime.GOOS == "windows" {
		want = 1
	}
	if got := strings.Count(out.String(), "not checked"); got != want {
		t.Fatalf("note said %d times on %s, want %d", got, runtime.GOOS, want)
	}
}
