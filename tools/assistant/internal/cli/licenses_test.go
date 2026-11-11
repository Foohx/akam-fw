package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// `akam licenses` prints the embedded THIRD_PARTY_NOTICES.md, byte for byte, with no state and no camera.
func TestLicensesPrintsTheEmbeddedNotices(t *testing.T) {
	want, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "THIRD_PARTY_NOTICES.md"))
	if err != nil {
		t.Fatal(err)
	}
	e := newEnv(t)
	r := e.run("", "licenses")
	expectRC(t, r, 0)
	if r.out != string(want) {
		t.Errorf("licenses output differs from THIRD_PARTY_NOTICES.md (run: go generate ./internal/assets)")
	}
	if !strings.Contains(r.out, "golang.org/x/crypto") {
		t.Error("the notices do not list x/crypto")
	}
	if _, err := os.Stat(filepath.Join(e.state, "state")); err == nil {
		t.Error("licenses touched the state")
	}
}
