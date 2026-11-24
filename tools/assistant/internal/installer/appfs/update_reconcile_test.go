package appfs

import (
	"bytes"
	"context"
	"errors"
	"strings"
	"testing"
)

// S1: a write that the PC did not finish recording is settled from a read-only dump of mtd3.
func TestReconcileSettlesAnUnrecordedWrite(t *testing.T) {
	stage := func(t *testing.T) (*uworld, *Prepared, func(int) (Conn, error)) {
		u := newUWorld(t)
		p := u.prepare(t)
		if err := u.store.Stage(p.Image, p.meta); err != nil {
			t.Fatal(err)
		}
		return u, p, func(port int) (Conn, error) { u.cam.dials = append(u.cam.dials, port); return u.cam, nil }
	}
	t.Run("the camera runs the pending image: promoted", func(t *testing.T) {
		u, p, dial := stage(t)
		u.cam.mtd[3] = append([]byte(nil), p.Image...)
		r, err := u.store.Reconcile(context.Background(), dial, "", nil)
		if err != nil || r != ReconPromoted {
			t.Fatalf("%v %v", r, err)
		}
		cur, _, err := u.store.Load("current")
		if err != nil || !bytes.Equal(cur, p.Image) {
			t.Fatalf("current: %v", err)
		}
		if prev, _, err := u.store.Load("previous"); err != nil || !bytes.Equal(prev, u.old) {
			t.Fatalf("previous: %v", err)
		}
		if _, _, err := u.store.Load("pending"); err == nil {
			t.Fatal("pending kept")
		}
		for _, c := range u.cam.cmds {
			if strings.Contains(c, "flashcp") || strings.Contains(c, "cat > ") || strings.Contains(c, "reboot") {
				t.Fatalf("the settlement is not read only: %q", c)
			}
		}
	})
	t.Run("the camera still runs the current image: discarded", func(t *testing.T) {
		u, _, dial := stage(t)
		r, err := u.store.Reconcile(context.Background(), dial, "", nil)
		if err != nil || r != ReconDiscarded {
			t.Fatalf("%v %v", r, err)
		}
		if cur, _, err := u.store.Load("current"); err != nil || !bytes.Equal(cur, u.old) {
			t.Fatal("current changed")
		}
		if _, _, err := u.store.Load("pending"); err == nil {
			t.Fatal("pending kept")
		}
	})
	t.Run("neither: refused with both hashes, nothing changed", func(t *testing.T) {
		u, p, dial := stage(t)
		other := append([]byte(nil), u.old...)
		other[2000] ^= 0xff
		u.cam.mtd[3] = other
		_, err := u.store.Reconcile(context.Background(), dial, "", nil)
		if err == nil {
			t.Fatal("accepted")
		}
		for _, want := range []string{sum(other), p.SHA, sum(u.old), "--current-image FILE"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("refusal lacks %q: %v", want, err)
			}
		}
		if _, _, err := u.store.Load("pending"); err != nil {
			t.Fatal("pending was dropped on a refusal")
		}
		if cur, _, _ := u.store.Load("current"); !bytes.Equal(cur, u.old) {
			t.Fatal("current changed")
		}
	})
	t.Run("the operator's image equals mtd3: pending dropped", func(t *testing.T) {
		u, _, dial := stage(t)
		other := append([]byte(nil), u.old...)
		other[2000] ^= 0xff
		u.cam.mtd[3] = other
		if r, err := u.store.Reconcile(context.Background(), dial, sum(other), nil); err != nil || r != ReconDiscarded {
			t.Fatalf("%v %v", r, err)
		}
	})
	t.Run("the camera cannot be reached: refused with the next command", func(t *testing.T) {
		u, _, _ := stage(t)
		_, err := u.store.Reconcile(context.Background(), func(int) (Conn, error) { return nil, errors.New("no route") }, "", nil)
		if err == nil || !strings.Contains(err.Error(), "akam update") {
			t.Fatal(err)
		}
		if _, _, err := u.store.Load("pending"); err != nil {
			t.Fatal("pending dropped")
		}
	})
	t.Run("nothing pending: the camera is not contacted", func(t *testing.T) {
		u := newUWorld(t)
		r, err := u.store.Reconcile(context.Background(), func(int) (Conn, error) { t.Fatal("dialed"); return nil, nil }, "", nil)
		if err != nil || r != ReconNone {
			t.Fatalf("%v %v", r, err)
		}
	})
}

// U1: a verified first flash records the image, so that the first update has a current image (and a rollback target).
func TestRecordFlashedMakesTheFirstUpdatePossible(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	fresh := ImageStore{Dir: t.TempDir()}
	u.in.Store = fresh
	if _, err := PrepareUpdate(u.in); err == nil {
		t.Fatal("an update was prepared with no record")
	}
	if err := fresh.RecordFlashed(nil, ImageMeta{}, u.old, ImageMeta{SHA256: sum(u.old), Version: "0.2.0"}); err != nil {
		t.Fatal(err)
	}
	cur, _, err := fresh.Load("current")
	if err != nil || !bytes.Equal(cur, u.old) {
		t.Fatalf("current: %v", err)
	}
	if _, err := PrepareUpdate(u.in); err != nil {
		t.Fatal(err)
	}
	// a second flash made by the same tool keeps the first as the rollback target
	if err := fresh.RecordFlashed(u.old, ImageMeta{SHA256: sum(u.old)}, p.Image, p.meta); err != nil {
		t.Fatal(err)
	}
	if prev, _, err := fresh.Load("previous"); err != nil || !bytes.Equal(prev, u.old) {
		t.Fatalf("previous: %v", err)
	}
	if err := fresh.RecordFlashed(nil, ImageMeta{}, p.Image, ImageMeta{SHA256: "bad"}); err == nil {
		t.Fatal("an image that does not match its checksum was recorded")
	}
}
