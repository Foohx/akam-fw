package models_test

import (
	"reflect"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
)

const rot = "lsc-rotatable-2k-ak3918av130"

// Golden: the 1080p entry is read exactly as before the `kind` field existed (the file is byte-identical to dev and
// carries no kind, so it relies on the default). Adding a second model must not change the default model either.
func TestThe1080pEntryIsReadExactlyAsBefore(t *testing.T) {
	m, ok := models.ByID(lsc)
	if !ok {
		t.Fatal("1080p model missing")
	}
	want := models.Model{
		ID: lsc, Name: "LSC Smart Indoor 1080p", Articles: "3215672.3", SoC: "AK3918AV130", Flash: "8 MiB SPI NOR",
		Layout: "8 partitions; sizes 204800 4096 4096 65536 1572864 1032192 262144 5242880; APP=mtd7",
		Records: []models.Record{
			{Status: "validated", Device: "LSC Smart Indoor 1080p 3215672.3 (board SI D26228)", SoC: "AK3918AV130", Kernel: "4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025", MTD: "8 partitions; mtd7=APP 00500000", Squash: "squashfs xz block=131072 bcj=none", AppSHA: lscAppSHA, Busybox: "1.30.1"},
			{Status: "untested", Device: "LSC Smart Indoor 1080p 3215672.2 (boards SI B26101 / C26101; run with akamd upstream, never with Akam)", SoC: "AK3918AV130", Kernel: "-", MTD: "-", Squash: "-", AppSHA: "-", Busybox: "-"},
			{Status: "untested", Device: "any other device, kernel build or stock APP (including a stock APP compressed with gzip)", SoC: "-", Kernel: "-", MTD: "-", Squash: "-", AppSHA: "-", Busybox: "-"},
		},
	}
	if !reflect.DeepEqual(m, want) {
		t.Errorf("1080p entry changed:\n got %+v\nwant %+v", m, want)
	}
	if m.EffectiveKind() != models.KindNativeApp || m.Unsupported() != "" {
		t.Errorf("1080p kind %q, unsupported %q", m.EffectiveKind(), m.Unsupported())
	}
	if o, ok := models.Only(); !ok || o.ID != lsc {
		t.Errorf("Only() = %v %v", o.ID, ok)
	}
	if k, ok := models.ByKernel("4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025"); !ok || k.ID != lsc {
		t.Errorf("by kernel: %v", k.ID)
	}
	if m, _, ok := models.ByAppSHA(lscAppSHA); !ok || m.ID != lsc {
		t.Errorf("by sha: %v", m.ID)
	}
}

// The validated 2K unit (AK3918AV130, 6 partitions, mtd3 = appfs 0x490000, squashfs xz 128K): it runs Akam card-less
// from flash, a flow the assistant does not drive yet, so it is validated but not actionable.
func TestTheRotatable2KIsCardlessAppfsAndNotActionable(t *testing.T) {
	m, ok := models.ByID(rot)
	if !ok {
		t.Fatal("rotatable model missing")
	}
	if m.Kind != models.KindCardlessAppfs || m.Articles != "3225722" || m.SoC != "AK3918AV130" || m.Flash != "8 MiB SPI NOR" ||
		!strings.HasPrefix(m.Layout, "6 partitions;") || !m.Validated() || m.NativeFlows() {
		t.Fatalf("%+v", m)
	}
	r := m.Records[0]
	if r.Status != "validated" || r.SoC != "AK3918AV130" || r.MTD != "6 partitions; mtd3=appfs 00490000" ||
		r.Squash != "squashfs xz block=131072" || r.AppSHA != "74575a6fe3707cd5a077bc7fd419cd394ede95f57d7b622b444f0636e691c0ef" {
		t.Errorf("record: %+v", r)
	}
	if u := m.Unsupported(); !strings.Contains(u, "no SD card") || !strings.Contains(u, "lsc-rotatable-2k/README.md#install") {
		t.Errorf("unsupported: %q", u)
	}
	if _, _, ok := models.ByAppSHA(r.AppSHA); !ok {
		t.Error("its stock APP must be recognised")
	}
	if k, ok := models.ByKernel(r.Kernel); !ok || k.ID != rot {
		t.Error("its validated kernel identifies the model")
	}
	if o, ok := models.Only(); !ok || o.ID != lsc {
		t.Errorf("the 2K must not change the default model: %v", o.ID)
	}
	if models.DefaultID != lsc {
		t.Errorf("default %q", models.DefaultID)
	}
}

// A malformed `kind` header fails closed: it is never read as an absent kind (which means native-app).
func TestMalformedKindFailsClosed(t *testing.T) {
	for name, line := range map[string]string{
		"spaces around =": "kind = decloud-overlay\n",
		"upper case key":  "Kind=decloud-overlay\n",
		"empty value":     "kind=\n",
		"crlf":            "kind=future\r\n",
		"conflicting":     "kind=native-app\nkind=decloud-overlay\n",
	} {
		m, ok := models.Parse("a-b-c", []byte("model=a-b-c\n"+line+"validated|x\n"))
		if !ok || m.Kind == "" || m.EffectiveKind() == models.KindNativeApp || m.Unsupported() == "" {
			t.Errorf("%s: %+v %v unsupported %q", name, m, ok, m.Unsupported())
		}
	}
	m, _ := models.Parse("a-b-c", []byte("model=a-b-c\nkind = native-app\nvalidated|x\n"))
	if m.Unsupported() != "" {
		t.Errorf("a spaced native-app is still native-app: %q", m.Unsupported())
	}
}

func TestKindParsingAndActionability(t *testing.T) {
	hdr := "model=a-b-c\n"
	for name, c := range map[string]struct{ text, kind, unsupported string }{
		"absent is native-app":    {hdr + "validated|x\n", "native-app", ""},
		"explicit native-app":     {hdr + "kind=native-app\nvalidated|x\n", "native-app", ""},
		"decloud-overlay":         {hdr + "kind=decloud-overlay\nvalidated|x\n", "decloud-overlay", "Phase 2"},
		"cardless-appfs":          {hdr + "kind=cardless-appfs\nvalidated|x\n", "cardless-appfs", "no SD card"},
		"unknown kind":            {hdr + "kind=future-kind\nvalidated|x\n", "future-kind", "not known"},
		"native-app untested":     {hdr + "untested|x\n", "native-app", "untested"},
		"native-app only reports": {hdr + "reported|x\n", "native-app", "untested"},
	} {
		m, ok := models.Parse("a-b-c", []byte(c.text))
		if !ok || m.EffectiveKind() != c.kind {
			t.Errorf("%s: %+v %v", name, m, ok)
			continue
		}
		got := m.Unsupported()
		if (c.unsupported == "") != (got == "") || !strings.Contains(got, c.unsupported) {
			t.Errorf("%s: unsupported %q, want %q", name, got, c.unsupported)
		}
	}
}
