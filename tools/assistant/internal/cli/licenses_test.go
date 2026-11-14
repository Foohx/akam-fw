package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// `akam licenses` prints Akam's own MIT licence (LICENSE, byte for byte) and then the embedded
// THIRD_PARTY_NOTICES.md, byte for byte, with no state and no camera.
func TestLicensesPrintsTheEmbeddedNotices(t *testing.T) {
	want, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "THIRD_PARTY_NOTICES.md"))
	if err != nil {
		t.Fatal(err)
	}
	lic, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "LICENSE"))
	if err != nil {
		t.Fatal(err)
	}
	e := newEnv(t)
	r := e.run("", "licenses")
	expectRC(t, r, 0)
	if r.out != "Akam (MIT)\n\n"+string(lic)+"\n\n"+string(want) {
		t.Errorf("licenses output differs from LICENSE + THIRD_PARTY_NOTICES.md (run: go generate ./internal/assets)")
	}
	if !strings.HasPrefix(r.out, "Akam (MIT)\n") || !strings.Contains(r.out, "FluxAlchemist") {
		t.Error("licenses does not start with Akam's own MIT licence and the upstream copyright line")
	}
	if !strings.Contains(r.out, "Copyright 2009 The Go Authors") || !strings.Contains(r.out, "Permission is hereby granted") {
		t.Error("licenses lacks the Go and MIT licence texts")
	}
	if !strings.Contains(r.out, "golang.org/x/crypto") {
		t.Error("the notices do not list x/crypto")
	}
	if _, err := os.Stat(filepath.Join(e.state, "state")); err == nil {
		t.Error("licenses touched the state")
	}
}
