package sdcard

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
)

// The privileged helper (`akam sd-helper`): a tiny, closed set of operations on one card. It takes ONE
// JSON request on stdin (never argv, so the Wi-Fi password is never on a command line), re-validates
// the device with the same guards as the unprivileged side (whole removable USB/SD disk of 128 MiB to
// 128 GiB, not a system disk, not part of a storage stack) and compares the identity token (size,
// model, serial, wwn, vendor, major:minor) given by its caller, opens the device O_RDWR|O_EXCL after
// unmounting it, re-checks again right before the first write, does ONE operation, answers one JSON
// document on stdout and exits. No network, no downloads, no files read from disk: the content to
// write travels in the request, so a caller cannot make it read a root-only file.

// Operations.
const (
	OpList    = "list"
	OpPrepare = "prepare"
	OpVerify  = "verify"
	OpWipe    = "wipe"
)

// MaxRequest bounds the request (a flash card carries about 20 MB).
const MaxRequest = 96 << 20

// Request is what the helper accepts.
type Request struct {
	Op     string      `json:"op"`
	Device string      `json:"device,omitempty"`
	Token  string      `json:"token,omitempty"` // Candidate.Ident of the confirmed disk
	Files  []File      `json:"files,omitempty"` // prepare: the whole tree
	Verify *VerifySpec `json:"verify,omitempty"`
	// GuardPaths are the caller's home, working and temp directories: under sudo or pkexec the helper's
	// own environment names root's, so the caller's are passed along and must not sit on the card.
	GuardPaths []string `json:"guard_paths,omitempty"`
	// ImageFile: Device is a regular file (tests only; refused unless built with -tags akamtest).
	ImageFile bool `json:"image_file,omitempty"`
}

// Response is the answer.
type Response struct {
	OK         bool        `json:"ok"`
	Error      string      `json:"error,omitempty"`
	Lines      []string    `json:"lines,omitempty"`
	Candidates []Candidate `json:"candidates,omitempty"`
}

// Helper is the logic of the helper, with its system view injectable.
type Helper struct {
	Sys Sys
	// Inv overrides the inventory (nil: Sys, the Linux one). Opener overrides the OS side of the privileged
	// steps (nil: the running OS's).
	Inv    Inventory
	Opener Opener
	// Unmount unmounts a mount point (nil: the Opener's).
	Unmount func(point string) error
	// Rescan asks the kernel to re-read the partition table (nil: the disk's own, BLKRRPART on Linux).
	Rescan func(f *os.File)
	// Progress receives the progress lines of a wipe (nil: silent).
	Progress io.Writer
}

func fail(format string, a ...any) Response { return Response{Error: fmt.Sprintf(format, a...)} }

// ImageToken is the identity token of a test image file.
func ImageToken(size int64) string { return fmt.Sprintf("image|%d", size) }

type target struct {
	d    Disk
	size int64
}

func (h Helper) inventory() Inventory {
	if h.Inv != nil {
		return h.Inv
	}
	return h.Sys
}

func (h Helper) opener() Opener {
	if h.Opener != nil {
		return h.Opener
	}
	return newPlatform().Opener
}

// open validates the device and opens it.
func (h Helper) open(req Request) (*target, error) {
	if req.ImageFile {
		if !imageFileAllowed {
			return nil, errors.New("image-file mode is not available in this build")
		}
		st, err := os.Stat(req.Device)
		if err != nil || !st.Mode().IsRegular() {
			return nil, errors.New("image-file mode needs a regular file")
		}
		if req.Token != ImageToken(st.Size()) {
			return nil, ErrIdentChanged
		}
		f, err := os.OpenFile(req.Device, os.O_RDWR, 0)
		if err != nil {
			return nil, err
		}
		return &target{fileDisk{f: f, rescan: h.Rescan}, st.Size()}, nil
	}
	if req.Token == "" {
		return nil, errors.New("no identity token: refusing")
	}
	ctx := Context{GuardPaths: h.Sys.GuardPaths}
	inv, op := h.inventory(), h.opener()
	c, err := recheckDevice(inv, req.Device, req.Token, ctx)
	if err != nil {
		return nil, fmt.Errorf("refusing: %w", err)
	}
	if err := op.CheckNode(req.Device); err != nil {
		return nil, err
	}
	for _, m := range c.Mounts {
		un := h.Unmount
		if un == nil {
			un = op.Unmount
		}
		if err := un(m.Point); err != nil {
			return nil, fmt.Errorf("cannot unmount %s: %v", m.Point, err)
		}
	}
	// the card was unmounted: it must still be the same one, right before the first write
	if _, err := recheckDevice(inv, req.Device, req.Token, ctx); err != nil {
		return nil, fmt.Errorf("refusing: %w (nothing was written)", err)
	}
	d, err := op.Open(req.Device, c)
	if err != nil {
		return nil, err
	}
	if h.Rescan != nil {
		if fd, ok := d.(fileDisk); ok {
			fd.rescan = h.Rescan
			d = fd
		}
	}
	return &target{d, c.Size}, nil
}

// Handle runs one request.
func (h Helper) Handle(req Request) Response {
	switch req.Op {
	case OpList:
		return Response{OK: true, Candidates: listDevices(h.inventory(), Context{GuardPaths: h.Sys.GuardPaths})}
	case OpPrepare, OpVerify, OpWipe:
	default:
		return fail("unknown operation %q", req.Op)
	}
	if req.Device == "" {
		return fail("no device")
	}
	h.Sys.GuardPaths = append(append([]string{}, h.Sys.GuardPaths...), req.GuardPaths...)
	t, err := h.open(req)
	if err != nil {
		return fail("%v", err)
	}
	defer t.d.Close()
	switch req.Op {
	case OpPrepare:
		return h.prepare(t, req)
	case OpVerify:
		return h.verify(t, req)
	}
	return h.wipe(t)
}

func (h Helper) say(format string, a ...any) {
	if h.Progress != nil {
		fmt.Fprintf(h.Progress, format+"\n", a...)
	}
}

func (h Helper) prepare(t *target, req Request) Response {
	if req.Verify == nil || len(req.Files) == 0 {
		return fail("prepare needs the files and the verification spec")
	}
	off, psize, err := FormatMBR(t.d, t.size)
	if err != nil {
		return fail("%v", err)
	}
	if err := WriteFAT32(t.d, off, psize, req.Files, WriteOptions{}); err != nil {
		return fail("writing the card failed: %v", err)
	}
	t.d.Rescan()
	card, err := OpenCard(t.d)
	if err != nil {
		return fail("the card cannot be read back: %v", err)
	}
	lines, bad := VerifyTree(card, *req.Verify)
	// every byte that was sent must be on the card
	for _, f := range req.Files {
		if f.Dir {
			continue
		}
		got, err := card.ReadFile(f.Path)
		if err != nil || !bytes.Equal(got, f.Data) {
			lines = append(lines, "FAIL: read-back differs: "+f.Path)
			bad++
		}
	}
	if bad > 0 {
		return Response{Error: fmt.Sprintf("the card does not verify after the write (%d problem(s))", bad), Lines: lines}
	}
	return Response{OK: true, Lines: lines}
}

func (h Helper) verify(t *target, req Request) Response {
	if req.Verify == nil {
		return fail("verify needs the verification spec")
	}
	card, err := OpenCard(t.d)
	if err != nil {
		return fail("the card cannot be read: %v", err)
	}
	lines, bad := VerifyTree(card, *req.Verify)
	if bad > 0 {
		return Response{Error: fmt.Sprintf("the card does not verify (%d problem(s))", bad), Lines: lines}
	}
	return Response{OK: true, Lines: lines}
}

// wipe zero-fills exactly the device size (4 MiB blocks, fsync), then spot-checks both ends.
func (h Helper) wipe(t *target) Response {
	const blk = 4 << 20
	zero := make([]byte, blk)
	last := -1
	for off := int64(0); off < t.size; off += blk {
		// progress every 5 %, and a sync every 256 MiB so that the figure is what reached the card
		if pct := int(off * 100 / t.size); pct/5 != last {
			last = pct / 5
			h.say("wipe: %d%%", pct)
		}
		if off > 0 && off%(256<<20) == 0 {
			if err := t.d.Sync(); err != nil {
				return fail("sync failed at %d: %v", off, err)
			}
		}
		n := int64(blk)
		if t.size-off < n {
			n = t.size - off
		}
		if _, err := t.d.WriteAt(zero[:n], off); err != nil {
			return fail("write failed at %d: %v", off, err)
		}
	}
	if err := t.d.Sync(); err != nil {
		return fail("sync failed: %v", err)
	}
	t.d.Rescan()
	chk := make([]byte, blk)
	for _, off := range []int64{0, max64(0, t.size-blk)} {
		n := int64(blk)
		if t.size-off < n {
			n = t.size - off
		}
		if _, err := t.d.ReadAt(chk[:n], off); err != nil {
			return fail("read-back failed: %v", err)
		}
		if !bytes.Equal(chk[:n], zero[:n]) {
			return fail("the card is not zero at offset %d after the wipe", off)
		}
	}
	return Response{OK: true, Lines: []string{fmt.Sprintf("zero-filled %d bytes", t.size)}}
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

// HelperEntry is the entry of the privileged mode: it handles `sd-helper` (the only argument today; per-OS
// flags will be parsed here) and reports whether args named it. Nothing else runs in this mode: no state
// directory, no lock, no UI.
func HelperEntry(args []string, in io.Reader, out, progress io.Writer) (code int, handled bool) {
	if len(args) != 1 || args[0] != "sd-helper" {
		return 0, false
	}
	p := newPlatform()
	h := Helper{Inv: p.Inventory, Opener: p.Opener, Progress: progress}
	if s, ok := p.Inventory.(Sys); ok {
		h.Sys = s
	}
	return HelperMain(in, out, h), true
}

// HelperMain is `akam sd-helper`: one request on in, one answer on out. It returns the exit status.
func HelperMain(in io.Reader, out io.Writer, h Helper) int {
	var req Request
	lr := &io.LimitedReader{R: in, N: MaxRequest + 1}
	dec := json.NewDecoder(lr)
	dec.DisallowUnknownFields()
	// exactly one document: an unknown field, a second document or anything after it is refused
	if err := dec.Decode(&req); err != nil || lr.N <= 0 || dec.More() {
		_ = json.NewEncoder(out).Encode(fail("bad request"))
		return 2
	}
	resp := h.Handle(req)
	_ = json.NewEncoder(out).Encode(resp)
	if !resp.OK {
		return 1
	}
	return 0
}
