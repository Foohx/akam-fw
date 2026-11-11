package cli_test

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

// kitServer serves a built kit over HTTPS (a test certificate) and records the requested paths.
type kitServer struct {
	srv  *httptest.Server
	mu   sync.Mutex
	hits []string
}

func newKitServer(t *testing.T, b kittest.Built, noSig bool) *kitServer {
	t.Helper()
	ks := &kitServer{}
	ks.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ks.mu.Lock()
		ks.hits = append(ks.hits, r.URL.Path)
		ks.mu.Unlock()
		name := filepath.Base(r.URL.Path)
		if !strings.HasPrefix(r.URL.Path, "/v1.0.0/") || (noSig && name == "SHA256SUMS.sig") {
			http.NotFound(w, r)
			return
		}
		data, err := os.ReadFile(filepath.Join(b.Dir, name))
		if err != nil {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write(data)
	}))
	t.Cleanup(ks.srv.Close)
	return ks
}

func getKitEnv(t *testing.T, ks *kitServer, b *kittest.Built) *env {
	t.Helper()
	e := newEnv(t)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\n")
	if ks != nil {
		e.http = ks.srv.Client()
		t.Setenv("AKAM_KIT_URL_BASE", ks.srv.URL)
	}
	if b != nil {
		f := filepath.Join(t.TempDir(), "allowed_signers")
		if err := os.WriteFile(f, b.AllowedSigners, 0o600); err != nil {
			t.Fatal(err)
		}
		t.Setenv("AKAM_ALLOWED_SIGNERS", f)
	}
	return e
}

func TestGetKitDownloadsAndVerifiesASignedKit(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, &b)
	r := e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 0)
	contains(t, r.out, "Step 2/9 - Get the Akam kit",
		"Downloading kit 1.0.0 from "+ks.srv.URL+"/v1.0.0 (HTTPS only; nothing from it is run before it is verified).",
		"ok: akam-kit-1.0.0.tar.xz sha256 "+b.TarSHA+" matches SHA256SUMS",
		"ok: signature valid (Good \"akam-release\" signature for akam-release with ED25519 key SHA256:",
		"ok: archive layout is safe", "ok: kit_format 1 is supported", "MANIFEST entries match (akam_version 1.0.0",
		"Kit verified (format 1, sha256 "+b.TarSHA[:12]+"...).",
		"Tip: compare that sha256 with the one in the release notes, or pass --expect-sha256 HEX.")
	if e.readState("mode") != "kit" || e.readState("kit_sha") != b.TarSHA || e.readState("kit_unsigned_ok") != "0" || e.readState("kit_version") != "1.0.0" || e.readState("kit_format") != "1" {
		t.Errorf("state: mode=%q sha=%q unsigned=%q version=%q format=%q", e.readState("mode"), e.readState("kit_sha"), e.readState("kit_unsigned_ok"), e.readState("kit_version"), e.readState("kit_format"))
	}
	if e.readState("step_get_kit_done") == "" {
		t.Error("step not marked done")
	}
	for _, f := range []string{"bin/flash_tool", "scripts/flash/akam_reflash_ssh.sh", "MANIFEST.txt"} {
		if _, err := os.Stat(filepath.Join(e.state, "kit", f)); err != nil {
			t.Errorf("kit not extracted: %v", err)
		}
	}
	if _, err := os.Stat(filepath.Join(e.state, "downloads", "1.0.0", "SHA256SUMS.sig")); err != nil {
		t.Error("signature not kept")
	}
	if got := strings.Join(ks.hits, " "); got != "/v1.0.0/akam-kit-1.0.0.tar.xz /v1.0.0/SHA256SUMS /v1.0.0/SHA256SUMS.sig" {
		t.Errorf("requests: %s", got)
	}
	// the state it leaves is a valid get-kit step
	r = e.run("", "status")
	contains(t, r.out, "2. [done   ] get-kit")
	// a second run replaces the extracted tree
	expectRC(t, e.run("", "get-kit", "--version", "v1.0.0", "--expect-sha256", b.TarSHA[:12]), 0)
}

// Today's allowed_signers has no key: a signed kit is refused, exactly as verify_kit.sh does.
func TestGetKitRefusesASignedKitWhileNoKeyIsPublished(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, nil)
	r := e.run("", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 1)
	contains(t, r.err+r.out, "verify_kit: FAIL: signature present but allowed_signers contains no public key (placeholder not replaced): cannot authenticate",
		"the kit failed verification (see the message above).")
	if e.readState("step_get_kit_done") != "" {
		t.Error("marked done")
	}
	if _, err := os.Stat(filepath.Join(e.state, "kit")); err == nil {
		t.Error("an unverified kit was extracted")
	}
}

func TestGetKitUnsignedNeedsConsent(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{Unsigned: true})
	for _, in := range []string{"", "\n", "n\n", "q\n", "yes please\n"} {
		ks := newKitServer(t, b, true)
		e := getKitEnv(t, ks, &b)
		r := e.run(in, "get-kit", "--version", "1.0.0")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "no signature file in this release: the kit would be checked by checksum only.",
			"this kit is NOT signed: only its checksum can be verified, so you must trust where you got it.",
			"Unsigned kit", "unsigned kit refused.")
		contains(t, r.err, "Use this kit without a signature? [y/N]")
		if e.readState("step_get_kit_done") != "" {
			t.Errorf("input %q: marked done", in)
		}
	}
	ks := newKitServer(t, b, true)
	e := getKitEnv(t, ks, &b)
	r := e.run("y\n", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 0)
	if e.readState("kit_unsigned_ok") != "1" {
		t.Error("the consent was not recorded")
	}
	// no terminal: refused whatever the input
	e = getKitEnv(t, newKitServer(t, b, true), &b)
	t.Setenv("AKAM_FORCE_TTY", "")
	r = e.run("y\n", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 1)
	t.Setenv("AKAM_FORCE_TTY", "1")
	// --allow-unsigned asks nothing
	e = getKitEnv(t, newKitServer(t, b, true), &b)
	r = e.run("", "get-kit", "--version", "1.0.0", "--allow-unsigned")
	expectRC(t, r, 0)
	notContains(t, r.err, "Use this kit without a signature?")
	if e.readState("kit_unsigned_ok") != "1" {
		t.Error("kit_unsigned_ok")
	}
}

func TestGetKitRefusals(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	t.Run("release not found", func(t *testing.T) {
		ks := newKitServer(t, b, false)
		e := getKitEnv(t, ks, &b)
		r := e.run("", "get-kit", "--version", "9.9.9")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "release v9.9.9 was not found (akam-kit-9.9.9.tar.xz). Akam may not have published a release yet.")
	})
	t.Run("tampered download", func(t *testing.T) {
		bad := kittest.Build(t, t.TempDir(), kittest.Spec{BadSumsLine: true})
		e := getKitEnv(t, newKitServer(t, bad, false), &bad)
		r := e.run("", "get-kit", "--version", "1.0.0")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "sha256 mismatch for akam-kit-1.0.0.tar.xz", "do not use it.")
	})
	t.Run("expected sha256 differs", func(t *testing.T) {
		e := getKitEnv(t, newKitServer(t, b, false), &b)
		r := e.run("", "get-kit", "--version", "1.0.0", "--expect-sha256", "deadbeefdead")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "does not start with --expect-sha256")
	})
	t.Run("plain http is never used", func(t *testing.T) {
		e := getKitEnv(t, nil, &b)
		t.Setenv("AKAM_KIT_URL_BASE", "http://127.0.0.1:1")
		r := e.run("", "get-kit", "--version", "1.0.0")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "refusing a non-HTTPS download address")
	})
	t.Run("redirect to http is refused", func(t *testing.T) {
		srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "http://127.0.0.1:1/evil", http.StatusFound)
		}))
		defer srv.Close()
		e := getKitEnv(t, nil, &b)
		e.http = srv.Client()
		t.Setenv("AKAM_KIT_URL_BASE", srv.URL)
		r := e.run("", "get-kit", "--version", "1.0.0")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "refusing a redirect to a non-HTTPS address")
	})
	t.Run("bad version", func(t *testing.T) {
		e := getKitEnv(t, nil, &b)
		r := e.run("", "get-kit", "--version", "1/../2")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "'1/../2' is not a valid version.")
	})
	t.Run("development checkout has no release", func(t *testing.T) {
		e := getKitEnv(t, nil, &b)
		r := e.runVersion("1.0.0-dev", "get-kit")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "this development checkout has no release to download.", "pass --kit FILE for a kit you already have")
	})
	t.Run("blocked before doctor", func(t *testing.T) {
		e := newEnv(t)
		r := e.run("", "get-kit", "--kit", b.Tarball)
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "step 'get-kit' needs these steps first: doctor")
	})
}

func TestGetKitFromAFile(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e := getKitEnv(t, nil, &b)
	r := e.run("", "get-kit", "--kit", b.Tarball)
	expectRC(t, r, 0)
	if e.readState("kit_path") != b.Tarball {
		t.Errorf("kit_path %q", e.readState("kit_path"))
	}
	r = e.run("", "get-kit", "--kit", filepath.Join(b.Dir, "missing.tar.xz"))
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "kit not found: ", "check the path given to --kit.")
	// a folder cannot be authenticated
	folder := filepath.Join(t.TempDir(), "kitcopy")
	if err := kit.CopyTree(filepath.Join(e.state, "kit"), folder); err != nil {
		t.Fatal(err)
	}
	r = e.run("", "get-kit", "--kit", folder)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the kit is a folder, which cannot be authenticated")
	r = e.run("", "get-kit", "--kit", folder, "--allow-unsigned")
	expectRC(t, r, 0)
	contains(t, r.err+r.out, "directory mode: the tarball signature is not checked here")
	if e.readState("kit_unsigned_ok") != "1" {
		t.Error("a folder kit must be recorded as unsigned")
	}
}

func TestGetKitDryRun(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	ks := newKitServer(t, b, false)
	e := getKitEnv(t, ks, &b)
	e.writeState("")
	if err := os.WriteFile(filepath.Join(e.state, "state.dry"), []byte("step_doctor_done=2026-11-11T00:00:00Z\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r := e.run("", "--dry-run", "get-kit", "--version", "1.0.0")
	expectRC(t, r, 0)
	contains(t, r.out, "REHEARSAL (--dry-run)", "WOULD download "+ks.srv.URL+"/v1.0.0/akam-kit-1.0.0.tar.xz", "WOULD verify ")
	if len(ks.hits) != 0 {
		t.Errorf("a dry run downloaded: %v", ks.hits)
	}
	r = e.runVersion("1.0.0-dev", "--dry-run", "get-kit")
	expectRC(t, r, 0)
	contains(t, r.out, "(development checkout: using the placeholder version X.Y.Z)")
}
