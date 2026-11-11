package sdcard

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
)

func tarball(files map[string][]byte, extra func(*tar.Writer)) []byte {
	var b bytes.Buffer
	zw := gzip.NewWriter(&b)
	tw := tar.NewWriter(zw)
	for p, c := range files {
		tw.WriteHeader(&tar.Header{Name: "lsc-tuya-toolkit-abc/sd_card/" + p, Typeflag: tar.TypeReg, Size: int64(len(c)), Mode: 0o644})
		tw.Write(c)
	}
	tw.WriteHeader(&tar.Header{Name: "lsc-tuya-toolkit-abc/README.md", Typeflag: tar.TypeReg, Size: 1, Mode: 0o644})
	tw.Write([]byte("x"))
	if extra != nil {
		extra(tw)
	}
	tw.Close()
	zw.Close()
	return b.Bytes()
}

func withPin(t *testing.T, tree map[string][]byte, srv *httptest.Server) {
	oldPin, oldURL := PinTreeSHA, ToolkitURL
	PinTreeSHA, ToolkitURL = TreeHash(tree), srv.URL+"/tk.tar.gz"
	t.Cleanup(func() { PinTreeSHA, ToolkitURL = oldPin, oldURL })
}

func TestEnsureToolkitDownloadsAndChecksThePin(t *testing.T) {
	tree := synthToolkit()
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(tarball(tree, nil)) }))
	defer srv.Close()
	withPin(t, tree, srv)
	dir := filepath.Join(t.TempDir(), "tk")
	got, err := EnsureToolkit(context.Background(), srv.Client(), dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(tree) {
		t.Errorf("%d files, want %d", len(got), len(tree))
	}
	// second call: from disk, no download
	srv.Close()
	if _, err := EnsureToolkit(context.Background(), srv.Client(), dir, nil); err != nil {
		t.Errorf("second: %v", err)
	}
}

func TestEnsureToolkitRefusesAWrongTree(t *testing.T) {
	tree := synthToolkit()
	bad := synthToolkit()
	bad["hack.sh"] = []byte("evil")
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(tarball(bad, nil)) }))
	defer srv.Close()
	withPin(t, tree, srv)
	dir := filepath.Join(t.TempDir(), "tk")
	if _, err := EnsureToolkit(context.Background(), srv.Client(), dir, nil); err == nil {
		t.Fatal("a tree with another hash was accepted")
	}
	if _, err := LoadToolkit(dir); err == nil {
		t.Error("a refused toolkit was left in place")
	}
}

func TestEnsureToolkitRefusesLinksAndPlainHTTP(t *testing.T) {
	tree := synthToolkit()
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(tarball(tree, func(tw *tar.Writer) {
			tw.WriteHeader(&tar.Header{Name: "lsc-tuya-toolkit-abc/sd_card/link", Typeflag: tar.TypeSymlink, Linkname: "/etc/passwd"})
		}))
	}))
	defer srv.Close()
	withPin(t, tree, srv)
	if _, err := EnsureToolkit(context.Background(), srv.Client(), filepath.Join(t.TempDir(), "tk"), nil); err == nil {
		t.Error("a link in the archive was accepted")
	}
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(tarball(tree, nil)) }))
	defer plain.Close()
	ToolkitURL = plain.URL + "/tk.tar.gz"
	if _, err := EnsureToolkit(context.Background(), nil, filepath.Join(t.TempDir(), "tk2"), nil); err == nil {
		t.Error("plain HTTP accepted by the default client")
	}
}
