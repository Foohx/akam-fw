//go:build akamtest

package cli_test

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// A hostile /proc/mtd (extra, reordered, repeated, oversized or endless) never makes a read happen
// nor a folder appear: the layout must be exactly the supported one and is read with a cap.
func TestBackupSSHHostileProcMtdVariants(t *testing.T) {
	good := testfake.ProcMtdText
	lines := strings.Split(strings.TrimRight(good, "\n"), "\n") // lines[0] is the header
	variants := map[string]string{
		"extra partition":    good + "mtd8: 00001000 00001000 \"EVIL\"\n",
		"repeated partition": good + lines[8] + "\n",
		"reordered":          lines[0] + "\n" + lines[2] + "\n" + lines[1] + "\n" + strings.Join(lines[3:], "\n") + "\n",
		"path in name":       strings.Replace(good, `"UBOOT"`, `"../../x"`, 1),
		"endless":            strings.Repeat("mtd0: 00032000 00001000 \"UBOOT\"\n", 200000),
		"one very long line": good + strings.Repeat("A", 1<<20) + "\n",
		"missing partition":  strings.Join(lines[:8], "\n") + "\n",
		"trailing blank":     strings.Replace(good, "00500000", "00500000 ", 1),
	}
	for name, text := range variants {
		e, root := sshBackupWorld(t, func(o *testfake.Options) { o.ProcMtd = text })
		r := e.run("", "backup")
		if r.rc == 0 {
			t.Errorf("%s: accepted", name)
		}
		if ddCommands(e) != 0 || len(sshBackupDirs(root)) != 0 {
			t.Errorf("%s: a read was sent or a folder made", name)
		}
		if len(r.out+r.err) > 8192 {
			t.Errorf("%s: %d bytes printed", name, len(r.out+r.err))
		}
	}
}
