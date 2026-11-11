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
func TestPlainOutputEqualsBash(t *testing.T) {
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
			want, err := os.ReadFile(filepath.Join("..", "..", "testdata", "golden", name+".golden"))
			if err != nil {
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
			if got != string(want) {
				t.Errorf("differs from the bash wizard\n--- bash ---\n%s\n--- go ---\n%s", want, got)
			}
		})
	}
	if n < 10 {
		t.Fatalf("only %d scenarios", n)
	}
}
