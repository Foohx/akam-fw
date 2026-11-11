//go:build !windows

package ssh

import (
	"errors"
	"io"
	"net"
)

// dialAgent connects to the ssh-agent socket named by SSH_AUTH_SOCK (empty: no agent).
func dialAgent(sock string) (io.ReadWriteCloser, error) {
	if sock == "" {
		return nil, errors.New("no ssh-agent")
	}
	return net.Dial("unix", sock)
}
