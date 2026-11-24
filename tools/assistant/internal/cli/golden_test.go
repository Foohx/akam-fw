package cli_test

import (
	"bytes"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
)

// TestPlainOutputEqualsBash runs the scenarios of testdata/scenarios.txt in plain mode (no
// terminal, no colour) and compares exit status, stdout and stderr with the files that
// testdata/gen_golden.sh made from the bash wizard. The one known difference is `say` (its
// rehearsal line names the built-in WAV decoder), which is therefore not in the table.
//
// The commands that read and change the camera's settings differ ON PURPOSE since the 1080p's text port 8091 was closed to the
// network: the assistant goes through the signed API (its rehearsal says so, and `debug` is gone) while the bash wizard still
// drives the port over SSH, on the camera's own loopback. Those scenarios (testdata/go_only.txt) are compared with
// testdata/golden_go/<name>.golden, the reviewed output of the assistant; every other scenario still equals the wizard byte for byte.
func TestPlainOutputEqualsBash(t *testing.T) {
	goOnly := map[string]bool{}
	if b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "go_only.txt")); err == nil {
		for _, l := range strings.Fields(string(b)) {
			goOnly[l] = true
		}
	}
	sc, err := os.ReadFile(filepath.Join("..", "..", "testdata", "scenarios.txt"))
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, line := range strings.Split(string(sc), "\n") {
		name, args, ok := strings.Cut(line, "|")
		if !ok || name == "" {
			continue
		}
		n++
		t.Run(name, func(t *testing.T) {
			dir := "golden"
			if goOnly[name] {
				dir = "golden_go"
			}
			want, err := os.ReadFile(filepath.Join("..", "..", "testdata", dir, name+".golden"))
			if err != nil && !(goOnly[name] && os.Getenv("AKAM_UPDATE_GO_GOLDEN") != "") {
				t.Fatalf("missing golden file (run tools/assistant/testdata/gen_golden.sh): %v", err)
			}
			home := t.TempDir()
			t.Setenv("HOME", home)
			t.Setenv("AKAM_FORCE_TTY", "")
			t.Setenv("AKAM_STATE_DIR", "")
			t.Setenv("NO_COLOR", "1")
			st := filepath.Join(t.TempDir(), "state")
			var out, errb bytes.Buffer
			argv := append([]string{"--no-color", "--state-dir", st}, strings.Fields(args)...)
			rc := cli.Run(argv, cli.Streams{In: strings.NewReader(""), Out: &out, Err: &errb}, "test", cli.Hooks{})
			got := "rc=" + strconv.Itoa(rc) + "\n--- stdout ---\n" + strings.ReplaceAll(out.String(), st, "STATE") +
				"--- stderr ---\n" + strings.ReplaceAll(errb.String(), st, "STATE")
			if goOnly[name] && os.Getenv("AKAM_UPDATE_GO_GOLDEN") != "" { // review the diff of what this writes before committing it
				if err := os.MkdirAll(filepath.Join("..", "..", "testdata", dir), 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join("..", "..", "testdata", dir, name+".golden"), []byte(got), 0o644); err != nil {
					t.Fatal(err)
				}
				return
			}
			if got != string(want) {
				t.Errorf("differs from the golden file (%s)\n--- want ---\n%s\n--- go ---\n%s", dir, want, got)
			}
		})
	}
	if n < 10 {
		t.Fatalf("only %d scenarios", n)
	}
}
