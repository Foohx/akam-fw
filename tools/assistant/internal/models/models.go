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

// Model is one registry entry.
type Model struct {
	ID       string
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

// Only is the single model of the registry when there is exactly one (ok false otherwise).
func Only() (Model, bool) {
	all := All()
	if len(all) == 1 {
		return all[0], true
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
