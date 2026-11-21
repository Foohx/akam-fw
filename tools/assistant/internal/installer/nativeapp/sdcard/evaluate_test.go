package sdcard

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type factsFixture struct {
	Facts  Facts  `json:"facts"`
	Refuse string `json:"refuse"` // "" = eligible, else a substring of the refusal
}

// Every fixture is one disk with its expected verdict (testdata/facts/{linux,darwin,windows}/*.json).
func TestEvaluateFactsFixtures(t *testing.T) {
	files, _ := filepath.Glob("testdata/facts/*/*.json")
	if len(files) < 20 {
		t.Fatalf("only %d fixtures", len(files))
	}
	for _, fn := range files {
		raw, err := os.ReadFile(fn)
		if err != nil {
			t.Fatal(err)
		}
		var fx factsFixture
		if err := json.Unmarshal(raw, &fx); err != nil {
			t.Fatalf("%s: %v", fn, err)
		}
		c, err := Evaluate(fx.Facts, DefaultPolicy)
		switch {
		case fx.Refuse == "" && err != nil:
			t.Errorf("%s: refused: %v", fn, err)
		case fx.Refuse != "" && (err == nil || !strings.Contains(err.Error(), fx.Refuse)):
			t.Errorf("%s: want refusal %q, got %v", fn, fx.Refuse, err)
		case err == nil && (c.Ident == "" || c.Dev != fx.Facts.Path):
			t.Errorf("%s: candidate %+v", fn, c)
		}
	}
}

func TestFactsRoundTripJSON(t *testing.T) {
	in := Facts{OS: "linux", Path: "/dev/sdb", Whole: true, Removable: Yes, Internal: No, Mounts: []Mount{{Node: "/dev/sdb1", Point: "/mnt"}}}
	b, err := json.Marshal(in)
	if err != nil {
		t.Fatal(err)
	}
	var out Facts
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatal(err)
	}
	if out.Removable != Yes || out.Internal != No || out.Mounts[0].Point != "/mnt" || !strings.Contains(string(b), `"removable":"yes"`) {
		t.Errorf("%s -> %+v", b, out)
	}
}

// The Linux identity token has not changed by a single byte: size|model|serial|wwn|vendor|major:minor.
func TestLinuxIdentIsTheHistoricalToken(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdb", 32*gib, "1", "usb1", "SD Reader  ", "8:16")
	f.put("sys/devices/pci0/usb1/sdb/device/wwid", "naa.5000")
	c, err := f.Sys.Check(f.dev("sdb"))
	if err != nil {
		t.Fatal(err)
	}
	want := "34359738368|SD Reader|S-sdb|naa.5000|Generic|8:16"
	if c.Ident != want {
		t.Errorf("ident %q, want %q", c.Ident, want)
	}
}

// The facts read from the fake /sys, evaluated, give the same verdict as the guards always did (the table
// of TestGuardMatrix, expressed on Facts).
func TestSysInspectThenEvaluate(t *testing.T) {
	f := newFakeSys(t)
	f.disk("sdc", 32*gib, "0", "usb1", "Hard disk", "8:32")
	f.disk("sdg", 16*gib, "1", "usb4", "Sys", "8:96")
	f.part("sdg", "sdg1", "8:97")
	f.mounts("20 1 8:97 / /home rw - ext4 /dev/sdg1 rw")
	facts, err := f.Sys.Inspect(f.dev("sdc"), Context{})
	if err != nil || facts.Removable != No || facts.Bus != "usb" || facts.OS != "linux" {
		t.Fatalf("%+v %v", facts, err)
	}
	if _, err := Evaluate(facts, DefaultPolicy); err == nil || !strings.Contains(err.Error(), "not removable") {
		t.Errorf("got %v", err)
	}
	facts, _ = f.Sys.Inspect(f.dev("sdg"), Context{})
	if len(facts.Mounts) != 1 || facts.Mounts[0].Point != "/home" {
		t.Errorf("mounts %+v", facts.Mounts)
	}
}
