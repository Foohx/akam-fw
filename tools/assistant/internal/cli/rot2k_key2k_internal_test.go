package cli

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

const newKeyAnswer = "key_id:   3fa91c07\nsecret:   " + keyFileSecret + "\nscope:    admin\nname:     assistant\n# the secret is shown ONCE; the API (port 8092) picks the key up within seconds\n"

type keyConn struct {
	out, errOut string
	status      int
	cmds        []string
}

func (c *keyConn) Run(_ context.Context, cmd string, _ io.Reader) (camssh.Result, error) {
	c.cmds = append(c.cmds, cmd)
	return camssh.Result{Stdout: []byte(c.out), Stderr: []byte(c.errOut), Status: c.status}, nil
}
func (c *keyConn) Close() {}

func keyApp(t *testing.T) (*App, *bytes.Buffer, string) {
	dir := t.TempDir()
	var out bytes.Buffer
	a := &App{UI: &ui.Plain{Out: &out, ErrOut: &out}, Opt: &Options{}, St: &state.Store{Dir: dir}, Secrets: &state.Secrets{}}
	return a, &out, filepath.Join(dir, "api_key")
}

// The key is made by the camera, kept on the PC (0600), never printed, and registered for redaction.
func TestKey2KIsKeptAndNeverPrinted(t *testing.T) {
	a, out, path := keyApp(t)
	c := &keyConn{out: newKeyAnswer}
	k, err := a.makeKey2K(c, path)
	if err != nil || k.ID != "3fa91c07" {
		t.Fatalf("%v %+v", err, k)
	}
	if len(c.cmds) != 1 || c.cmds[0] != keyToolCmd2K || strings.Contains(c.cmds[0], keyFileSecret) {
		t.Fatalf("commands: %q", c.cmds)
	}
	b, err := os.ReadFile(path)
	if err != nil || strings.TrimSpace(string(b)) != "3fa91c07:"+keyFileSecret {
		t.Fatalf("the key file: %q %v", b, err)
	}
	if st, _ := os.Stat(path); runtime.GOOS != "windows" && st.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v", st.Mode())
	}
	if strings.Contains(out.String(), keyFileSecret) {
		t.Fatal("the secret was printed")
	}
	if got := a.Secrets.Redact("x " + keyFileSecret); strings.Contains(got, keyFileSecret) {
		t.Fatal("the secret is not registered for redaction")
	}
	// and the file is what update reads
	if _, ok, err := a.apiKey2K(); err != nil || !ok {
		t.Fatalf("apiKey2K: %v %v", ok, err)
	}
}

func TestKey2KFailuresKeepNothingAndShowNoSecret(t *testing.T) {
	for name, c := range map[string]*keyConn{
		"the name exists on the camera": {status: 1, errOut: "akam_api_key: a key named assistant exists\n"},
		"the app is not running":        {status: 1, errOut: "akam_api_key: /tmp/akamd not found\n"},
		"an answer without a key":       {out: "something else\n"},
		"a malformed secret":            {out: "key_id:   3fa91c07\nsecret:   nothex\n"},
	} {
		t.Run(name, func(t *testing.T) {
			a, out, path := keyApp(t)
			if _, err := a.makeKey2K(c, path); err == nil {
				t.Fatal("accepted")
			}
			if _, err := os.Stat(path); err == nil {
				t.Fatal("a key file was written")
			}
			if strings.Contains(out.String(), keyFileSecret) {
				t.Fatal("secret printed")
			}
		})
	}
	t.Run("the key cannot be saved: the id is shown, the secret is not", func(t *testing.T) {
		a, out, _ := keyApp(t)
		blocked := filepath.Join(t.TempDir(), "file")
		_ = os.WriteFile(blocked, []byte("x"), 0o600)
		if _, err := a.makeKey2K(&keyConn{out: newKeyAnswer}, filepath.Join(blocked, "api_key")); err == nil {
			t.Fatal("accepted")
		}
		if strings.Contains(out.String(), keyFileSecret) || !strings.Contains(out.String(), "3fa91c07") || !strings.Contains(out.String(), "revoke") {
			t.Fatalf("%s", out)
		}
	})
}
