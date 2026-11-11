package kit_test

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func TestAllowedSignersOptionsAreNotSilentlyIgnored(t *testing.T) {
	line := strings.TrimSpace(string(kittest.AllowedLine(kittest.NewSigner(t))))
	i := strings.Index(line, "ssh-")
	key := line[i:]
	for _, opt := range []string{"cert-authority", `valid-before="20200101"`, `namespaces="a",valid-before="20200101"`, `namespaces="a`} {
		if _, err := kit.ParseAllowedSigners([]byte("akam-release " + opt + " " + key + "\n")); err == nil {
			t.Errorf("option %q accepted", opt)
		}
	}
	a, err := kit.ParseAllowedSigners([]byte(`akam-release namespaces="akam-release,x" ` + key + "\n"))
	if err != nil || len(a) != 1 || len(a[0].Namespaces) != 2 {
		t.Errorf("namespaces: %v %v", a, err)
	}
}

func TestDownloadIsLimited(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(strings.Repeat("x", 5000)))
	}))
	defer srv.Close()
	out := filepath.Join(t.TempDir(), "f")
	err := kit.FetchLimit(context.Background(), srv.Client(), srv.URL+"/f", out, 4096)
	if err == nil || !strings.Contains(err.Error(), "larger than") {
		t.Fatalf("an over-long download was accepted: %v", err)
	}
	if _, err := os.Stat(out); err == nil {
		t.Error("the over-long download was kept")
	}
	if err := kit.FetchLimit(context.Background(), srv.Client(), srv.URL+"/f", out, 5000); err != nil {
		t.Errorf("a download at the limit failed: %v", err)
	}
}
