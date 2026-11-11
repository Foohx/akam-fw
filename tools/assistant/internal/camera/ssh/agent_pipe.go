package ssh

import "strings"

// agentPipe is the named pipe of the OpenSSH for Windows agent service.
const agentPipe = `\\.\pipe\openssh-ssh-agent`

// agentPipeName is the pipe to dial: SSH_AUTH_SOCK when it names a pipe (the prefix is
// case-insensitive on Windows), else the OpenSSH default. Never a file path.
func agentPipeName(sock string) string {
	const prefix = `\\.\pipe\`
	if len(sock) > len(prefix) && strings.EqualFold(sock[:len(prefix)], prefix) {
		return sock
	}
	return agentPipe
}
