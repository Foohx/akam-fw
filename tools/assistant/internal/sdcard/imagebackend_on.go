//go:build akamtest

package sdcard

import (
	"errors"
	"os"
)

// ImageBackend is a test backend that treats a regular file as the card (helper image-file mode).
// It exists only in builds made with -tags akamtest.
type ImageBackend struct {
	Path  string
	Model string
	// Refuse makes Check refuse the device with this reason (a system disk, a wrong size...).
	Refuse string
	// Elevate makes NeedsElevation true (the "administrator rights" question is asked).
	Elevate bool
	// Calls counts destructive operations.
	Prepared, Wiped int
}

func (b *ImageBackend) cand() (*Candidate, error) {
	st, err := os.Stat(b.Path)
	if err != nil {
		return nil, err
	}
	m := b.Model
	if m == "" {
		m = "Test Card"
	}
	return &Candidate{Dev: b.Path, Name: "image", Size: st.Size(), Model: m, Tran: "usb", Ident: ImageToken(st.Size())}, nil
}

func (b *ImageBackend) List() []Candidate {
	if c, err := b.cand(); err == nil && b.Refuse == "" {
		return []Candidate{*c}
	}
	return nil
}

func (b *ImageBackend) Check(dev string) (*Candidate, error) {
	if b.Refuse != "" {
		return nil, errors.New(b.Refuse)
	}
	if dev != b.Path {
		return nil, errors.New(dev + " is not a whole USB/SD disk node like /dev/sdb or /dev/mmcblk0")
	}
	return b.cand()
}

func (b *ImageBackend) NeedsElevation(*Candidate) bool { return b.Elevate }

func (b *ImageBackend) Prepare(c *Candidate, files []File, spec VerifySpec) ([]string, error) {
	b.Prepared++
	r := Helper{}.Handle(Request{Op: OpPrepare, Device: c.Dev, Token: c.Ident, ImageFile: true, Files: files, Verify: &spec})
	if !r.OK {
		return r.Lines, errors.New(r.Error)
	}
	return r.Lines, nil
}

func (b *ImageBackend) Wipe(c *Candidate) error {
	b.Wiped++
	r := Helper{}.Handle(Request{Op: OpWipe, Device: c.Dev, Token: c.Ident, ImageFile: true})
	if !r.OK {
		return errors.New(r.Error)
	}
	return nil
}
