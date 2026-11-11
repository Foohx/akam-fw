package kit_test

import (
	"io"
	"math/rand"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func TestArchiveNamesCollidingOnWindowsOrMacOSRefused(t *testing.T) {
	cases := []struct {
		name  string
		files map[string][]byte
		want  string
	}{
		{"case", map[string][]byte{"docs/Readme.txt": []byte("a\n"), "docs/readme.txt": []byte("b\n")}, "differ only in case"},
		{"trailing dot", map[string][]byte{"docs/x.": []byte("a\n")}, "dot-terminated"},
		{"dot element", map[string][]byte{"docs/./x": []byte("a\n")}, "dot-terminated"},
		{"double slash", map[string][]byte{"docs//x": []byte("a\n")}, "dot-terminated"},
		{"sidecar in archive", map[string][]byte{".akam-links": []byte("overlay/bin/dropbear dropbearmulti\n")}, "only the extraction itself writes"},
	}
	for _, c := range cases {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Files: c.files})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, c.want)
	}
}

// The pre-scan parses attacker bytes: no mutation of a valid file may panic it.
func TestXZPrescanMutationsNeverPanic(t *testing.T) {
	base := handBuilt(0x16, goodRec())
	r := rand.New(rand.NewSource(7))
	for i := 0; i < 3000; i++ {
		b := append([]byte(nil), base...)
		for k := 0; k <= r.Intn(3); k++ {
			b[r.Intn(len(b))] = byte(r.Intn(256))
		}
		if i%5 == 0 {
			b = b[:r.Intn(len(b))]
		}
		rc, err := kit.XZReader(writeTemp(t, b), kit.XZLimits{})
		if err == nil {
			_, _ = io.Copy(io.Discard, rc)
			rc.Close()
		}
	}
}
