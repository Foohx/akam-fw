package cli

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// TestGoldensHoldNoSecret: no golden file of the assistant (cli, ui, session UI) holds a password the
// tests type (Wi-Fi, stream, MQTT, prompts) or a clear "password=value" assignment.
func TestGoldensHoldNoSecret(t *testing.T) {
	secrets := []string{"sup3r-secret-wifi", "S3NTINEL-br0ker-pw", "open sesame 42", "hunter2", "Zq9-Hunter-Wifi-Pw"}
	assign := regexp.MustCompile(`(?i)(pass(word)?|psk)=[^*\s']`)
	n := 0
	for _, dir := range []string{"testdata", "../ui/testdata", "../ui/tui/testdata"} {
		_ = filepath.Walk(dir, func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() {
				return nil
			}
			b, _ := os.ReadFile(p)
			n++
			for _, s := range secrets {
				if strings.Contains(string(b), s) {
					t.Errorf("%s holds the secret %q", p, s)
				}
			}
			if assign.Match(b) {
				t.Errorf("%s holds a clear password assignment", p)
			}
			return nil
		})
	}
	if n == 0 {
		t.Error("no golden file found")
	}
}
