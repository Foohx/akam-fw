//go:build akamtest

package cli

import (
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

// The default folder is <Documents>/Akam backups/<camera>/<date>; the test hook keeps the legacy layout.
func TestBackupBaseLayouts(t *testing.T) {
	t.Setenv("AKAM_BACKUP_ROOT", "/r")
	a := &App{Cams: &state.Cameras{Name: "salon"}}
	base, cloud, _, _ := a.backupBase(time.Date(2026, 11, 11, 0, 0, 0, 0, time.UTC))
	if cloud != "" || !strings.HasPrefix(filepath.ToSlash(base), "/r/akam-backup-") {
		t.Errorf("hook layout: %s", base)
	}
	t.Setenv("AKAM_BACKUP_ROOT", "")
	base, _, _, par := a.backupBase(time.Date(2026, 11, 11, 0, 0, 0, 0, time.UTC))
	if !strings.Contains(filepath.ToSlash(base), "/Akam backups/") || strings.Contains(base, "akam-backup-") {
		t.Errorf("default layout: %s", base)
	}
	if len(par) != 2 || filepath.Base(par[1]) != "salon" || filepath.Base(filepath.Dir(base)) != "salon" || filepath.Base(base) != "20261111" {
		t.Errorf("camera folder layout: %s %v", base, par)
	}
}
