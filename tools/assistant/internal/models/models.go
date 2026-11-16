// Package models is the registry of the camera models Akam knows (tools/release/models/<id>.txt, embedded in the
// binary). A model has a stable id (see kit.ValidModel), the fingerprints of its stock firmware and a status per
// fingerprint (validated, reported, untested). The assistant uses it to recognise a camera from the facts it
// already reads (the stock APP sha256 of a backup, the kernel line) and to refuse a kit made for another model.
package models

import (
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// Record is one line of a model file: a known stock firmware fingerprint and its status. Unknown fields are "-".
type Record struct {
	Status  string // validated | reported | untested
	Device  string
	SoC     string
	Kernel  string // `uname -r` and `uname -v`
	MTD     string
	Squash  string
	AppSHA  string // sha256 of the stock APP partition dump
	Busybox string
}

// Firmware kinds of a model (header `kind`). An absent header means KindNativeApp, so that a file written before
// the field existed keeps its meaning.
const (
	KindNativeApp      = "native-app"      // Akam replaces the APP partition (the validated 1080p)
	KindDecloudOverlay = "decloud-overlay" // the stock app stays; Akam only cuts the cloud, from an SD overlay
	// KindCardlessAppfs: Akam runs from the flash, with no SD card. The appfs partition (squashfs) is replaced by an
	// Akam one (stock appfs + the Akam payload; the stock app is removed). The first install and the recovery use an
	// SD card (build_card.sh). The assistant does not drive this flow yet (docs/assistant-rot2k-flash-design.md).
	KindCardlessAppfs = "cardless-appfs"
)

// DefaultID is the camera type of a profile that carries none (every camera saved before the type was asked is a
// 1080p). It is the only model whose flows (backup, flash, restore, options) the assistant implements.
const DefaultID = "lsc-indoor-1080p-ak3918"

// Model is one registry entry.
type Model struct {
	ID       string
	Kind     string // header `kind` as written ("" when absent); use EffectiveKind
	Name     string // display name
	Articles string
	SoC      string
	Flash    string
	Layout   string
	Records  []Record
}

// Parse reads a model file (header lines key=value, then '|' records). id is the file name without .txt; a
// file whose model= header differs, or that has no valid id, is not a model (ok false).
func Parse(id string, b []byte) (Model, bool) {
	m := Model{ID: id}
	hdr := ""
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimRight(l, "\r")
		t := strings.TrimSpace(l)
		if t == "" || strings.HasPrefix(t, "#") {
			continue
		}
		if !strings.Contains(l, "|") {
			k, v, _ := strings.Cut(l, "=")
			if strings.EqualFold(strings.TrimSpace(k), "kind") { // lenient on spaces and case: a variant must never read as absent
				kv := strings.TrimSpace(v)
				if kv == "" {
					kv = "(empty)"
				}
				if m.Kind != "" && m.Kind != kv {
					kv = "(conflicting)"
				}
				m.Kind = kv
				continue
			}
			switch k {
			case "model":
				hdr = v
			case "name":
				m.Name = v
			case "articles":
				m.Articles = v
			case "soc":
				m.SoC = v
			case "flash":
				m.Flash = v
			case "layout":
				m.Layout = v
			}
			continue
		}
		f := strings.Split(l, "|")
		get := func(i int) string {
			if i < len(f) {
				return f[i]
			}
			return "-"
		}
		m.Records = append(m.Records, Record{Status: f[0], Device: get(1), SoC: get(2), Kernel: get(3), MTD: get(4), Squash: get(5), AppSHA: get(6), Busybox: get(7)})
	}
	if !kit.ValidModel(id) || hdr != id {
		return Model{}, false
	}
	if m.Name == "" {
		m.Name = id
	}
	return m, true
}

// EffectiveKind is the firmware kind: the `kind` header, KindNativeApp when it is absent.
func (m Model) EffectiveKind() string {
	if m.Kind == "" {
		return KindNativeApp
	}
	return m.Kind
}

// Validated reports whether an Akam image was flashed and run on real hardware of this model (a validated record).
func (m Model) Validated() bool {
	for _, r := range m.Records {
		if r.Status == "validated" {
			return true
		}
	}
	return false
}

// Unsupported is "" when the assistant may install or update firmware on this model, else why not: only a
// native-app model with a validated record is actionable. Any other kind (including one this build does not know)
// and any model that is only untested or reported is refused, so that a registry entry can never make the
// assistant write flash by itself.
func (m Model) Unsupported() string {
	switch m.EffectiveKind() {
	case KindNativeApp:
	case KindDecloudOverlay:
		return "it keeps its stock app and boots from an SD overlay, which the assistant cannot build or install yet (planned for Phase 2)"
	case KindCardlessAppfs:
		return "it runs Akam from its flash with no SD card, a flow the assistant cannot drive yet (see docs/assistant-rot2k-flash-design.md); the 1080p installer would brick it"
	default:
		return "its firmware kind '" + m.Kind + "' is not known to this version"
	}
	if !m.Validated() {
		return "no Akam image is validated for it yet (status untested)"
	}
	return ""
}

// NativeFlows reports whether the assistant's backup, flash, restore and camera-option flows (written for the
// native-app 1080p) may run on this model. Any other kind is refused by every such flow: running the 1080p logic
// on it could brick the camera.
func (m Model) NativeFlows() bool { return m.EffectiveKind() == KindNativeApp }

// extra holds models added by tests (models_akamtest.go); always empty in a release build.
var extra []Model

// All is every model of the embedded registry, sorted by id.
func All() []Model {
	out := append([]Model(nil), extra...)
	for _, n := range assets.Names() {
		id, ok := strings.CutSuffix(n, ".txt")
		if !ok {
			continue
		}
		b, err := assets.Script(n)
		if err != nil {
			continue
		}
		if m, ok := Parse(id, b); ok {
			out = append(out, m)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// ByID finds a model by its id.
func ByID(id string) (Model, bool) {
	for _, m := range All() {
		if m.ID == id {
			return m, true
		}
	}
	return Model{}, false
}

// Only is the single native-app model of the registry when there is exactly one (ok false otherwise). Models of
// another kind never count: they cannot be the default of an install, so adding one does not change the default.
func Only() (Model, bool) {
	var one []Model
	for _, m := range All() {
		if m.EffectiveKind() == KindNativeApp {
			one = append(one, m)
		}
	}
	if len(one) == 1 {
		return one[0], true
	}
	return Model{}, false
}

// ByAppSHA finds the model and the record whose stock APP sha256 is sha ("" matches nothing).
func ByAppSHA(sha string) (Model, Record, bool) {
	if sha == "" || sha == "-" {
		return Model{}, Record{}, false
	}
	for _, m := range All() {
		for _, r := range m.Records {
			if r.AppSHA == sha {
				return m, r, true
			}
		}
	}
	return Model{}, Record{}, false
}

// ByKernel finds the model of a validated record with exactly this kernel line (`uname -r` + " " + `uname -v`).
func ByKernel(kernel string) (Model, bool) {
	for _, m := range All() {
		for _, r := range m.Records {
			if r.Status == "validated" && r.Kernel == kernel {
				return m, true
			}
		}
	}
	return Model{}, false
}

// Label is "Name (id)" for a known model, the bare id otherwise.
func Label(id string) string {
	if m, ok := ByID(id); ok && m.Name != id {
		return m.Name + " (" + id + ")"
	}
	return id
}
