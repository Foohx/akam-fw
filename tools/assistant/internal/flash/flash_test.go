package flash_test

import (
	"bytes"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	gossh "golang.org/x/crypto/ssh"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/flash"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

type world struct {
	cam  *testfake.Camera
	plan flash.Plan
	deps flash.Deps
	out  *[]string
	data []byte
}

func newWorld(t *testing.T) *world {
	t.Helper()
	dir := t.TempDir()
	key := filepath.Join(dir, "k")
	if err := camssh.GenerateKey(key, "t", nil); err != nil {
		t.Fatal(err)
	}
	pb, _ := os.ReadFile(key + ".pub")
	pub, _, _, _, err := gossh.ParseAuthorizedKey(pb)
	if err != nil {
		t.Fatal(err)
	}
	cam := testfake.New(t, testfake.Options{AuthorizedKey: pub})
	data := bytes.Repeat([]byte{0x5a, 0xa5, 0x01}, 5242880/3+1)[:5242880]
	img := filepath.Join(dir, "img")
	_ = os.WriteFile(img, data, 0o600)
	tool := filepath.Join(dir, "flash_tool")
	_ = os.WriteFile(tool, []byte("tool"), 0o755)
	m := md5.Sum(data)
	var out []string
	w := &world{cam: cam, out: &out, data: data}
	w.plan = flash.Plan{Image: img, MD5: hex.EncodeToString(m[:]), FlashTool: tool,
		ScriptsDir: filepath.Join("..", "..", "..", "flash_scripts"), Port: cam.SSHPort(), SecondPort: cam.SecondPort(),
		Poll: 10 * time.Millisecond, RetrySleep: time.Millisecond}
	w.deps = flash.Deps{
		Out: func(l string) { out = append(out, l) },
		Dial: func(port int) (flash.Conn, error) {
			c, err := camssh.Dial(camssh.Config{Host: "127.0.0.1", Port: port, KeyPath: key, KnownHosts: filepath.Join(dir, "kh"),
				TrustFingerprint: cam.HostFingerprint(), Timeout: 5 * time.Second})
			if err != nil {
				return nil, err
			}
			return c, nil
		},
	}
	return w
}

func (w *world) untouched(t *testing.T) {
	t.Helper()
	if !bytes.Equal(w.cam.Mtd7(), testfake.Mtd7Initial()) || w.cam.FlashStarts() != 0 {
		t.Error("the flash was touched")
	}
}

func confirmed(t *testing.T, md5 string) flash.Confirmation {
	t.Helper()
	c, err := flash.NewConfirmation(md5, func() (bool, error) { return true, nil })
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestFlashNeedsAnExplicitConfirmationForThisImage(t *testing.T) {
	w := newWorld(t)
	// the zero value is not a confirmation
	if err := flash.Flash(w.plan, flash.Confirmation{}, w.deps); err == nil || !strings.Contains(err.Error(), "refusing to flash without an explicit confirmation") {
		t.Fatalf("zero confirmation accepted: %v", err)
	}
	// a confirmation for another image is not valid for this one
	other := confirmed(t, strings.Repeat("a", 32))
	if err := flash.Flash(w.plan, other, w.deps); err == nil {
		t.Fatal("a confirmation for another md5 was accepted")
	}
	if len(w.cam.Commands()) != 0 {
		t.Errorf("commands sent: %q", w.cam.Commands())
	}
	// "no" and a failing question both give no Confirmation
	if _, err := flash.NewConfirmation(w.plan.MD5, func() (bool, error) { return false, nil }); !errors.Is(err, flash.ErrNotConfirmed) {
		t.Errorf("no: %v", err)
	}
	boom := errors.New("no terminal")
	if _, err := flash.NewConfirmation(w.plan.MD5, func() (bool, error) { return false, boom }); !errors.Is(err, boom) {
		t.Errorf("error: %v", err)
	}
	w.untouched(t)
	// and a valid one flashes
	if err := flash.Flash(w.plan, confirmed(t, w.plan.MD5), w.deps); err != nil {
		t.Fatalf("%v\n%s", err, strings.Join(*w.out, "\n"))
	}
	if !bytes.Equal(w.cam.Mtd7(), w.data) {
		t.Error("not flashed")
	}
}

func TestPlanIsValidatedBeforeAnythingTouchesTheCamera(t *testing.T) {
	cases := []struct {
		name string
		mod  func(*world)
		want string
	}{
		{"short md5", func(w *world) { w.plan.MD5 = "abc" }, "--md5 must be 32 lowercase hex digits"},
		{"upper-case md5", func(w *world) { w.plan.MD5 = strings.ToUpper(w.plan.MD5) }, "--md5 must be 32 lowercase hex digits"},
		{"wrong md5", func(w *world) { w.plan.MD5 = strings.Repeat("0", 32) }, "you said 00000000000000000000000000000000: refusing"},
		{"wrong size", func(w *world) {
			p := filepath.Join(t.TempDir(), "short")
			_ = os.WriteFile(p, []byte("x"), 0o600)
			w.plan.Image = p
		}, "image is 1 bytes, expected exactly 5242880 (full APP partition)"},
		{"missing image", func(w *world) { w.plan.Image = "/nonexistent/img" }, "image not found"},
		{"missing tool", func(w *world) { w.plan.FlashTool = "/nonexistent/tool" }, "flash tool not found"},
		{"missing scripts", func(w *world) { w.plan.ScriptsDir = t.TempDir() }, "missing"},
		{"remote dir outside /tmp", func(w *world) { w.plan.RemoteDir = "/etc/akam" }, "must be below /tmp"},
		{"remote dir with ..", func(w *world) { w.plan.RemoteDir = "/tmp/../etc" }, "must be a plain absolute path"},
		{"remote dir with a quote", func(w *world) { w.plan.RemoteDir = "/tmp/a'b" }, "must be a plain absolute path"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			w := newWorld(t)
			c.mod(w)
			err := flash.Flash(w.plan, confirmed(t, w.plan.MD5), w.deps)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("want %q, got %v", c.want, err)
			}
			if err := flash.Precheck(w.plan, w.deps); err == nil {
				t.Fatal("precheck accepted a bad plan")
			}
			if n := len(w.cam.Commands()); n != 0 {
				t.Errorf("%d commands sent for a bad plan", n)
			}
			w.untouched(t)
		})
	}
}

func TestPrecheckWritesNothing(t *testing.T) {
	w := newWorld(t)
	if err := flash.Precheck(w.plan, w.deps); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.Join(*w.out, "\n"), "reflash_over_ssh: precheck-only: done, nothing written.") {
		t.Errorf("output: %q", *w.out)
	}
	w.untouched(t)
	if len(w.cam.RamFile("image.squashfs")) != 0 || len(w.cam.RamFile("flash_tool")) != 0 {
		t.Error("precheck uploaded the image or the tool")
	}
	for _, c := range w.cam.Commands() {
		if strings.Contains(c, "start") || strings.Contains(c, "prepare") {
			t.Errorf("precheck sent %q", c)
		}
	}
}

// TOCTOU: the image, flash_tool and scripts are read once; replacing the files after the md5 check
// (here: at the first connection) cannot change what is uploaded and written.
func TestFilesReplacedAfterTheCheckAreNotUploaded(t *testing.T) {
	w := newWorld(t)
	dial := w.deps.Dial
	swapped := false
	w.deps.Dial = func(port int) (flash.Conn, error) {
		if !swapped {
			swapped = true
			if err := os.WriteFile(w.plan.Image, bytes.Repeat([]byte{0x66}, 5242880), 0o600); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(w.plan.FlashTool, []byte("evil tool"), 0o755); err != nil {
				t.Fatal(err)
			}
		}
		return dial(port)
	}
	if err := flash.Flash(w.plan, confirmed(t, w.plan.MD5), w.deps); err != nil {
		t.Fatalf("%v\n%s", err, strings.Join(*w.out, "\n"))
	}
	if !swapped {
		t.Fatal("the swap did not run")
	}
	if !bytes.Equal(w.cam.Mtd7(), w.data) {
		t.Error("the camera did not get the image that was confirmed")
	}
}
