// Package childenv builds the environment of the child processes the assistant starts (ffmpeg,
// pactl, sh, ...): the current one, without the secret variables (AKAM_*PASS*).
package childenv

import (
	"os"
	"strings"
)

// IsSecret reports whether the environment variable name carries a secret (AKAM_RTSP_PASSWORD,
// AKAM_WIFI_PASS, AKAM_SD_PASS, ...).
func IsSecret(name string) bool {
	u := strings.ToUpper(name)
	return strings.HasPrefix(u, "AKAM_") && strings.Contains(u, "PASS")
}

// Filter returns env without the secret variables.
func Filter(env []string) []string {
	out := make([]string, 0, len(env))
	for _, kv := range env {
		name, _, _ := strings.Cut(kv, "=")
		if !IsSecret(name) {
			out = append(out, kv)
		}
	}
	return out
}

// Env is the current environment without the secret variables, for exec.Cmd.Env.
func Env() []string { return Filter(os.Environ()) }

// Take reads the secret variables of the current environment, removes them from it, and returns
// them by name. Called once at start-up so that nothing started later can inherit them.
func Take() map[string]string {
	got := map[string]string{}
	for _, kv := range os.Environ() {
		name, val, _ := strings.Cut(kv, "=")
		if IsSecret(name) {
			got[name] = val
			os.Unsetenv(name)
		}
	}
	return got
}
