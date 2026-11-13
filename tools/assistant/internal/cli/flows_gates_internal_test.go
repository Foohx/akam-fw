package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// funcBody returns the source of the named method of App in file.
func funcBody(t *testing.T, file, name string) string {
	t.Helper()
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	s := string(b)
	i := strings.Index(s, "func (a *App) "+name+"(")
	if i < 0 {
		t.Fatalf("%s: func %s not found", file, name)
	}
	rest := s[i:]
	if j := strings.Index(rest, "\nfunc "); j > 0 {
		rest = rest[:j]
	}
	return rest
}

// Every path that writes the camera flash or a flash card re-hashes the backup first (spec 11, section 9,
// point 8). The restore, the flash and the flash card need the STOCK backup (stockBackupProblem refuses
// kind=akam); the update accepts any verified backup (backupGate). An image that was not built by this run
// is structure-checked before it is recorded or flashed.
func TestEveryFlashPathRehashesTheBackupFirst(t *testing.T) {
	for _, c := range []struct{ file, fn, gate, before string }{
		{"flashcmds.go", "updateWrite", "a.backupGate()", "a.flashSSH("},
		{"flashcmds.go", "cmdRestore", "stockBackupProblem(", "a.flashSSH("},
		{"flashcmds.go", "cmdRestore", "stockBackupProblem(", "a.flashTelnet("},
		{"telnetcmds.go", "cmdFlash", "stockBackupProblem(", "a.flashSSH("},
		{"telnetcmds.go", "cmdFlash", "stockBackupProblem(", "a.flashTelnet("},
		{"sdcmds.go", "cmdPrepareFlashSD", "stockBackupProblem(", "a.sdConfirmErase("},
		{"flashcmds.go", "updateWrite", "a.imageGateChecked(", "a.flashSSH("},
		{"flashcmds.go", "cmdRestore", "a.imageGateChecked(", "a.flashSSH("},
		{"flashcmds.go", "cmdRestore", "a.imageGateChecked(", "a.flashTelnet("},
		{"telnetcmds.go", "cmdFlash", "a.imageGateChecked(", "a.flashSSH("},
		{"telnetcmds.go", "cmdFlash", "a.imageGateChecked(", "a.flashTelnet("},
		{"sdcmds.go", "cmdPrepareFlashSD", "a.imageSquashBytes(", "a.sdConfirmErase("},
		{"flows_update.go", "flowUpdate", "a.backupGate()", "a.updateWrite("},
		{"flows_update.go", "flowUpdate", "a.imageGateChecked(", "a.updateWrite("},
		{"flows_update.go", "installImage", "a.imageGateChecked(", `a.St.MarkDone("image")`},
	} {
		body := funcBody(t, c.file, c.fn)
		g, b := strings.Index(body, c.gate), strings.LastIndex(body, c.before) // the rehearsal branch before the gate never confirms
		if g < 0 || b < 0 || g > b {
			t.Errorf("%s.%s: %q must come before %q", c.file, c.fn, c.gate, c.before)
		}
	}
}

// Nothing but the audited commands calls the writers of the camera.
func TestOnlyAuditedCommandsCallTheFlashWriters(t *testing.T) {
	allowed := map[string]bool{"flashcmds.go": true, "telnetcmds.go": true}
	all, _ := filepath.Glob("*.go")
	for _, f := range all {
		if strings.HasSuffix(f, "_test.go") || allowed[f] {
			continue
		}
		b, _ := os.ReadFile(f)
		for _, w := range []string{"a.flashSSH(", "a.flashTelnet(", "flash.Flash("} {
			if strings.Contains(string(b), w) {
				t.Errorf("%s calls %s: only flashcmds.go and telnetcmds.go may", f, w)
			}
		}
	}
}
