package cli_test

import (
	"os"
	"path/filepath"
	"testing"
)

// `update` on a 2K is its own flow: the 1080p update is never reached, and nothing is written without a terminal.
func TestUpdateOfA2KIsItsOwnFlow(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	// a rehearsal lists the steps, runs none of them
	r := e.run("", "--dry-run", "--model", rotModel, "update")
	expectRC(t, r, 0)
	contains(t, r.out, "Update the card-less 2K", "read the model and the Akam version", "rehearsal on the camera",
		"explicit confirmation of the write", "never rebooted")
	notContains(t, r.out+r.err, "not yet supported for this model")
	// the rollback has its own title
	r = e.run("", "--dry-run", "--model", rotModel, "update", "--rollback")
	expectRC(t, r, 0)
	contains(t, r.out, "Roll the card-less 2K back")
	// a real run needs a terminal, even with --yes and --recovery-at-hand: nothing is read, built or sent
	_, bdir := appfsDirs(t)
	r = e.run("yes\nyes\nyes\n", "update", "--yes", "--recovery-at-hand", "--backup-dir", bdir, "--kit", t.TempDir())
	if r.rc == 0 {
		t.Fatalf("an update ran without a terminal:\n%s%s", r.out, r.err)
	}
	notContains(t, r.out, "What will change", "LIVE run")
	if _, err := os.Stat(filepath.Join(e.state, "appfs-images")); err == nil {
		t.Error("the image store was created: the run went past the terminal gate")
	}
}

// --yes is explicit and never alone for the recovery question; --stock is the 1080p way back.
func TestUpdateOf2KFlagRules(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	r := e.run("", "update", "--recovery-at-hand")
	expectRC(t, r, 2)
	contains(t, r.out+r.err, "--recovery-at-hand only goes with --yes")
	r = e.run("", "update", "--stock")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "--rollback")
}

// A 1080p keeps its own update: the 2K flow is chosen by the firmware kind, not offered to every camera.
func TestUpdateOfA1080pIsNotTheCardlessFlow(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + lsc1080 + "\ncamera_ip=192.0.2.10\n")
	r := e.run("", "--dry-run", "--model", lsc1080, "update")
	notContains(t, r.out+r.err, "card-less 2K")
}

func TestPrepare2KSD(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	r := e.run("", "--dry-run", "--model", rotModel, "prepare-2k-sd")
	expectRC(t, r, 0)
	contains(t, r.out, "first-install SD card", "WOULD verify the 2K kit")
	// refused on a 1080p and with no terminal; no real disk is ever asked (newEnv checks it)
	e.writeState("camera_model=" + lsc1080 + "\n")
	r = e.run("", "prepare-2k-sd")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not served by it")
	e.writeState("camera_model=" + rotModel + "\n")
	r = e.run("", "prepare-2k-sd")
	if r.rc == 0 {
		t.Fatalf("a card was prepared without a terminal:\n%s%s", r.out, r.err)
	}
}

// The card disables the rescue telnet unless --rescue-telnet asks for it, and the rehearsal says which.
func TestPrepare2KSDTelnetWording(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\n")
	r := e.run("", "--dry-run", "--model", rotModel, "prepare-2k-sd")
	expectRC(t, r, 0)
	contains(t, r.out, "disables the rescue telnet", "akam/no_telnet")
	notContains(t, r.out, "NO password")
	r = e.run("", "--dry-run", "--model", rotModel, "prepare-2k-sd", "--rescue-telnet")
	expectRC(t, r, 0)
	contains(t, r.out, "NO password", "rescue only")
	notContains(t, r.out, "disables the rescue telnet")
}

// seed-2k: refused on a 1080p, the rehearsal changes nothing and says what it would do, a real run needs a terminal.
func TestSeed2KCommand(t *testing.T) {
	e := newEnv(t)
	e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
	r := e.run("", "--dry-run", "--model", rotModel, "seed-2k")
	expectRC(t, r, 0)
	contains(t, r.out, "/config/akam", "never overwritten", "Nothing secret is shown")
	r = e.run("", "--model", rotModel, "seed-2k")
	if r.rc == 0 {
		t.Fatalf("seeding ran without a terminal:\n%s%s", r.out, r.err)
	}
	r = e.run("", "--model", lsc1080, "seed-2k")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "is not served by it")
}
