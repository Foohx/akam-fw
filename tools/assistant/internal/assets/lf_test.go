package assets

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Windows checkouts must never turn the embedded scripts into CRLF: no file under scripts has a CR,
// and the attributes file at the repository root pins the rule that keeps it so.
func TestEmbeddedScriptsAreLF(t *testing.T) {
	ents, err := os.ReadDir("scripts")
	if err != nil || len(ents) == 0 {
		t.Fatalf("scripts: %v", err)
	}
	for _, e := range ents {
		b, err := os.ReadFile(filepath.Join("scripts", e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		if bytes.ContainsRune(b, '\r') {
			t.Errorf("%s contains a CR", e.Name())
		}
	}
	ga, err := os.ReadFile(filepath.Join("..", "..", "..", "..", ".gitattributes"))
	if err != nil {
		t.Fatal(err)
	}
	for _, rule := range []string{"*.sh text eol=lf", "*.embed text eol=lf", "tools/assistant/internal/assets/scripts/** text eol=lf", "*.txt text eol=lf"} {
		if !strings.Contains(string(ga), rule+"\n") {
			t.Errorf("attributes file lacks %q", rule)
		}
	}
}
