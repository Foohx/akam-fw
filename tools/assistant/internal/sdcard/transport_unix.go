//go:build unix

package sdcard

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
)

// unixTransport re-executes this program under pkexec (graphical session) or sudo, request on stdin, minimal
// environment.
type unixTransport struct{}

func newTransport() Transport { return unixTransport{} }

func (unixTransport) NeedsElevation(c *Candidate) bool {
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

func elevator(choice string) ([]string, error) {
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

func (unixTransport) RunElevated(self, elevate string, req Request, stderr io.Writer) (Response, error) {
	pre, err := elevator(elevate)
	if err != nil {
		return Response{}, err
	}
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
	cmd.Stderr = stderr
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
