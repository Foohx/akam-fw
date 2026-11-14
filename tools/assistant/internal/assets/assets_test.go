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

// Every model of the registry (tools/release/models/<id>.txt) is embedded: the assistant recognises cameras by it.
func TestEveryRegistryModelIsEmbedded(t *testing.T) {
	files, err := filepath.Glob(filepath.Join("..", "..", "..", "release", "models", "*.txt"))
	if err != nil || len(files) == 0 {
		t.Fatalf("no model file found: %v", err)
	}
	for _, f := range files {
		if _, err := Script(filepath.Base(f)); err != nil {
			t.Errorf("%s is not embedded: add it to sources.txt and run go generate ./internal/assets", filepath.Base(f))
		}
	}
}
