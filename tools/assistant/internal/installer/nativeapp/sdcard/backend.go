package sdcard

import (
	"errors"
	"io"
	"os"
	"path/filepath"
)

// Backend is what the commands need from the card layer (tests replace it).
type Backend interface {
	// List returns the eligible cards (no privilege).
	List() []Candidate
	// Check validates a device given by the user (links resolved) and returns it.
	Check(dev string) (*Candidate, error)
	// NeedsElevation reports whether writing c needs administrator rights.
	NeedsElevation(c *Candidate) bool
	// Prepare formats the card and writes the tree, then reads it back and verifies.
	Prepare(c *Candidate, files []File, spec VerifySpec) ([]string, error)
	// Wipe zero-fills the card.
	Wipe(c *Candidate) error
}

// System is the real backend: guards on /sys, writes through the helper (re-executed with sudo or
// pkexec unless the user may already write the device).
type System struct {
	Sys Sys
	// Self is the path of this program (default os.Executable()).
	Self string
	// Elevate: "sudo", "pkexec" or "" (auto: pkexec with a graphical session and polkit, else sudo).
	Elevate string
	// Stderr receives the elevation tool's prompts and the helper's stderr.
	Stderr io.Writer
	// Inv, Opener and Transport override the running OS's (nil: Sys, and the platform's own).
	Inv       Inventory
	Opener    Opener
	Transport Transport
}

// NewSystem is the backend of the real program.
func NewSystem() *System {
	p := newPlatform()
	return &System{Sys: RealSys(), Inv: p.Inventory, Opener: p.Opener, Transport: p.Transport, Elevate: os.Getenv("AKAM_SD_ELEVATE"), Stderr: os.Stderr}
}

func (s *System) List() []Candidate {
	return listDevices(s.inventory(), Context{GuardPaths: s.Sys.GuardPaths})
}

// Check resolves /dev/disk/by-* links first, like the bash tool, then applies the guards.
func (s *System) Check(dev string) (*Candidate, error) {
	if real, err := filepath.EvalSymlinks(dev); err == nil {
		dev = real
	}
	return checkDevice(s.inventory(), dev, Context{GuardPaths: s.Sys.GuardPaths})
}

func (s *System) NeedsElevation(c *Candidate) bool { return s.transport().NeedsElevation(c) }

func (s *System) transport() Transport {
	if s.Transport != nil {
		return s.Transport
	}
	return newPlatform().Transport
}

func (s *System) inventory() Inventory {
	if s.Inv != nil {
		return s.Inv
	}
	return s.Sys
}

// call sends one request: in this process when no rights are needed (or already root), else to
// `<self> sd-helper` under sudo/pkexec, with the request on stdin and a minimal environment (the Wi-Fi
// password is in the request only: never in argv, never in the environment).
func (s *System) call(c *Candidate, req Request) (Response, error) {
	if !s.NeedsElevation(c) {
		return Helper{Sys: s.Sys, Inv: s.Inv, Opener: s.Opener, Progress: s.Stderr}.Handle(req), nil
	}
	req.GuardPaths = s.Sys.GuardPaths
	return s.transport().RunElevated(s.Self, s.Elevate, req, s.Stderr)
}

func (s *System) Prepare(c *Candidate, files []File, spec VerifySpec) ([]string, error) {
	resp, err := s.call(c, Request{Op: OpPrepare, Device: c.Dev, Token: c.Ident, Files: files, Verify: &spec})
	if err != nil {
		return nil, err
	}
	if !resp.OK {
		return resp.Lines, errors.New(resp.Error)
	}
	return resp.Lines, nil
}

func (s *System) Wipe(c *Candidate) error {
	resp, err := s.call(c, Request{Op: OpWipe, Device: c.Dev, Token: c.Ident})
	if err != nil {
		return err
	}
	if !resp.OK {
		return errors.New(resp.Error)
	}
	return nil
}
