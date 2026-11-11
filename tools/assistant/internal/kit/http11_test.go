package kit_test

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// The release binary is built with -tags nethttpomithttp2 (no HTTP/2 client): the production client must still
// download from a TLS server that offers HTTP/2 (GitHub does), over HTTP/1.1.
func TestDownloadWorksOverHTTP11WhenServerOffersHTTP2(t *testing.T) {
	var proto atomic.Int32
	srv := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		proto.Store(int32(r.ProtoMajor))
		_, _ = w.Write([]byte("kit bytes"))
	}))
	srv.EnableHTTP2 = true
	srv.StartTLS()
	defer srv.Close()
	c := kit.NewHTTPClient()
	c.Transport.(*http.Transport).TLSClientConfig.RootCAs = srv.Client().Transport.(*http.Transport).TLSClientConfig.RootCAs
	out := filepath.Join(t.TempDir(), "f")
	if err := kit.Fetch(context.Background(), c, srv.URL+"/f", out); err != nil {
		t.Fatalf("download: %v", err)
	}
	if b, _ := os.ReadFile(out); string(b) != "kit bytes" {
		t.Errorf("content %q", b)
	}
	if proto.Load() != 1 {
		t.Errorf("the download used HTTP/%d, want HTTP/1.1 (the client must not depend on the bundled HTTP/2)", proto.Load())
	}
}
