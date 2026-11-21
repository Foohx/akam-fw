package appfs

import (
	"bytes"
	"strings"
	"testing"
)

// T1: the rollback end to end. An update (old -> new), then a rollback with the same gates: the camera gets the old
// image back, the store swaps current and previous, the questions are the same, nothing reboots the camera.
func TestRunUpdateRollbackEndToEnd(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	if _, err := RunUpdate(p, u.deps()); err != nil {
		t.Fatalf("the update: %v\n%s", err, u.outText())
	}
	if !bytes.Equal(u.cam.mtd[3], p.Image) {
		t.Fatal("the update did not write the new image")
	}
	newImg := p.Image

	// the camera runs the new version now; the rollback needs no kit
	u.in.Rollback, u.in.Kit = true, nil
	u.in.WorkDir = t.TempDir() + "/work2" // a run needs a fresh work dir (the CLI names it by the time)
	u.in.Camera.Version = "0.3.0"
	u.detect = Camera{Version: "0.2.0"}
	u.asked, u.cycled = nil, 0
	u.cam.cmds = nil
	rp, err := PrepareUpdate(u.in)
	if err != nil {
		t.Fatal(err)
	}
	if !rp.Rollback || !bytes.Equal(rp.Image, u.old) || rp.ToVersion != "0.2.0" || rp.FromVersion != "0.3.0" {
		t.Fatalf("%+v", rp)
	}
	if !strings.Contains(strings.Join(rp.Summary(), "\n"), "rollback") {
		t.Fatal("the summary does not say rollback")
	}
	for _, w := range rp.Warnings {
		t.Errorf("a clean rollback target carries a warning: %s", w)
	}
	res, err := RunUpdate(rp, u.deps())
	if err != nil {
		t.Fatalf("the rollback: %v\n%s", err, u.outText())
	}
	if !res.Rehearsed || !res.Written || !res.Verified || res.VersionSeen != "0.2.0" || res.Note != "" {
		t.Fatalf("%+v", res)
	}
	// same questions, same gates, once each: rehearsal (recovery + yes), then recovery + the typed go for THIS image
	if want := []string{"recovery", "rehearsal", "recovery", "write:" + sum(u.old)}; strings.Join(u.asked, ",") != strings.Join(want, ",") {
		t.Fatalf("questions %v, want %v", u.asked, want)
	}
	if !bytes.Equal(u.cam.mtd[3], u.old) {
		t.Fatal("mtd3 is not the old image after the rollback")
	}
	if u.cam.count(" run rehearsal ") != 1 || u.cam.count(" run hot ") != 1 || u.cycled != 1 {
		t.Fatalf("rehearsals %d, writes %d, power cycles %d", u.cam.count(" run rehearsal "), u.cam.count(" run hot "), u.cycled)
	}
	u.assertNeverRebooted()
	// the store is swapped: current is the old image, previous the new one, nothing pending
	cur, cm, err := u.store.Load("current")
	if err != nil || !bytes.Equal(cur, u.old) || cm.Version != "0.2.0" {
		t.Fatalf("current: %v %+v", err, cm)
	}
	prev, pm, err := u.store.Load("previous")
	if err != nil || !bytes.Equal(prev, newImg) || pm.Version != "0.3.0" {
		t.Fatalf("previous: %v %+v", err, pm)
	}
	if _, _, err := u.store.Load("pending"); err == nil {
		t.Fatal("a pending image is left")
	}
	// and the rollback can be rolled back (forward again)
	u.in.Camera.Version = "0.2.0"
	fp, err := PrepareUpdate(u.in)
	if err != nil || !bytes.Equal(fp.Image, newImg) {
		t.Fatalf("roll forward: %v", err)
	}
}

// The rollback goes through the same gate as an update: a camera whose mtd3 is not the recorded current image is not
// written, and the store is left alone.
func TestRollbackIsRefusedWhenTheCameraIsNotWhatTheRecordSays(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	if _, err := RunUpdate(p, u.deps()); err != nil {
		t.Fatal(err)
	}
	u.in.Rollback, u.in.Kit = true, nil
	u.in.WorkDir = t.TempDir() + "/work2"
	rp, err := PrepareUpdate(u.in)
	if err != nil {
		t.Fatal(err)
	}
	other := append([]byte(nil), u.cam.mtd[3]...)
	other[3000] ^= 0xff // somebody changed the camera since
	u.cam.mtd[3] = other
	if _, err := RunUpdate(rp, u.deps()); err == nil || !strings.Contains(err.Error(), "differs from CURRENT_IMAGE") {
		t.Fatalf("%v", err)
	}
	if !bytes.Equal(u.cam.mtd[3], other) {
		t.Fatal("mtd3 was written")
	}
	if cur, _, err := u.store.Load("current"); err != nil || !bytes.Equal(cur, p.Image) {
		t.Fatalf("current changed: %v", err)
	}
	if prev, _, err := u.store.Load("previous"); err != nil || !bytes.Equal(prev, u.old) {
		t.Fatalf("previous changed: %v", err)
	}
	if _, _, err := u.store.Load("pending"); err == nil {
		t.Fatal("a pending image is left")
	}
}
