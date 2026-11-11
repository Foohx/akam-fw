package sdcard

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
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
}

// NewSystem is the backend of the real program.
func NewSystem() *System {
	return &System{Sys: RealSys(), Elevate: os.Getenv("AKAM_SD_ELEVATE"), Stderr: os.Stderr}
}

func (s *System) List() []Candidate { return s.Sys.List() }

// Check resolves /dev/disk/by-* links first, like the bash tool, then applies the guards.
func (s *System) Check(dev string) (*Candidate, error) {
	if real, err := filepath.EvalSymlinks(dev); err == nil {
		dev = real
	}
	return s.Sys.Check(dev)
}

func (s *System) NeedsElevation(c *Candidate) bool {
	if os.Geteuid() == 0 {
		return false
	}
	f, err := os.OpenFile(c.Dev, os.O_RDWR, 0)
	if err != nil {
		return true
	}
	f.Close()
	return false
}

func (s *System) elevator() ([]string, error) {
	choice := s.Elevate
	if choice == "" {
		if _, err := exec.LookPath("pkexec"); err == nil && (os.Getenv("DISPLAY") != "" || os.Getenv("WAYLAND_DISPLAY") != "") {
			choice = "pkexec"
		} else {
			choice = "sudo"
		}
	}
	p, err := exec.LookPath(choice)
	if err != nil {
		return nil, fmt.Errorf("%s is not installed: cannot get administrator rights", choice)
	}
	if choice == "sudo" {
		return []string{p, "--"}, nil
	}
	if choice != "pkexec" {
		return nil, fmt.Errorf("AKAM_SD_ELEVATE must be sudo or pkexec, not %q", choice)
	}
	return []string{p}, nil
}

// call sends one request: in this process when no rights are needed (or already root), else to
// `<self> sd-helper` under sudo/pkexec, with the request on stdin and a minimal environment (the Wi-Fi
// password is in the request only: never in argv, never in the environment).
func (s *System) call(c *Candidate, req Request) (Response, error) {
	if !s.NeedsElevation(c) {
		return Helper{Sys: s.Sys}.Handle(req), nil
	}
	pre, err := s.elevator()
	if err != nil {
		return Response{}, err
	}
	self := s.Self
	if self == "" {
		if self, err = os.Executable(); err != nil {
			return Response{}, err
		}
	}
	body, err := json.Marshal(req)
	if err != nil {
		return Response{}, err
	}
	args := append(append([]string{}, pre[1:]...), self, "sd-helper")
	cmd := exec.Command(pre[0], args...)
	cmd.Stdin = bytes.NewReader(body)
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = s.Stderr
	cmd.Env = minimalEnv()
	if err := cmd.Run(); err != nil && out.Len() == 0 {
		return Response{}, fmt.Errorf("the administrator step did not run: %v", err)
	}
	var resp Response
	if err := json.NewDecoder(&out).Decode(&resp); err != nil {
		return Response{}, errors.New("the helper gave no usable answer")
	}
	return resp, nil
}

func minimalEnv() []string {
	var env []string
	for _, k := range []string{"PATH", "HOME", "TERM", "LANG", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"} {
		if v, ok := os.LookupEnv(k); ok {
			env = append(env, k+"="+v)
		}
	}
	return env
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
