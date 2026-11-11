package assets

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The embedded copies must equal the repository files (run `go generate ./internal/assets` to refresh).
func TestEmbeddedScriptsMatchTheRepo(t *testing.T) {
	src, err := os.ReadFile("sources.txt")
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, l := range strings.Fields(string(src)) {
		repo, err := os.ReadFile(filepath.Join("..", "..", "..", "..", l))
		if err != nil {
			t.Fatalf("source %s: %v", l, err)
		}
		got, err := Script(filepath.Base(l))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, repo) {
			t.Errorf("%s drifted from %s: run go generate ./internal/assets", filepath.Base(l), l)
		}
		n++
	}
	if n != len(Names()) {
		t.Errorf("%d sources but %d embedded files", n, len(Names()))
	}
}
