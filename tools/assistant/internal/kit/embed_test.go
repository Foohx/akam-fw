package kit_test

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// The embedded copy must be the file the maintainer edits.
func TestEmbeddedAllowedSignersMatchesRelease(t *testing.T) {
	b, err := os.ReadFile(filepath.Join("..", "..", "..", "release", "allowed_signers"))
	if err != nil {
		t.Skip("skip: not in a source checkout")
	}
	if !bytes.Equal(b, kit.DefaultAllowedSigners) {
		t.Fatal("internal/kit/allowed_signers differs from tools/release/allowed_signers: copy it again")
	}
}
