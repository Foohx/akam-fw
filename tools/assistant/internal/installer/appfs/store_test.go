package appfs

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func failRenames(t *testing.T, match string) {
	t.Helper()
	old := rename
	rename = func(a, b string) error {
		if strings.Contains(filepath.Base(b), match) {
			return errors.New("simulated: file held by another program")
		}
		return old(a, b)
	}
	t.Cleanup(func() { rename = old })
}

// S2: a failure in the middle of a promotion leaves the store exactly as it was (one rename, never four).
func TestPromoteFailureLeavesTheStoreAsItWas(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	if err := u.store.Stage(p.Image, p.meta); err != nil {
		t.Fatal(err)
	}
	failRenames(t, "store.json")
	if err := u.store.Promote(); err == nil {
		t.Fatal("promotion reported success")
	}
	if cur, _, err := u.store.Load("current"); err != nil || !bytes.Equal(cur, u.old) {
		t.Fatalf("current changed: %v", err)
	}
	if pend, _, err := u.store.Load("pending"); err != nil || !bytes.Equal(pend, p.Image) {
		t.Fatalf("pending changed: %v", err)
	}
	if _, _, err := u.store.Load("previous"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("a previous image appeared: %v", err)
	}
	// and the update that follows still works once the file is free again
	rename = os.Rename
	if err := u.store.Promote(); err != nil {
		t.Fatal(err)
	}
	if prev, _, err := u.store.Load("previous"); err != nil || !bytes.Equal(prev, u.old) {
		t.Fatalf("previous: %v", err)
	}
}

func TestStageFailureDoesNotTouchCurrent(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	failRenames(t, "store.json")
	if err := u.store.Stage(p.Image, p.meta); err == nil {
		t.Fatal("staging reported success")
	}
	if cur, _, err := u.store.Load("current"); err != nil || !bytes.Equal(cur, u.old) {
		t.Fatalf("current: %v", err)
	}
	if _, _, err := u.store.Load("pending"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("pending: %v", err)
	}
}

func TestStoreLeavesNoTemporaryFile(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	if err := u.store.Stage(p.Image, p.meta); err != nil {
		t.Fatal(err)
	}
	if err := u.store.Promote(); err != nil {
		t.Fatal(err)
	}
	ents, _ := os.ReadDir(u.store.Dir)
	n := 0
	for _, e := range ents {
		if strings.HasSuffix(e.Name(), ".tmp") {
			t.Errorf("leftover %s", e.Name())
		}
		n++
	}
	// store.json + current (image, meta) + previous (image, meta); the pending files are the current ones
	if n != 5 {
		t.Errorf("%d files in the store, want 5", n)
	}
}

// A damaged record is refused with its reason, never used.
func TestStoreBadStates(t *testing.T) {
	idx := func(u *uworld) string { return filepath.Join(u.store.Dir, "store.json") }
	cases := map[string]struct {
		damage func(u *uworld)
		want   string
	}{
		"store.json garbage": {func(u *uworld) { _ = os.WriteFile(idx(u), []byte("{nope"), 0o600) }, "unreadable"},
		"current image file missing": {func(u *uworld) {
			_ = os.Remove(filepath.Join(u.store.Dir, sum(u.old)+".squashfs"))
		}, "missing or unreadable"},
		"metadata missing": {func(u *uworld) { _ = os.Remove(filepath.Join(u.store.Dir, sum(u.old)+".json")) }, "no metadata"},
		"metadata of another image": {func(u *uworld) {
			_ = os.WriteFile(filepath.Join(u.store.Dir, sum(u.old)+".json"), []byte(`{"sha256":"`+strings.Repeat("0", 64)+`"}`), 0o600)
		}, "does not match its checksum"},
		"image truncated": {func(u *uworld) {
			_ = os.WriteFile(filepath.Join(u.store.Dir, sum(u.old)+".squashfs"), u.old[:1000], 0o600)
		}, "does not match its checksum"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			u := newUWorld(t)
			c.damage(u)
			_, err := PrepareUpdate(u.in)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("want %q, got %v", c.want, err)
			}
			if errors.Is(err, os.ErrNotExist) {
				t.Fatal("a damaged record was reported as 'no record' (which suggests --current-image blindly)")
			}
		})
	}
}

// S3: --current-image never replaces a good record before a live step (a) proved it.
func TestNamedCurrentImageIsRecordedOnlyOnceProven(t *testing.T) {
	t.Run("a wrong file does not replace the record", func(t *testing.T) {
		u := newUWorld(t)
		stock := u.backup[3]
		wrongRes, err := Build(Inputs{Backup: stock, Kit: kitVersion(t, "0.1.0")})
		if err != nil {
			t.Fatal(err)
		}
		u.in.CurrentOverride = wrongRes.Image
		p := u.prepare(t)
		if _, err := RunUpdate(p, u.deps()); err == nil {
			t.Fatal("an update ran on a wrong current image")
		}
		if cur, _, err := u.store.Load("current"); err != nil || !bytes.Equal(cur, u.old) {
			t.Fatalf("the good record was replaced: %v", err)
		}
		if _, _, err := u.store.Load("pending"); err == nil {
			t.Fatal("pending left")
		}
		if !bytes.Equal(u.cam.mtd[3], u.old) {
			t.Fatal("mtd3 changed")
		}
	})
	t.Run("the right file is recorded once the write run proved it, and is the rollback target", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Store = ImageStore{Dir: filepath.Join(t.TempDir(), "fresh")}
		u.in.CurrentOverride = u.old
		p := u.prepare(t)
		if _, _, err := u.in.Store.Load("current"); err == nil {
			t.Fatal("recorded by preparing")
		}
		// the hot rehearsal skips step (a) and the write is declined: nothing proved the file, nothing is recorded
		d := u.deps()
		d.Asks.ConfirmWrite = func(string) (bool, error) { return false, nil }
		if _, err := RunUpdate(p, d); !errors.Is(err, ErrNotConfirmed) {
			t.Fatal(err)
		}
		if _, _, err := u.in.Store.Load("current"); err == nil {
			t.Fatal("recorded although no live step (a) compared it with the camera")
		}
		// the whole run: step (a) of the write run proves it
		p = u.prepare(t)
		if _, err := RunUpdate(p, u.deps()); err != nil {
			t.Fatalf("%v\n%s", err, u.outText())
		}
		if prev, _, err := u.in.Store.Load("previous"); err != nil || !bytes.Equal(prev, u.old) {
			t.Fatalf("previous: %v", err)
		}
		if cur, _, err := u.in.Store.Load("current"); err != nil || !bytes.Equal(cur, p.Image) {
			t.Fatalf("current: %v", err)
		}
	})
}
