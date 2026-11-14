//go:build darwin

package audio

import (
	"os/exec"
	"syscall"
)

// setProcAttr puts the child in its own process group. macOS has no Pdeathsig: the group is killed
// on every exit path of the program (Close), and a program that is itself killed by SIGKILL can leave
// the recorder running until it notices its closed pipe (documented limit).
func setProcAttr(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
}

func afterStart(cmd *exec.Cmd) {}

func terminate(cmd *exec.Cmd, kill bool) {
	if cmd.Process == nil {
		return
	}
	sig := syscall.SIGTERM
	if kill {
		sig = syscall.SIGKILL
	}
	_ = syscall.Kill(-cmd.Process.Pid, sig)
	_ = cmd.Process.Signal(sig)
}
