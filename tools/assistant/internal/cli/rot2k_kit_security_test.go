package cli_test

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

// SEC1: a kit that is DOWNLOADED must be signed. A release without SHA256SUMS.sig is refused (also with --version and
// with --allow-unsigned): the unsigned red box exists for a local file the operator chose, not for the network.
func TestDownloaded2KKitWithoutSignatureIsRefused(t *testing.T) {
	var mu sync.Mutex
	var hits []string
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		hits = append(hits, r.URL.Path)
		mu.Unlock()
		if strings.HasSuffix(r.URL.Path, "SHA256SUMS.sig") {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write([]byte("not a kit, and it must never be opened"))
	}))
	t.Cleanup(srv.Close)
	for name, extra := range map[string][]string{
		"by version":                      {"--version", "1.0.0"},
		"by version and --allow-unsigned": {"--version", "1.0.0", "--allow-unsigned"},
	} {
		t.Run(name, func(t *testing.T) {
			e := newEnv(t)
			e.http = kittest.ClientFor(srv)
			t.Setenv("AKAM_KIT_URL_BASE", srv.URL)
			e.writeState("camera_model=" + rotModel + "\ncamera_ip=192.0.2.10\n")
			_, bdir := appfsDirs(t)
			// every question is answered yes: the refusal must not depend on a refused question
			r := e.run("yes\nyes\nyes\nyes\n", append([]string{"--model", rotModel, "update", "--backup-dir", bdir}, extra...)...)
			if r.rc == 0 {
				t.Fatalf("an unsigned downloaded kit was accepted:\n%s%s", r.out, r.err)
			}
			contains(t, r.out+r.err, "has no signature")
			notContains(t, r.out+r.err, "Unsigned kit", "Use this kit without a signature", "What will change")
		})
	}
}
