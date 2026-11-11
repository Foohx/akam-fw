//go:build !linux

package audio

import "os/exec"

func setProcAttr(cmd *exec.Cmd) {}

func terminate(cmd *exec.Cmd, kill bool) {
	if cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}
