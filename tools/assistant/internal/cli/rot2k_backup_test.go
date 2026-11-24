package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// backup-2k is the six-partition read-only backup of the 2K. The fake camera of the harness has the 1080p's eight
// partitions: the command must refuse it, and leave nothing in the destination.
func TestBackup2KCommand(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	dry := e.run("", "--dry-run", "--model", rotModel, "backup-2k")
	expectRC(t, dry, 0)
	contains(t, dry.out, "Back up the card-less 2K", "WOULD read /proc/mtd", "Nothing is written on the camera")

	out := filepath.Join(t.TempDir(), "backup")
	r := e.run("", "--model", rotModel, "backup-2k", "--out", out)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "not the layout of the 2K")
	if ents, _ := os.ReadDir(out); len(ents) != 0 {
		t.Errorf("a refused backup left %d file(s)", len(ents))
	}
	if e.readState("rot2k_backup") != "" {
		t.Error("a backup folder was recorded for a refused backup")
	}

	// a 1080p is not served by it
	e.writeState("camera_model=" + lsc1080 + "\n")
	r = e.run("", "--model", lsc1080, "backup-2k")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not served by it")
}

// key-2k: the dry run changes nothing, a key already kept on this PC is never replaced, a 1080p is refused.
func TestKey2KCommand(t *testing.T) {
	e := newEnv(t)
	e.noAPIKey() // a 2K whose PC has no key yet (the harness gives a 1080p install one by default)
	e.writeState("camera_model=" + rotModel + "\n")
	r := e.run("", "--dry-run", "--model", rotModel, "key-2k")
	expectRC(t, r, 0)
	contains(t, r.out, "Create the API key of the card-less 2K", "without printing it")
	if _, err := os.Stat(filepath.Join(e.state, "api_key")); err == nil {
		t.Fatal("a dry run wrote a key file")
	}
	secret := strings.Repeat("ab", 32)
	if err := os.WriteFile(filepath.Join(e.state, "api_key"), []byte("3fa91c07:"+secret+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = e.run("y\n", "--model", rotModel, "key-2k")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "already has an API key")
	notContains(t, r.out+r.err, secret)
	e.writeState("camera_model=" + lsc1080 + "\n")
	r = e.run("", "--model", lsc1080, "key-2k")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not served by it")
}
