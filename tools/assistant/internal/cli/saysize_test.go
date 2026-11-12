package cli_test

import (
	"os"
	"path/filepath"
	"testing"
)

// A huge file is refused before it is read into memory (a sparse file: no disk is used).
func TestSayRefusesHugeFile(t *testing.T) {
	e := talkEnv(t)
	e.pass = talkPass
	f := filepath.Join(t.TempDir(), "huge.wav")
	fh, err := os.Create(f)
	if err != nil {
		t.Fatal(err)
	}
	if err := fh.Truncate(257 << 20); err != nil {
		t.Skip("cannot make a sparse file here")
	}
	fh.Close()
	r := e.run("", "say", f)
	expectRC(t, r, 1)
	contains(t, r.err, "is too large")
	if len(e.cam.RTSP.Requests()) != 0 {
		t.Error("the camera was contacted")
	}
}
