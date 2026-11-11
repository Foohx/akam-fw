//go:build !linux && !windows && !darwin

package audio

import "os/exec"

func setProcAttr(cmd *exec.Cmd) {}

func afterStart(cmd *exec.Cmd) {}

func terminate(cmd *exec.Cmd, kill bool) {
	if cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}
