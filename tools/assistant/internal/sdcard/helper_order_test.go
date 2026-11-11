package sdcard

import (
	"fmt"
	"io"
	"strings"
	"testing"
)

// recorder logs every call of the fake inventory, opener and disk in one list, so the ORDER of the helper's
// steps is asserted: re-inspect, compare identity, unmount, re-check, open, write, flush, rescan, read, release.
type recorder struct {
	log   []string
	facts Facts
	size  int64
}

func (r *recorder) add(s string, a ...any) { r.log = append(r.log, fmt.Sprintf(s, a...)) }

func (r *recorder) Devices() []string { return nil }
func (r *recorder) Inspect(dev string, ctx Context) (Facts, error) {
	r.add("inspect")
	return r.facts, nil
}
func (r *recorder) CheckNode(dev string) error { r.add("checknode"); return nil }
func (r *recorder) Unmount(p string) error     { r.add("unmount %s", p); return nil }
func (r *recorder) Open(dev string, c *Candidate) (Disk, error) {
	r.add("open %s", c.Ident)
	return &recDisk{r: r}, nil
}

type recDisk struct {
	r      *recorder
	wrote  bool
	readOK bool
}

func (d *recDisk) ReadAt(p []byte, off int64) (int, error) {
	if !d.readOK {
		d.r.add("read")
		d.readOK = true
	}
	for i := range p {
		p[i] = 0
	}
	return len(p), nil
}
func (d *recDisk) WriteAt(p []byte, off int64) (int, error) {
	if !d.wrote {
		d.r.add("write")
		d.wrote = true
	}
	return len(p), nil
}
func (d *recDisk) Sync() error  { d.r.add("sync"); return nil }
func (d *recDisk) Rescan()      { d.r.add("rescan") }
func (d *recDisk) Close() error { d.r.add("close"); return nil }

func TestHelperOrderOfOperations(t *testing.T) {
	const size = 128 << 20
	facts := Facts{OS: "linux", Path: "/dev/sdz", Name: "sdz", Whole: true, Size: size, Bus: "usb", RemovableRaw: "1",
		Model: "Card", DevNum: "8:48", Mounts: []Mount{{Node: "/dev/sdz1", Point: "/mnt/a"}, {Node: "/dev/sdz2", Point: "/mnt/b"}}}
	r := &recorder{facts: facts}
	tok := Ident(facts)
	resp := Helper{Inv: r, Opener: r, Progress: io.Discard}.Handle(Request{Op: OpWipe, Device: "/dev/sdz", Token: tok})
	if !resp.OK {
		t.Fatal(resp.Error)
	}
	got := strings.Join(r.log, ",")
	want := "inspect,checknode,unmount /mnt/a,unmount /mnt/b,inspect,open " + tok + ",write,sync,rescan,read,close"
	// the wipe syncs once per 256 MiB and at the end: with 128 MiB only the final sync, then rescan
	if got != want {
		t.Errorf("order:\n got %s\nwant %s", got, want)
	}
}

// A card that changed between the two inspections is never opened.
func TestHelperRefusesWhenTheIdentityChangesAfterUnmount(t *testing.T) {
	facts := Facts{OS: "linux", Path: "/dev/sdz", Name: "sdz", Whole: true, Size: 128 << 20, Bus: "usb", RemovableRaw: "1", Model: "A", DevNum: "8:48"}
	r := &swapRec{recorder: recorder{facts: facts}, other: facts}
	r.other.Model = "B"
	resp := Helper{Inv: r, Opener: r}.Handle(Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts)})
	if resp.OK || !strings.Contains(resp.Error, "nothing was written") {
		t.Fatalf("got %+v", resp)
	}
	for _, s := range r.log {
		if strings.HasPrefix(s, "open") || s == "write" {
			t.Errorf("reached %s", s)
		}
	}
}

// swapRec answers the second inspection with another card.
type swapRec struct {
	recorder
	other Facts
	n     int
}

func (s *swapRec) Inspect(dev string, ctx Context) (Facts, error) {
	s.n++
	s.add("inspect")
	if s.n >= 2 {
		return s.other, nil
	}
	return s.facts, nil
}
