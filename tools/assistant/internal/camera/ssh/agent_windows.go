//go:build windows

package ssh

import (
	"io"
	"os"
)

// agentPipe is the named pipe of the OpenSSH for Windows agent service.
const agentPipe = `\\.\pipe\openssh-ssh-agent`

// pipeConn wraps the opened pipe as the io.ReadWriteCloser that agent.NewClient accepts.
type pipeConn struct{ f *os.File }

func (p pipeConn) Read(b []byte) (int, error)  { return p.f.Read(b) }
func (p pipeConn) Write(b []byte) (int, error) { return p.f.Write(b) }
func (p pipeConn) Close() error                { return p.f.Close() }

// dialAgent opens the agent pipe: SSH_AUTH_SOCK when it names a pipe, else the OpenSSH default.
func dialAgent(sock string) (io.ReadWriteCloser, error) {
	name := agentPipe
	if len(sock) > 9 && sock[:9] == `\\.\pipe\` {
		name = sock
	}
	f, err := os.OpenFile(name, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	return pipeConn{f}, nil
}
