//go:build linux

package audio

import (
	"os/exec"
	"syscall"
)

// setProcAttr puts the child in its own process group and asks the kernel to kill it when this
// process dies (so a killed assistant never leaves the microphone recording).
func setProcAttr(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
}

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
