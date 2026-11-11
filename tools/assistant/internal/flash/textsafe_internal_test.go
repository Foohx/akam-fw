package flash

import (
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// What a camera prints is untrusted: escape sequences must not reach the terminal.
func TestCameraOutputIsSanitised(t *testing.T) {
	var out []string
	r := &runner{d: Deps{Out: func(l string) { out = append(out, l) }}}
	r.showCamera("hello \x1b[2J\x1b]0;title\x07world\r\n\x1b[31m\n")
	r.showRaw(camssh.Result{Stdout: []byte("ok\x1b[Hline\x07\n"), Stderr: []byte("\x9b31merr\n")})
	all := strings.Join(out, "\n")
	for _, bad := range []string{"\x1b", "\x07", "\x9b"} {
		if strings.Contains(all, bad) {
			t.Errorf("control character %q reached the output: %q", bad, out)
		}
	}
	if !strings.Contains(all, "camera: hello world") || !strings.Contains(all, "okline") {
		t.Errorf("text lost: %q", out)
	}
	for _, l := range out {
		if l == "camera: " {
			t.Error("an empty camera line was printed")
		}
	}
}
