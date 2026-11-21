package appfs

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// uworld is a camera that runs the OLD Akam image, an assistant store that records it, and the kit of a NEW release.
type uworld struct {
	*world
	old, new  []byte
	kit       *Kit
	store     ImageStore
	in        UpdateInput
	asked     []string
	cycled    int
	detect    Camera
	detectErr error
	rehStatus []string // replaces the rehearsal's scripted statuses
}

func kitVersion(t *testing.T, ver string) *Kit {
	t.Helper()
	f := kitFiles()
	f[pathOverlay+"common.sh"] = []byte("#!/bin/sh\n# common.sh of " + ver + "\n")
	f[pathManifest] = bytes.Replace(f[pathManifest], []byte("akam_version=0.2.0"), []byte("akam_version="+ver), 1)
	return mustKit(t, sealKit(f))
}

func newUWorld(t *testing.T) *uworld {
	t.Helper()
	w := newWorld(t) // busybox pinned to the fixture's; backup dir built from the stock appfs
	stock := w.backup[3]
	oldK, newK := kitVersion(t, "0.2.0"), kitVersion(t, "0.3.0")
	oldRes, err := Build(Inputs{Backup: stock, Kit: oldK})
	if err != nil {
		t.Fatal(err)
	}
	u := &uworld{world: w, old: oldRes.Image, kit: newK, store: ImageStore{Dir: filepath.Join(t.TempDir(), "images")},
		detect: Camera{Model: "lsc-rotatable-2k-ak3918av130", Version: "0.3.0-abc"}}
	if err := u.store.SetCurrent(u.old, ImageMeta{SHA256: oldRes.SHA256, Version: "0.2.0", Commit: "abc123"}); err != nil {
		t.Fatal(err)
	}
	// the camera runs the old image, card-less
	w.cam.mtd[3] = append([]byte(nil), u.old...)
	w.cam.hotResult = "OK"
	u.in = UpdateInput{ModelID: "lsc-rotatable-2k-ak3918av130", Camera: Camera{Model: "lsc-rotatable-2k-ak3918av130", Version: "0.2.0"},
		Kit: newK, BackupDir: w.plan.BackupDir, Store: u.store, Hot: true, Target: "root@192.0.2.10", WorkDir: w.workDir}
	return u
}

func (u *uworld) deps() RunDeps {
	flash := func(p Plan, c Confirmation, d Deps) (*FlashResult, error) {
		switch {
		case p.HotPreflight:
			u.cam.status = []string{"WAIT_wpa|x", "WAIT_dbalt|x", "WAIT_db22|x", "DONE_RESTORED|rehearsal"}
			if u.rehStatus != nil {
				u.cam.status = u.rehStatus
			}
		case p.Hot:
			u.cam.status = []string{"", "H1|scan", "WAIT_wpa|x", "WAIT_dbalt|x", "WAIT_db22|x", "WAIT_upload|x", "WAIT_go|x", "WRITING|x"}
		}
		return Flash(p, c, d)
	}
	return RunDeps{
		Flash: flash,
		FlashDeps: Deps{
			Dial:  func(port int) (Conn, error) { u.cam.dials = append(u.cam.dials, port); return u.cam, nil },
			Out:   func(l string) { u.out = append(u.out, l) },
			Sleep: func(time.Duration) {},
			Ctx:   context.Background(),
		},
		Asks: Asks{
			AckRecovery:      func() (bool, error) { u.asked = append(u.asked, "recovery"); return true, nil },
			ConfirmRehearsal: func() (bool, error) { u.asked = append(u.asked, "rehearsal"); return true, nil },
			ConfirmWrite: func(sha string) (bool, error) {
				u.asked = append(u.asked, "write:"+sha)
				return true, nil
			},
		},
		Detect:     func(context.Context) (Camera, error) { return u.detect, u.detectErr },
		PowerCycle: func() error { u.cycled++; return nil },
		Sleep:      func(time.Duration) {},
		Say:        func(s string) { u.out = append(u.out, s) },
		Tries:      3,
	}
}

func (u *uworld) prepare(t *testing.T) *Prepared {
	t.Helper()
	p, err := PrepareUpdate(u.in)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

func TestPrepareShowsWhatChanges(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	if p.FromVersion != "0.2.0" || p.ToVersion != "0.3.0" || p.SHA == p.CurrentSHA || len(p.Image) != PartitionSize {
		t.Fatalf("%+v", p)
	}
	found := false
	for _, c := range p.Changes {
		if c.Path == "/akam/common.sh" && c.Kind == "changed" {
			found = true
		}
		if strings.HasPrefix(c.Path, "/bin/") || strings.HasPrefix(c.Path, "/lib/") {
			t.Errorf("a stock file changed: %v", c)
		}
	}
	if !found {
		t.Fatalf("changes: %v", p.Changes)
	}
	sum := strings.Join(p.Summary(), "\n")
	for _, want := range []string{"0.2.0 -> 0.3.0", p.SHA[:16], "/akam/common.sh", "card-less", "previous image is kept"} {
		if !strings.Contains(sum, want) {
			t.Errorf("summary lacks %q:\n%s", want, sum)
		}
	}
	if len(u.cam.cmds) != 0 {
		t.Fatal("preparing talked to the camera")
	}
}

func TestPrepareRefusals(t *testing.T) {
	t.Run("another camera model", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Camera.Model = "lsc-indoor-1080p-ak3918"
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "refused") {
			t.Fatal(err)
		}
	})
	t.Run("kit of another model", func(t *testing.T) {
		u := newUWorld(t)
		f := kitFiles()
		f[pathManifest] = bytes.Replace(f[pathManifest], []byte("model=lsc-rotatable-2k-ak3918av130"), []byte("model=lsc-indoor-1080p-ak3918"), 1)
		u.in.Kit = mustKit(t, sealKit(f))
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "this kit is for") {
			t.Fatal(err)
		}
	})
	t.Run("no record of the image on the camera", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Store = ImageStore{Dir: t.TempDir()}
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "--current-image") {
			t.Fatal(err)
		}
	})
	t.Run("current image given by the operator", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Store = ImageStore{Dir: t.TempDir()}
		u.in.CurrentOverride = u.old
		p := u.prepare(t)
		if !strings.Contains(p.CurrentFrom, "--current-image") {
			t.Fatal(p.CurrentFrom)
		}
		u.in.CurrentOverride = []byte("short")
		if _, err := PrepareUpdate(u.in); err == nil {
			t.Fatal("a short current image was accepted")
		}
	})
	t.Run("stored image altered", func(t *testing.T) {
		u := newUWorld(t)
		img := filepath.Join(u.store.Dir, sum(u.old)+".squashfs")
		b, _ := os.ReadFile(img)
		b[1000] ^= 1
		_ = os.WriteFile(img, b, 0o600)
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "does not match its checksum") {
			t.Fatal(err)
		}
	})
	t.Run("already up to date", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Kit = kitVersion(t, "0.2.0")
		if _, err := PrepareUpdate(u.in); !errors.Is(err, ErrUpToDate) {
			t.Fatal(err)
		}
	})
	t.Run("downgrade needs force", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Camera.Version = "0.9.0"
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "older") {
			t.Fatal(err)
		}
		u.in.Force = true
		if _, err := PrepareUpdate(u.in); err != nil {
			t.Fatal(err)
		}
	})
	t.Run("no backup", func(t *testing.T) {
		u := newUWorld(t)
		u.in.BackupDir = t.TempDir()
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "mtd3.bin") {
			t.Fatal(err)
		}
	})
	t.Run("rollback without a previous image", func(t *testing.T) {
		u := newUWorld(t)
		u.in.Rollback, u.in.Kit = true, nil
		if _, err := PrepareUpdate(u.in); err == nil || !strings.Contains(err.Error(), "no previous image") {
			t.Fatal(err)
		}
	})
}

func TestRunUpdateHotHappyPath(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	d := u.deps()
	// the operator power-cycles: the camera comes back with the new image (the runner wrote it)
	res, err := RunUpdate(p, d)
	if err != nil {
		t.Fatalf("%v\n%s", err, u.outText())
	}
	if !res.Rehearsed || !res.Written || !res.Verified || res.VersionSeen != "0.3.0-abc" || res.Note != "" {
		t.Fatalf("%+v", res)
	}
	if want := []string{"recovery", "rehearsal", "recovery", "write:" + p.SHA}; strings.Join(u.asked, ",") != strings.Join(want, ",") {
		t.Fatalf("questions %v, want %v", u.asked, want)
	}
	// the rehearsal ran before the write, and never wrote
	reh, hot := -1, -1
	for i, c := range u.cam.cmds {
		if strings.Contains(c, " run rehearsal ") && reh < 0 {
			reh = i
		}
		if strings.Contains(c, " run hot ") && hot < 0 {
			hot = i
		}
	}
	if reh < 0 || hot < 0 || reh > hot {
		t.Fatalf("rehearsal at %d, write at %d", reh, hot)
	}
	if !bytes.Equal(u.cam.mtd[3], p.Image) || u.cycled != 1 {
		t.Fatal("mtd3 / power cycle")
	}
	u.assertNeverRebooted()
	// the stored images: current is the new one, previous is the old one (the rollback target)
	cur, cm, err := u.store.Load("current")
	if err != nil || !bytes.Equal(cur, p.Image) || cm.Version != "0.3.0" {
		t.Fatalf("current: %v %v", err, cm)
	}
	prev, pm, err := u.store.Load("previous")
	if err != nil || !bytes.Equal(prev, u.old) || pm.Version != "0.2.0" {
		t.Fatalf("previous: %v %v", err, pm)
	}
	if _, _, err := u.store.Load("pending"); err == nil {
		t.Error("a pending image is left")
	}
	// and a rollback is now possible: the previous image comes back, with the same gates
	u.in.Rollback, u.in.Kit, u.in.Camera.Version = true, nil, "0.3.0"
	rp, err := PrepareUpdate(u.in)
	if err != nil || !bytes.Equal(rp.Image, u.old) || !strings.Contains(strings.Join(rp.Summary(), "\n"), "rollback") {
		t.Fatalf("rollback prepare: %v", err)
	}
}

func (u *uworld) outText() string { return strings.Join(u.out, "\n") }

func TestNothingIsSentWithoutAnswers(t *testing.T) {
	cases := map[string]func(*RunDeps){
		"no questions at all": func(d *RunDeps) { d.Asks = Asks{} },
		"recovery refused": func(d *RunDeps) {
			d.Asks.AckRecovery = func() (bool, error) { return false, nil }
		},
		"rehearsal refused": func(d *RunDeps) {
			d.Asks.ConfirmRehearsal = func() (bool, error) { return false, nil }
		},
		"no terminal": func(d *RunDeps) {
			d.Asks.AckRecovery = func() (bool, error) { return false, errors.New("no terminal") }
		},
	}
	for name, mod := range cases {
		t.Run(name, func(t *testing.T) {
			u := newUWorld(t)
			p := u.prepare(t)
			d := u.deps()
			mod(&d)
			if _, err := RunUpdate(p, d); err == nil {
				t.Fatal("ran without an answer")
			}
			if len(u.cam.cmds) != 0 || len(u.cam.dials) != 0 {
				t.Fatalf("the camera was contacted: %v", u.cam.cmds)
			}
			if _, _, err := u.store.Load("pending"); err == nil {
				t.Error("a pending image is left")
			}
		})
	}
}

func TestDecliningTheWriteLeavesTheCameraAsItWas(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	d := u.deps()
	d.Asks.ConfirmWrite = func(string) (bool, error) { return false, nil }
	res, err := RunUpdate(p, d)
	if !errors.Is(err, ErrNotConfirmed) || res.Written {
		t.Fatalf("%v %+v", err, res)
	}
	if u.cam.count(" run rehearsal ") != 1 || u.cam.count(" run hot ") != 0 || u.cam.count("touch "+hotDir+"/confirm.go") != 0 {
		t.Fatalf("commands: %v", u.cam.cmds)
	}
	if !bytes.Equal(u.cam.mtd[3], u.old) || u.cycled != 0 {
		t.Fatal("mtd3 changed or the operator was asked to power-cycle")
	}
	cur, _, err := u.store.Load("current")
	if err != nil || !bytes.Equal(cur, u.old) {
		t.Fatal("the recorded current image changed")
	}
	if _, _, err := u.store.Load("previous"); err == nil {
		t.Fatal("a previous image appeared")
	}
}

func TestRehearsalFailureStopsBeforeTheQuestionOfTheWrite(t *testing.T) {
	u := newUWorld(t)
	p := u.prepare(t)
	u.rehStatus = []string{"ABORTED_UNCHANGED|memory"}
	d := u.deps()
	if _, err := RunUpdate(p, d); err == nil || !strings.Contains(err.Error(), "rehearsal failed, nothing was written") {
		t.Fatal(err)
	}
	for _, a := range u.asked {
		if strings.HasPrefix(a, "write:") {
			t.Fatal("the write was asked after a failed rehearsal")
		}
	}
	if !bytes.Equal(u.cam.mtd[3], u.old) {
		t.Fatal("mtd3 changed")
	}
}

func TestVerificationFailuresDoNotRecordTheUpdate(t *testing.T) {
	t.Run("mtd3 is not the new image after the reboot", func(t *testing.T) {
		u := newUWorld(t)
		p := u.prepare(t)
		d := u.deps()
		d.PowerCycle = func() error { u.cam.mtd[3] = append([]byte(nil), u.old...); return nil }
		res, err := RunUpdate(p, d)
		if err == nil || !strings.Contains(err.Error(), "NOT updated") || res.Verified {
			t.Fatalf("%v %+v", err, res)
		}
		if cur, _, _ := u.store.Load("current"); !bytes.Equal(cur, u.old) {
			t.Fatal("current was promoted")
		}
	})
	t.Run("the camera never comes back", func(t *testing.T) {
		u := newUWorld(t)
		p := u.prepare(t)
		d := u.deps()
		inner := d.FlashDeps.Dial
		dials := 0
		d.FlashDeps.Dial = func(port int) (Conn, error) {
			if u.cycled > 0 {
				dials++
				return nil, errors.New("no route")
			}
			return inner(port)
		}
		d.PowerCycle = func() error { u.cycled++; return nil }
		_, err := RunUpdate(p, d)
		if err == nil || !strings.Contains(err.Error(), "did not come back") || !strings.Contains(err.Error(), "SD card") {
			t.Fatal(err)
		}
		if dials != 3 {
			t.Errorf("%d reconnect attempts, want 3", dials)
		}
		if cur, _, _ := u.store.Load("current"); !bytes.Equal(cur, u.old) {
			t.Fatal("current was promoted")
		}
	})
	t.Run("another version is reported: warned, image still verified", func(t *testing.T) {
		u := newUWorld(t)
		u.detect = Camera{Version: "9.9.9"}
		p := u.prepare(t)
		res, err := RunUpdate(p, u.deps())
		if err != nil || !res.Verified || !strings.Contains(res.Note, "9.9.9") {
			t.Fatalf("%v %+v", err, res)
		}
	})
	t.Run("no API: verified by the image alone, said so", func(t *testing.T) {
		u := newUWorld(t)
		u.detectErr = errors.New("closed")
		p := u.prepare(t)
		res, err := RunUpdate(p, u.deps())
		if err != nil || !res.Verified || !strings.Contains(res.Note, "version was not checked") {
			t.Fatalf("%v %+v", err, res)
		}
	})
}

func TestRunUpdateFromACardBootUsesADryRunFirst(t *testing.T) {
	u := newUWorld(t)
	u.in.Hot = false
	u.cam.sdMount = "/dev/mmcblk0p1 vfat"
	p := u.prepare(t)
	d := u.deps()
	res, err := RunUpdate(p, d)
	if err != nil {
		t.Fatalf("%v\n%s", err, u.outText())
	}
	if res.Rehearsed || !res.Written || !res.Verified {
		t.Fatalf("%+v", res)
	}
	// the dry run asked nothing and sent nothing; then recovery + typed go, once
	if want := []string{"recovery", "write:" + p.SHA}; strings.Join(u.asked, ",") != strings.Join(want, ",") {
		t.Fatalf("questions %v", u.asked)
	}
	if !strings.Contains(u.outText(), "DRY RUN") {
		t.Error("no dry run")
	}
}
