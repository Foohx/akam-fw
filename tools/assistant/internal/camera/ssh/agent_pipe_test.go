package ssh

import "testing"

func TestAgentPipeName(t *testing.T) {
	for in, want := range map[string]string{
		``: agentPipe, `/tmp/ssh-x/agent.1`: agentPipe, `C:\x\agent`: agentPipe, `\\.\pipe\`: agentPipe,
		`\\.\pipe\custom`: `\\.\pipe\custom`, `\\.\PIPE\Custom`: `\\.\PIPE\Custom`,
	} {
		if got := agentPipeName(in); got != want {
			t.Errorf("agentPipeName(%q)=%q want %q", in, got, want)
		}
	}
}
