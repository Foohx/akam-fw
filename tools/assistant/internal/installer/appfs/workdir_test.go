package appfs

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func workFiles(t *testing.T, dir string) map[string]bool {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	m := map[string]bool{}
	for _, e := range ents {
		m[e.Name()] = true
	}
	return m
}

// S6: the work directory of an update does not keep the dumps. The Wi-Fi config partition (mtd4) is never kept; the
// other dumps go once the update is verified, and stay when something failed (what an operator looks at).
func TestWorkDirIsCleanedAfterAnUpdate(t *testing.T) {
	t.Run("verified: no dump is left", func(t *testing.T) {
		u := newUWorld(t)
		p := u.prepare(t)
		if _, err := RunUpdate(p, u.deps()); err != nil {
			t.Fatalf("%v\n%s", err, u.outText())
		}
		got := workFiles(t, u.in.WorkDir)
		for n := range got {
			if filepath.Ext(n) == ".bin" {
				t.Errorf("%s was left in the work dir", n)
			}
		}
		if !got["proc_mtd.txt"] {
			t.Error("the small proc_mtd.txt note is gone")
		}
	})
	t.Run("written but not verified: the dumps of the camera stay, the Wi-Fi config does not", func(t *testing.T) {
		u := newUWorld(t)
		p := u.prepare(t)
		d := u.deps()
		d.PowerCycle = func() error { u.cam.mtd[3] = append([]byte(nil), u.old...); return nil } // not the new image
		if _, err := RunUpdate(p, d); err == nil {
			t.Fatal("verified")
		}
		got := workFiles(t, u.in.WorkDir)
		if got["mtd4.bin"] {
			t.Error("mtd4.bin (Wi-Fi settings) was kept")
		}
		for _, want := range []string{"mtd3.bin", "mtd3.readback.bin"} {
			if !got[want] {
				t.Errorf("%s was deleted although the update failed", want)
			}
		}
	})
}

// U6: the update flow gives ONE power-cycle instruction (the PowerCycle prompt); the flash output does not add a
// second one that tells the operator to run `ssh ... reboot`.
func TestUpdateFlowHasASingleRebootInstruction(t *testing.T) {
	for _, hot := range []bool{true, false} {
		u := newUWorld(t)
		u.in.Hot = hot
		u.cam.sdMount = "/dev/mmcblk0p1 vfat"
		p := u.prepare(t)
		if _, err := RunUpdate(p, u.deps()); err != nil {
			t.Fatalf("hot=%v: %v\n%s", hot, err, u.outText())
		}
		out := u.outText()
		for _, bad := range []string{"Reboot it yourself now", "Reboot ONCE", "ssh root@192.0.2.10 reboot"} {
			if strings.Contains(out, bad) {
				t.Errorf("hot=%v: a second reboot instruction %q:\n%s", hot, bad, out)
			}
		}
		if !strings.Contains(out, "its one instruction is given below") {
			t.Errorf("hot=%v: the flash output does not point to the power-cycle prompt", hot)
		}
	}
}

func TestCleanWorkDirTolerance(t *testing.T) {
	CleanWorkDir("", true)
	CleanWorkDir(filepath.Join(t.TempDir(), "absent"), true)
}
