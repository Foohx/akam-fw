package testhook

import "testing"

func TestEnvFollowsTheBuildTag(t *testing.T) {
	t.Setenv("AKAM_ALLOWED_SIGNERS", "/some/file")
	got := Env("AKAM_ALLOWED_SIGNERS")
	if Compiled && got != "/some/file" {
		t.Fatalf("test build must honour the hook, got %q", got)
	}
	if !Compiled && got != "" {
		t.Fatalf("release build must ignore the hook, got %q", got)
	}
}
