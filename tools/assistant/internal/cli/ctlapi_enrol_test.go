package cli_test

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The 1080p is driven through the signed API. An install made before that (text port 8091 open to the network, no API key) keeps
// working: the first command that needs the key offers to create it over SSH, once.

func keyFile(e *env) string { return filepath.Join(e.state, "api_key") }

// a secret never reaches a screen, the log or the command lines the camera receives
func noSecretAnywhere(t *testing.T, e *env, outs ...string) {
	t.Helper()
	b, err := os.ReadFile(keyFile(e))
	if err != nil {
		t.Fatal(err)
	}
	k, err := akamapi.ParseKey(string(b))
	if err != nil {
		t.Fatal(err)
	}
	secret := regexp.MustCompile(`[0-9a-f]{64}`).FindString(string(b))
	if len(secret) != 64 || k.ID == "" {
		t.Fatalf("key file %q", b)
	}
	for _, o := range append(outs, e.logText(), strings.Join(e.cam.Commands(), "\n")) {
		if strings.Contains(o, secret) {
			t.Fatal("the secret of the API key appeared in an output, the log or a command line")
		}
	}
	if st, err := os.Stat(keyFile(e)); err != nil || st.Mode().Perm() != 0o600 {
		t.Errorf("key file mode %v %v", st, err)
	}
}

func TestExistingInstallEnrolsTheKeyOnTheFirstSettingCommand(t *testing.T) {
	e := newEnv(t)
	e.noAPIKey()
	e.openCtlPort() // an install from before the text port was closed: 8091 open, no key
	if e.cam.APIKeys() != 0 {
		t.Fatal("the camera has a key")
	}
	r := e.run("n\n", "led") // a terminal, the question defaults to No
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "Create one over SSH", "not confirmed: nothing was changed on the camera", "run akam api-key when you want to create it")
	if e.cam.APIKeys() != 0 {
		t.Fatal("a key was made without a yes")
	}
	if _, err := os.Stat(keyFile(e)); err == nil {
		t.Fatal("a key file without a yes")
	}
	r = e.run("y\n", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "API key", "created on the camera and saved in", "the secret is not shown", "Front LED: auto")
	if e.cam.APIKeys() != 1 {
		t.Fatalf("keys on the camera: %d", e.cam.APIKeys())
	}
	noSecretAnywhere(t, e, r.out, r.err)
	// the next run does not ask again and goes through the API
	r = e.run("", "led")
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, "Create one over SSH")
	// every setting travelled through the API, never through the text port
	for _, w := range e.cam.CtlWords() {
		t.Errorf("the text port received %q", w)
	}
	for _, c := range e.cam.Commands() {
		if strings.Contains(c, "nc ") {
			t.Errorf("an nc session was opened: %s", c)
		}
	}
}

func TestEnrolmentWithYesAndWithoutATerminal(t *testing.T) {
	e := newEnv(t)
	e.noAPIKey()
	t.Setenv("AKAM_FORCE_TTY", "")
	r := e.run("y\n", "led")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "no API key for this camera", "run once, in a terminal: akam api-key")
	if e.cam.APIKeys() != 0 {
		t.Fatal("a key was made without a terminal and without --yes")
	}
	r = e.run("", "--yes", "led")
	expectRC(t, r, 0)
	if e.cam.APIKeys() != 1 {
		t.Fatalf("--yes did not make the key: %d", e.cam.APIKeys())
	}
	noSecretAnywhere(t, e, r.out, r.err)
}

func TestRehearsalNeverMakesAKey(t *testing.T) {
	e := newEnv(t)
	e.noAPIKey()
	r := e.run("", "--dry-run", "led", "status", "off")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD use the signed API", "WOULD ask once, if this PC has no API key")
	if e.cam.APIKeys() != 0 || len(e.cam.Commands()) != 0 {
		t.Errorf("a rehearsal touched the camera: %d keys, %v", e.cam.APIKeys(), e.cam.Commands())
	}
}

func TestApiKeyCommandStatusReplaceAndALostKey(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "api-key")
	expectRC(t, r, 0)
	contains(t, r.out, "the camera accepts it", "lsc-smart-indoor-1080p")
	// the camera forgot the key (reset): the PC's file is refused; the command offers to replace it
	e.cam.RevokeAPIKeys()
	r = e.run("y\n", "api-key")
	expectRC(t, r, 0)
	contains(t, r.err+r.out, "does not answer its control API", "Replace it with a new one over SSH")
	if e.cam.APIKeys() != 1 {
		t.Fatalf("keys %d", e.cam.APIKeys())
	}
	noSecretAnywhere(t, e, r.out, r.err)
	// this PC lost its file but the camera still has the key named assistant: the old one is revoked first, after a yes
	if err := os.Remove(keyFile(e)); err != nil {
		t.Fatal(err)
	}
	r = e.run("y\nn\n", "api-key") // create one? yes. The camera has one of that name: revoke it? no
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "already has a key named 'assistant'")
	if e.cam.APIKeys() != 1 {
		t.Fatalf("a refusal changed the keys: %d", e.cam.APIKeys())
	}
	r = e.run("y\ny\n", "api-key") // yes, and yes to the revocation
	expectRC(t, r, 0)
	if e.cam.APIKeys() != 1 {
		t.Fatalf("replace must leave one key: %d", e.cam.APIKeys())
	}
	noSecretAnywhere(t, e, r.out, r.err)
	r = e.run("", "led")
	expectRC(t, r, 0)
}

func TestARefusedKeyOffersToReplaceItDuringACommand(t *testing.T) {
	e := newEnv(t)
	e.cam.RevokeAPIKeys()
	if _, err := e.cam.NewAPIKey("assistant", "admin"); err != nil { // the camera was reinstalled: a key with this name, not this PC's
		t.Fatal(err)
	}
	r := e.run("n\n", "led")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "the camera refused this PC's API key")
	r = e.run("y\n", "led")
	expectRC(t, r, 0)
	contains(t, r.out, "Front LED: auto")
	noSecretAnywhere(t, e, r.out, r.err)
}

func TestVerifyOffersTheKeyAtTheEndOfAnInstall(t *testing.T) {
	w := newFlashWorld(t, testfake.FlashOptions{})
	w.noAPIKey()
	w.verifyWorld(t)
	// Enter (power cycled), "no" to the stream password, "no" to ONVIF, "no" to time sync
	r := w.run("\nn\nn\nn\n", "verify")
	expectRC(t, r, 0)
	if w.cam.APIKeys() != 0 {
		t.Fatalf("a key was made without a yes: %d", w.cam.APIKeys())
	}
	contains(t, r.out+r.err, "This PC has no API key for this camera yet", "not confirmed")
	w = newFlashWorld(t, testfake.FlashOptions{})
	w.noAPIKey()
	w.verifyWorld(t)
	r = w.run("\ny\nn\nn\nn\n", "verify") // Enter, yes to the key
	expectRC(t, r, 0)
	if w.cam.APIKeys() != 1 {
		t.Fatalf("verify did not make the key: %d", w.cam.APIKeys())
	}
	contains(t, r.out, "created on the camera and saved in")
	noSecretAnywhere(t, w.env, r.out, r.err)
}

// A camera still on firmware 0.1.x has no /usr/bin/akamd: the key commands fall back to /usr/bin/ak_rtsp.
func TestApiKeyFallsBackToTheOldProgramName(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.LegacyNames = true })
	r := e.run("y\n", "api-key")
	expectRC(t, r, 0)
	if e.cam.APIKeys() != 1 {
		t.Fatalf("keys %d, the old-name camera did not make the key\n%s%s", e.cam.APIKeys(), r.out, r.err)
	}
	noSecretAnywhere(t, e, r.out, r.err)
}

// Without a key and without a terminal the first error says why; no second, misleading "did not answer" follows.
func TestNoSecondErrorAfterTheKeyError(t *testing.T) {
	for _, args := range [][]string{{"audio", "show"}, {"led"}, {"picture"}} {
		e := newEnv(t)
		e.noAPIKey()
		t.Setenv("AKAM_FORCE_TTY", "")
		r := e.run("", args...)
		expectRC(t, r, 1)
		contains(t, r.err+r.out, "no API key for this camera")
		notContains(t, r.err+r.out, "did not answer", "older Akam image")
	}
}
