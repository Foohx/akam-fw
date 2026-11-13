//go:build akamtest

package cli

import (
	"path/filepath"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/profile"
)

func TestDiffTreatsTildeAndAbsoluteKeyPathsAsOneKey(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	abs := filepath.Join(home, ".ssh", "id_work.pub")
	live, want, sec := profile.New(profile.Settings), profile.New(profile.Settings), profile.New(profile.Secrets)
	for k, v := range map[string]string{profile.KeyKind: "custom", profile.KeyPath: abs} {
		live.Vals[k] = v
	}
	want.Vals[profile.KeyKind], want.Vals[profile.KeyPath] = "custom", "~/.ssh/id_work.pub"
	if ch, _ := cfgDiff(live, want, sec, nil, false); len(ch) != 0 {
		t.Errorf("a ~ path and its absolute file are one key, got %+v", ch)
	}
	want.Vals[profile.KeyPath] = "~/.ssh/other.pub"
	if ch, _ := cfgDiff(live, want, sec, nil, false); len(ch) != 1 || ch[0].Op != opKey {
		t.Errorf("another file is a change: %+v", ch)
	}
}
