package cli_test

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

// realSessionOuter is a plain UI whose Session is the REAL full-screen session (the program reads its
// keys from a pipe and draws into a buffer): the flow gets the same ui.UI the terminal gives it.
type realSessionOuter struct {
	*ui.Plain
	in  io.Reader
	out io.Writer
}

func (o realSessionOuter) Session(fn func(ui.UI) error) error {
	return tui.RunSession(o.Plain, o.in, o.out, fn)
}

type screenBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *screenBuf) Write(p []byte) (int, error) { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Write(p) }
func (s *screenBuf) String() string              { s.mu.Lock(); defer s.mu.Unlock(); return s.b.String() }
func (s *screenBuf) Len() int                    { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Len() }

func (s *screenBuf) waitFor(t *testing.T, text string, d time.Duration) {
	t.Helper()
	end := time.Now().Add(d)
	for time.Now().Before(end) {
		if strings.Contains(s.String(), text) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for %q on the screen:\n%s", text, s.String())
}

// B1: `akam steps` -> flash through the real session. The write is under way: double Ctrl-C does not
// quit, the footer does not offer a stop, and the flash finishes and is verified.
func TestStepsFlashWriteIsLockedInTheRealSession(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.verifyWorld(t)
	st, _ := os.ReadFile(filepath.Join(w.state, "state"))
	var keep []string
	for _, l := range strings.Split(string(st), "\n") {
		if !strings.HasPrefix(l, "step_flash_done=") {
			keep = append(keep, l)
		}
	}
	w.writeState(strings.Join(keep, "\n"))
	t.Setenv("AKAM_REFLASH_POLL_MS", "400") // the write lasts several polls: time to press Ctrl-C

	inR, inW := io.Pipe()
	defer inW.Close()
	screen := &screenBuf{}
	h := w.hooks()
	h.UI = func(o *cli.Options, s cli.Streams, log *state.Log, sec *state.Secrets, dry bool) ui.UI {
		return realSessionOuter{cli.NewPlainUI(o, s, log, sec, dry), inR, screen}
	}
	done := make(chan result, 1)
	go func() {
		done <- runWith(w.env, h, "", append(w.base(), "--camera", "default", "--image", w.img, "steps")...)
	}()
	send := func(s string) {
		t.Helper()
		if _, err := inW.Write([]byte(s)); err != nil {
			t.Fatal(err)
		}
	}
	screen.waitFor(t, "Akam - camera", 5*time.Second)
	send("\r") // the cursor starts on the first step to do: the flash
	screen.waitFor(t, "Flash with untested firmware?", 10*time.Second)
	time.Sleep(400 * time.Millisecond) // the red box is not answered by type-ahead
	send("y")
	screen.waitFor(t, "Reflash over SSH", 10*time.Second)
	send("y")
	screen.waitFor(t, "Write the camera flash now?", 10*time.Second)
	time.Sleep(400 * time.Millisecond)
	send("y")
	screen.waitFor(t, "cannot be interrupted safely", 10*time.Second)
	mark := screen.Len()
	send("\x03\x03\x03")
	select {
	case r := <-done:
		t.Fatalf("double Ctrl-C quit the session during the write (rc %d):\n%s", r.rc, screen.String())
	case <-time.After(300 * time.Millisecond):
	}
	if seg := screen.String()[mark:]; strings.Contains(seg, "Ctrl-C stop") || strings.Contains(seg, "Esc stop") {
		t.Errorf("the footer offered a stop during the write:\n%s", seg)
	}
	// the flash goes on to the end
	screen.waitFor(t, "Flash done and verified", 20*time.Second)
	// the menu comes back (a new one, after this point): q leaves it
	mark = screen.Len()
	for end := time.Now().Add(10 * time.Second); !strings.Contains(screen.String()[mark:], "Enter run") && time.Now().Before(end); {
		time.Sleep(10 * time.Millisecond)
	}
	send("q")
	var r result
	select {
	case r = <-done:
	case <-time.After(10 * time.Second):
		t.Fatalf("the session did not end:\n%s", screen.String())
	}
	_ = r
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Error("the image was not written completely")
	}
}
