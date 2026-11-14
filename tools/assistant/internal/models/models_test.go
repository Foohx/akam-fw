package models_test

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
)

const lsc = "lsc-indoor-1080p-ak3918"
const lscAppSHA = "14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294"

func TestTheEmbeddedRegistryKnowsTheValidatedCamera(t *testing.T) {
	all := models.All()
	if len(all) == 0 {
		t.Fatal("no model embedded")
	}
	m, ok := models.ByID(lsc)
	if !ok || m.Name != "LSC Smart Indoor 1080p" || m.SoC != "AK3918AV130" || m.Articles != "3215672.3" {
		t.Fatalf("%+v %v", m, ok)
	}
	m, r, ok := models.ByAppSHA(lscAppSHA)
	if !ok || m.ID != lsc || r.Status != "validated" || !strings.Contains(r.Device, "3215672.3") {
		t.Fatalf("by app sha: %+v %+v %v", m, r, ok)
	}
	if _, _, ok := models.ByAppSHA(strings.Repeat("0", 64)); ok {
		t.Error("an unknown stock APP must not match")
	}
	for _, empty := range []string{"", "-"} {
		if _, _, ok := models.ByAppSHA(empty); ok {
			t.Errorf("%q must match nothing", empty)
		}
	}
	if k, ok := models.ByKernel("4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025"); !ok || k.ID != lsc {
		t.Errorf("by kernel: %v %v", k, ok)
	}
	if _, ok := models.ByKernel("5.0 #1"); ok {
		t.Error("unknown kernel")
	}
	if models.Label(lsc) != "LSC Smart Indoor 1080p ("+lsc+")" || models.Label("nope-model") != "nope-model" {
		t.Errorf("labels: %q %q", models.Label(lsc), models.Label("nope-model"))
	}
	if o, ok := models.Only(); ok && o.ID != lsc {
		t.Errorf("only: %v", o)
	}
}

func TestEveryEmbeddedModelIsConsistent(t *testing.T) {
	for _, m := range models.All() {
		if len(m.Records) == 0 {
			t.Errorf("%s has no record", m.ID)
		}
		validated := false
		for _, r := range m.Records {
			validated = validated || r.Status == "validated"
			switch r.Status {
			case "validated", "reported", "untested":
			default:
				t.Errorf("%s: status %q", m.ID, r.Status)
			}
		}
		if !validated {
			t.Errorf("%s has no validated record (a kit is only built for a validated model)", m.ID)
		}
	}
}

func TestParse(t *testing.T) {
	good := "# c\nsupported_hardware_format=1\nmodel=a-b-c\nname=Cam\narticles=1 2\nsoc=S\nflash=8 MiB\nlayout=L\nvalidated|Dev|SoC|k|mtd|sq|" + strings.Repeat("a", 64) + "|1.30\nuntested|x\n"
	m, ok := models.Parse("a-b-c", []byte(good))
	if !ok || m.Name != "Cam" || m.Articles != "1 2" || m.Flash != "8 MiB" || len(m.Records) != 2 || m.Records[1].AppSHA != "-" {
		t.Fatalf("%+v %v", m, ok)
	}
	for name, c := range map[string]struct{ id, text string }{
		"header differs":    {"a-b-c", "model=other\nvalidated|x\n"},
		"no header":         {"a-b-c", "validated|x\n"},
		"bad id":            {"A_B", "model=A_B\nvalidated|x\n"},
		"too short":         {"ab", "model=ab\nvalidated|x\n"},
		"double dash in id": {"a--b", "model=a--b\nvalidated|x\n"},
	} {
		if _, ok := models.Parse(c.id, []byte(c.text)); ok {
			t.Errorf("%s must not parse", name)
		}
	}
}

func TestAddForTestsIsRemoved(t *testing.T) {
	remove := models.AddForTests(models.Model{ID: "tmp-model", Records: []models.Record{{Status: "validated", AppSHA: "x1"}}})
	if _, _, ok := models.ByAppSHA("x1"); !ok {
		t.Error("added model not found")
	}
	remove()
	if _, _, ok := models.ByAppSHA("x1"); ok {
		t.Error("added model still there")
	}
}
