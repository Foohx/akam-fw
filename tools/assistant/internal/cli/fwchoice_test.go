package cli_test

import (
	"bytes"
	"context"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// "Which firmware?": the release list, the chooser, the downgrade question, local kits and images, the
// stock firmware, the plain commands and the state they leave. Everything is synthetic: one HTTPS test server
// plays GitHub's API and the download address, nothing leaves the machine.

// relJSON is one release object of the API answer.
func relJSON(tag string, draft, pre bool) string {
	return relJSONFor(kittest.DefaultModel, tag, draft, pre)
}

// relJSONFor is relJSON for the kit of a given model. tag is the version with a v ("v1.0.0"): the release tag is
// firmware-v1.0.0.
func relJSONFor(model, tag string, draft, pre bool) string {
	ver := strings.TrimPrefix(tag, "v")
	return fmt.Sprintf(`{"tag_name":%q,"draft":%t,"prerelease":%t,"published_at":"2026-05-06T00:00:00Z","assets":[`+
		`{"name":"akam-kit-%s-%s.tar.xz"},{"name":"SHA256SUMS"},{"name":"SHA256SUMS.sig"}]}`, "firmware-"+tag, draft, pre, model, ver)
}

var errNoNet = errors.New("no network in this test")

func readAll(t *testing.T, p string) string {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func relList(items ...string) []byte { return []byte("[" + strings.Join(items, ",") + "]") }

// fwAPI serves the release list and the kit of version 1.0.0 (the one kittest builds).
type fwAPI struct {
	srv       *httptest.Server
	kb        kittest.Built
	mu        sync.Mutex
	listHits  int
	listCode  int
	failFirst int // the first N list requests are refused with 403
	list      []byte
	hits      []string // kit download paths
	query     string
	accept    string
}

func newFwAPI(t *testing.T, list []byte) *fwAPI {
	t.Helper()
	a := &fwAPI{kb: kittest.Build(t, t.TempDir(), kittest.Spec{}), listCode: 200, list: list}
	a.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		defer a.mu.Unlock()
		if r.URL.Path == "/repos/Foohx/akam-fw/releases" {
			a.listHits++
			if a.failFirst > 0 {
				a.failFirst--
				w.WriteHeader(403)
				return
			}
			a.query, a.accept = r.URL.RawQuery, r.Header.Get("Accept")
			w.WriteHeader(a.listCode)
			_, _ = w.Write(a.list)
			return
		}
		a.hits = append(a.hits, r.URL.Path)
		if !strings.HasPrefix(r.URL.Path, "/firmware-v1.0.0/") {
			http.NotFound(w, r)
			return
		}
		data, err := os.ReadFile(filepath.Join(a.kb.Dir, filepath.Base(r.URL.Path)))
		if err != nil {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write(data)
	}))
	t.Cleanup(a.srv.Close)
	return a
}

func (a *fwAPI) kitHits() int { a.mu.Lock(); defer a.mu.Unlock(); return len(a.hits) }

// useAPI points the program at the test server (list, downloads, trust list).
func useAPI(t *testing.T, e *env, a *fwAPI) {
	t.Helper()
	e.http = kittest.ClientFor(a.srv)
	t.Setenv("AKAM_API_URL_BASE", a.srv.URL)
	t.Setenv("AKAM_KIT_URL_BASE", a.srv.URL)
	f := filepath.Join(t.TempDir(), "allowed_signers")
	if err := os.WriteFile(f, a.kb.AllowedSigners, 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("AKAM_ALLOWED_SIGNERS", f)
}

// fwWorld is an Akam camera (the fake) with a verified backup, an image that was built from the kit 1.0.0, and
// the API of useAPI.
type fwWorld struct {
	*fw
	api *fwAPI
	bk  string
}

// newFwWorld: the image on record was built from the kit 1.0.0 (it is valid once that kit is verified, so it is
// not offered as "already built" before). newFwWorldBuilt: it is valid now, from the checkout's kit.
func newFwWorld(t *testing.T, list []byte, stateExtra string) *fwWorld {
	t.Helper()
	return newFwWorldKit(t, list, stateExtra, "")
}

func newFwWorldBuilt(t *testing.T, list []byte, stateExtra string) *fwWorld {
	t.Helper()
	return newFwWorldKit(t, list, stateExtra, "source")
}

func newFwWorldKit(t *testing.T, list []byte, stateExtra, imageKit string, opts ...func(*testfake.Options)) *fwWorld {
	t.Helper()
	w := &fwWorld{fw: newFlashWorld(t, testfake.FlashOptions{}, opts...), api: newFwAPI(t, list), bk: backupFixture(t)}
	useAPI(t, w.env, w.api)
	app, _ := hashOf(filepath.Join(w.bk, "mtdblock7.bin"))
	isha, _ := hashOf(w.img)
	if imageKit == "" {
		imageKit = w.api.kb.TarSHA
	}
	w.writeState("camera_ip=127.0.0.1\ncamera_kind=akam\nmode=source\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt +
		"step_backup_done=" + doneAt + "step_image_done=" + doneAt + "backup_dir=" + w.bk + "\nbackup_sha=" + app + "\n" +
		"image_path=" + w.img + "\nimage_sha=" + isha + "\nimage_kit_sha=" + imageKit + "\nimage_backup_sha=" + app + "\n" + stateExtra)
	return w
}

// plain home action 2 is "Update this camera".
func (w *fwWorld) update(in string, extra ...string) result { return w.homeRun("2\n"+in, extra...) }

func TestReleasesCommandListsNewestFirst(t *testing.T) {
	list := relList(relJSON("v0.2.0", false, false), relJSON("v0.10.0", false, false), relJSON("v1.0.0", true, false),
		relJSON("v0.3.0-rc1", false, false), relJSON("v0.11.0", false, true))
	a := newFwAPI(t, list)
	e := newEnv(t)
	useAPI(t, e, a)
	r := e.run("", "releases")
	expectRC(t, r, 0)
	contains(t, r.out, "Akam firmware releases published on GitHub (Foohx/akam-fw)", "0.10.0       2026-05-06  latest", "0.2.0        2026-05-06",
		"update --version X.Y.Z", "Prereleases are hidden")
	if strings.Index(r.out, "0.10.0") > strings.Index(r.out, "0.2.0") {
		t.Error("not newest first")
	}
	notContains(t, r.out, "1.0.0", "rc1", "0.11.0")
	if a.query != "per_page=50&page=1" || a.accept != "application/vnd.github+json" {
		t.Errorf("request: query %q accept %q", a.query, a.accept)
	}
	r = e.run("", "releases", "--include-prereleases")
	expectRC(t, r, 0)
	contains(t, r.out, "0.11.0       2026-05-06  latest  prerelease")
	notContains(t, r.out, "Prereleases are hidden")
	if a.kitHits() != 0 {
		t.Error("the list downloaded a kit")
	}
}

func TestReleasesCommandFailuresAndEmptyList(t *testing.T) {
	for _, c := range []struct {
		code int
		body string
		want string
	}{
		{403, `{"message":"rate limit"}`, "GitHub refused the request (HTTP 403"},
		{429, `{}`, "GitHub refused the request (HTTP 429"},
		{404, `{}`, "GitHub answered HTTP 404"},
		{200, `<html>captive portal</html>`, "GitHub's answer was not a list of releases"},
		{200, strings.Repeat("x", kit.MaxReleaseList+1), "GitHub's answer was not a list of releases"},
	} {
		a := newFwAPI(t, []byte(c.body))
		a.listCode = c.code
		e := newEnv(t)
		useAPI(t, e, a)
		r := e.run("", "releases")
		expectRC(t, r, 1)
		contains(t, r.out+r.err, "cannot list the releases: "+c.want, "update --kit FILE")
	}
	// offline
	e := newEnv(t)
	e.http = &kit.Client{DialContext: noNet}
	t.Setenv("AKAM_API_URL_BASE", "https://api.invalid")
	r := e.run("", "releases")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "cannot list the releases: this computer cannot reach GitHub.")
	// nothing published yet
	e = newEnv(t)
	useAPI(t, e, newFwAPI(t, relList()))
	r = e.run("", "releases")
	expectRC(t, r, 0)
	contains(t, r.out, "No release is published yet")
	// a rehearsal asks nothing
	r = e.run("", "--dry-run", "releases")
	expectRC(t, r, 0)
	contains(t, r.out, "WOULD ask GitHub")
}

// Without the test hook a test build never reaches the network, whatever the client.
func TestReleaseListNeverReachesTheNetworkInATestBuild(t *testing.T) {
	e := newEnv(t)
	t.Setenv("AKAM_API_URL_BASE", "")
	called := false
	e.http = &kit.Client{DialContext: func(_ context.Context, _, _ string) (net.Conn, error) { called = true; return nil, errNoNet }}
	r := e.run("", "releases")
	expectRC(t, r, 1)
	if called {
		t.Error("the client was used")
	}
}

func TestChooserShowsTheListAndDefaultsToTheLatest(t *testing.T) {
	// no image valid for any kit: the default is the latest release (which this server does not carry)
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false), relJSON("v2.0.0", false, false), relJSON("v0.9.0", false, false)), "fw_version=0.9.0\n")
	r := w.update("\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Which firmware?  (the camera runs: b35-2af81c5; Akam 0.9.0 installed here)",
		"Asking GitHub for the firmware releases for LSC Smart Indoor 1080p (lsc-indoor-1080p-ak3918) (read only)",
		"1) Akam 2.0.0 (latest)", "2) Akam 1.0.0  - older", "3) Akam 0.9.0  - older  - on the camera now",
		"4) A kit file or folder...", "5) An image file you built (.squashfs)...",
		"6) Stock firmware (from your backup)  - never tested on a real camera", "7) Back",
		"firmware release 2.0.0 was not found")
	if got := strings.Join(w.api.hits, " "); got != "/firmware-v2.0.0/akam-kit-lsc-indoor-1080p-ak3918-2.0.0.tar.xz" {
		t.Errorf("downloads: %s", got)
	}
	w.unchanged(t)
	if w.readState("fw_source") != "" {
		t.Error("a firmware that was never written was recorded")
	}
}

// This computer cannot build an image (no --builder native) and one is already built and valid: that image is
// the first row and the default, as before the chooser existed; the releases follow.
func TestBuiltImageIsTheDefaultWhenNoBuilderIsAvailable(t *testing.T) {
	w := newFwWorldBuilt(t, relList(relJSON("v2.0.0", false, false), relJSON("v1.0.0", false, false)), "")
	r := w.update("\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "1) The image already built from your backup (", "2) Akam 2.0.0 (latest)", "3) Akam 1.0.0  - older",
		"Updating: WRITES THE CAMERA FLASH", "Image md5 written and read back: "+w.imgMD5)
	if w.api.kitHits() != 0 {
		t.Error("Enter downloaded a kit although an image was ready")
	}
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) || w.readState("fw_source") != "image:"+w.img {
		t.Errorf("source %q", w.readState("fw_source"))
	}
}

// The latest release needs an image that cannot be built here: the kit is verified, the choice is refused with
// an update-specific message, and NOTHING of the previous kit, kit state or image is touched.
func TestReleaseWithoutAnImageLeavesTheStateUntouched(t *testing.T) {
	w := newFwWorldKit(t, relList(relJSON("v1.0.0", false, false)), "", "0000000000000000000000000000000000000000000000000000000000000000")
	// a previous kit folder that must survive
	kitDir := filepath.Join(w.state, "kit")
	if err := os.MkdirAll(kitDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(kitDir, "MARK"), []byte("previous kit"), 0o600); err != nil {
		t.Fatal(err)
	}
	keys := []string{"mode", "kit_path", "kit_sha", "kit_format", "kit_unsigned_ok", "kit_version", "step_get_kit_done", "image_path", "image_sha", "image_kit_sha", "step_image_done"}
	snap := func() string {
		var out []string
		for _, k := range keys {
			out = append(out, k+"="+w.readState(k))
		}
		return strings.Join(out, "\n")
	}
	before := snap()
	r := w.update("1\ny\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Kit verified", "Akam 1.0.0 is verified, but this program cannot build its image on this computer",
		"the image for this kit cannot be built here: nothing was changed", "get-kit --version 1.0.0", "step image, then update again")
	notContains(t, r.out+r.err, "choose 'Set up a new camera'", "WRITES THE CAMERA FLASH")
	w.unchanged(t)
	if got := snap(); got != before {
		t.Errorf("the state changed:\n--- before ---\n%s\n--- after ---\n%s", before, got)
	}
	if b, err := os.ReadFile(filepath.Join(kitDir, "MARK")); err != nil || string(b) != "previous kit" {
		t.Errorf("the previous kit folder is gone: %v", err)
	}
	if _, err := os.Stat(kitDir + ".keep"); err == nil {
		t.Error("a leftover kit.keep")
	}
}

// A kit that is already recorded with the same version is not trusted: the release is downloaded and verified
// again, whatever was recorded (even a local unsigned kit).
func TestRecordedKitOfTheSameVersionIsNeverReused(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	w.writeState(readAll(t, filepath.Join(w.state, "state")) + "mode=kit\nkit_version=1.0.0\nkit_unsigned_ok=1\nkit_path=/somewhere/else/akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz\n")
	r := w.update("1\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "ok: signature valid", "Kit verified")
	if w.api.kitHits() != 3 {
		t.Errorf("downloads %v", w.api.hits)
	}
	if w.readState("kit_unsigned_ok") != "0" || w.readState("fw_source") != "release:1.0.0" {
		t.Errorf("unsigned %q source %q", w.readState("kit_unsigned_ok"), w.readState("fw_source"))
	}
}

func TestChooserBackWritesNothing(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	// 1 release, kit file, image file, stock, Back = 5
	r := w.update("5\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "No firmware chosen. Nothing was changed.")
	if w.api.kitHits() != 0 {
		t.Error("something was downloaded")
	}
	w.unchanged(t)
	for _, in := range []string{"", "x\n"} { // no answer at all
		r = w.update(in)
		expectRC(t, r, 0)
		w.unchanged(t)
	}
}

func TestReleaseListIsAskedOncePerSession(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	// update, Back, update, Back, Quit
	r := w.homeRun("2\n5\n2\n5\n10\n")
	expectRC(t, r, 0)
	if w.api.listHits != 1 {
		t.Errorf("the list was asked %d times", w.api.listHits)
	}
}

func TestChooserOfflineFallsBackToLocalChoices(t *testing.T) {
	for name, setup := range map[string]func(w *fwWorld){
		"403":     func(w *fwWorld) { w.api.listCode = 403 },
		"429":     func(w *fwWorld) { w.api.listCode = 429 },
		"garbage": func(w *fwWorld) { w.api.list = []byte("{{{") },
		"offline": func(w *fwWorld) { w.http = &kit.Client{DialContext: noNet} },
	} {
		w := newFwWorldBuilt(t, relList(relJSON("v1.0.0", false, false)), "")
		setup(w)
		// built image, kit file, image file, stock, Back
		r := w.update("5\n10\n")
		expectRC(t, r, 0)
		contains(t, r.out+r.err, "could not read the list of releases: ", "Kits and images on this computer can still be chosen.",
			"1) The image already built from your backup (", "2) A kit file or folder...", "No firmware chosen.")
		notContains(t, r.out+r.err, "(latest)")
		if t.Failed() {
			t.Fatal(name)
		}
	}
}

func TestChooserNoReleasePublishedYet(t *testing.T) {
	w := newFwWorldBuilt(t, relList(), "")
	r := w.update("5\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "No Akam firmware release is published on GitHub for this camera model yet", "1) The image already built from your backup (")
}

// Choosing the release of the kit that the image was built from: downloaded, verified by signature, the image
// is the one on record, then the red box; the source is recorded once the write is verified.
func TestUpdateWithAReleaseWritesAndRecordsIt(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "fw_version=0.5.0\n")
	r := w.update("1\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Which firmware?", "1) Akam 1.0.0 (latest)", "ok: signature valid", "Kit verified (format 1,",
		"Updating: WRITES THE CAMERA FLASH", "Image md5 written and read back: "+w.imgMD5)
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Fatal("the image was not written")
	}
	if got := strings.Join(w.api.hits, " "); got != "/firmware-v1.0.0/akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz /firmware-v1.0.0/SHA256SUMS /firmware-v1.0.0/SHA256SUMS.sig" {
		t.Errorf("downloads: %s", got)
	}
	if w.readState("fw_source") != "release:1.0.0" || w.readState("fw_version") != "1.0.0" || w.readState("fw_choice") != "" {
		t.Errorf("state: source %q version %q choice %q", w.readState("fw_source"), w.readState("fw_version"), w.readState("fw_choice"))
	}
	r = w.run("", "--camera", "default", "status")
	contains(t, r.out, "Firmware source: release:1.0.0")
	w.checkClosedSet(t)
}

// The red box still defaults to No: a chosen release is downloaded and verified, nothing is written.
func TestUpdateWithAReleaseNeedsTheRedBoxYes(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	r := w.update("1\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "not confirmed.", "nothing was written.")
	w.unchanged(t)
	if w.readState("fw_source") != "" {
		t.Error("recorded without a write")
	}
}

// The signature is checked whatever the list says: a list that names a version whose kit is tampered with
// does not get that kit used.
func TestReleaseKitIsVerifiedWhateverTheListSays(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	if err := os.WriteFile(filepath.Join(w.api.kb.Dir, "akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz"), []byte("not the signed kit"), 0o600); err != nil {
		t.Fatal(err)
	}
	r := w.update("1\ny\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "the kit failed verification")
	notContains(t, r.out+r.err, "WRITES THE CAMERA FLASH")
	w.unchanged(t)
}

func TestDowngradeNeedsAnExplicitYes(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v2.0.0", false, false), relJSON("v1.0.0", false, false)), "fw_version=2.0.0\n")
	// the camera runs 2.0.0: choosing 1.0.0 (item 2) asks; Enter = No goes back to the list; then Back
	r := w.update("2\n\n6\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "WARNING: Akam 1.0.0 is OLDER than the 2.0.0 on this camera: this is a downgrade.",
		"Settings files saved by the newer version may not be understood by the older one. Your backup stays valid.",
		"Go back to Akam 1.0.0? [y/N]", "No firmware chosen.")
	if w.api.kitHits() != 0 {
		t.Error("a refused downgrade downloaded the kit")
	}
	w.unchanged(t)
	// yes: it goes on (download, verify, image, red box answered No)
	w = newFwWorld(t, relList(relJSON("v2.0.0", false, false), relJSON("v1.0.0", false, false)), "fw_version=2.0.0\n")
	r = w.update("2\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "this is a downgrade.", "Go back to Akam 1.0.0? [y/N]", "Kit verified", "not confirmed.")
	if w.api.kitHits() != 3 {
		t.Errorf("hits %v", w.api.hits)
	}
	w.unchanged(t)
	// the latest or the same version is not a downgrade
	w = newFwWorld(t, relList(relJSON("v2.0.0", false, false)), "fw_version=2.0.0\n")
	r = w.update("1\n\n10\n")
	notContains(t, r.out+r.err, "downgrade")
}

func TestDowngradeOnTheCommandLineAsksAndRefusesWithoutAnAnswer(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "fw_version=2.0.0\n")
	r := w.run("", "--camera", "default", "update", "--version", "1.0.0")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this is a downgrade.", "downgrade not confirmed.")
	if w.api.kitHits() != 0 {
		t.Error("downloaded")
	}
	w.unchanged(t)
	r = w.run("y\n\n", "--camera", "default", "update", "--version", "v1.0.0")
	expectRC(t, r, 1) // the red box is answered No
	contains(t, r.out+r.err, "Kit verified", "not confirmed.")
	w.unchanged(t)
}

func TestUpdateVersionCommandRecordsAfterTheWrite(t *testing.T) {
	w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
	r := w.run("y\n", "--camera", "default", "update", "--version", "1.0.0")
	expectRC(t, r, 0)
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Fatal("not written")
	}
	if w.readState("fw_source") != "release:1.0.0" {
		t.Errorf("fw_source %q", w.readState("fw_source"))
	}
}

// A kit that was downloaded earlier is listed even without any network, and is verified again when used.
func TestDownloadedKitIsListedAndVerifiedAgain(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	w.http = &kit.Client{DialContext: noNet}
	dl := filepath.Join(w.state, "downloads", "1.0.0")
	if err := os.MkdirAll(dl, 0o700); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"} {
		b, _ := os.ReadFile(filepath.Join(w.api.kb.Dir, f))
		if err := os.WriteFile(filepath.Join(dl, f), b, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	// also junk that must not be listed
	_ = os.MkdirAll(filepath.Join(w.state, "downloads", "9.9.9"), 0o700)
	_ = os.MkdirAll(filepath.Join(w.state, "downloads", "bad name"), 0o700)
	r := w.update("1\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "1) Kit already downloaded: 1.0.0", "ok: signature valid", "Kit verified", "Updating: WRITES THE CAMERA FLASH")
	notContains(t, r.out+r.err, "9.9.9", "bad name")
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) {
		t.Fatal("not written")
	}
	// a kit kept in the downloads folder IS the download of that release: the recorded source is a fact
	if got := w.readState("fw_source"); got != "release:1.0.0" {
		t.Errorf("fw_source %q", got)
	}
	// tampered after the download: refused, nothing written
	w = newFwWorld(t, relList(), "")
	w.http = &kit.Client{DialContext: noNet}
	dl = filepath.Join(w.state, "downloads", "1.0.0")
	_ = os.MkdirAll(dl, 0o700)
	for _, f := range []string{"akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"} {
		b, _ := os.ReadFile(filepath.Join(w.api.kb.Dir, f))
		if f == "akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz" {
			b = append(b, 'x')
		}
		_ = os.WriteFile(filepath.Join(dl, f), b, 0o600)
	}
	r = w.update("1\ny\n10\n")
	contains(t, r.out+r.err, "the kit failed verification")
	w.unchanged(t)
}

func TestKitFileFromThePathPrompt(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	kitFile := filepath.Join(w.api.kb.Dir, "akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz")
	// items: kit file(1), image file, stock, Back
	r := w.update("1\n" + kitFile + "\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Path of the kit (.tar.xz file or folder; empty: go back)", "ok: signature valid", "Kit verified", "WRITES THE CAMERA FLASH")
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) || w.readState("fw_source") != "kit:"+kitFile {
		t.Errorf("source %q", w.readState("fw_source"))
	}
	// a path that does not exist, then an empty one: back to the list
	w = newFwWorld(t, relList(), "")
	r = w.update("1\n/no/such/kit.tar.xz\n4\n10\n")
	contains(t, r.out+r.err, "no such file or folder: /no/such/kit.tar.xz", "No firmware chosen.")
	w.unchanged(t)
}

func TestKitFlagOnTheCommandLine(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	kitFile := filepath.Join(w.api.kb.Dir, "akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz")
	r := w.run("n\n", "--camera", "default", "update", "--kit", kitFile)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "Kit verified", "Updating: WRITES THE CAMERA FLASH", "not confirmed.")
	w.unchanged(t)
	// not a kit at all
	bad := filepath.Join(t.TempDir(), "kit.tar.xz")
	_ = os.WriteFile(bad, []byte("junk"), 0o600)
	r = w.run("y\n", "--camera", "default", "update", "--kit", bad)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the kit failed verification")
	w.unchanged(t)
}

func TestImageFileFromThePrompt(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	mine := filepath.Join(t.TempDir(), "mine.squashfs")
	if err := os.WriteFile(mine, w.imgData, 0o600); err != nil {
		t.Fatal(err)
	}
	sum := md5.Sum(w.imgData)
	want := hex.EncodeToString(sum[:])
	// items: kit file, image file(2), stock, Back(4). Enter at the question = No: not used, back to the list.
	r := w.update("2\n" + mine + "\n\n4\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Path of the image (.squashfs, exactly 5242880 bytes; empty: go back)",
		"md5 "+want+".", "WARNING: this image was not built by this program: you take responsibility for what it contains.",
		"Use this image? [y/N]", "Not used.", "No firmware chosen.")
	w.unchanged(t)
	// yes, then the red box: No -> nothing written
	w = newFwWorld(t, relList(), "")
	r = w.update("2\n" + mine + "\ny\n\n10\n")
	contains(t, r.out+r.err, "Image structure checked", "Updating: WRITES THE CAMERA FLASH", "not confirmed.")
	w.unchanged(t)
	if w.readState("fw_source") != "" {
		t.Error("recorded without a write")
	}
	// yes and yes: written, recorded
	w = newFwWorld(t, relList(), "")
	r = w.update("2\n" + mine + "\ny\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Image md5 written and read back: "+want)
	if !bytes.Equal(w.cam.Mtd7(), w.imgData) || w.readState("fw_source") != "image:"+mine {
		t.Errorf("source %q", w.readState("fw_source"))
	}
}

func TestImageFileRefusals(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	small := filepath.Join(t.TempDir(), "small.squashfs")
	_ = os.WriteFile(small, []byte("far too small"), 0o600)
	junk := filepath.Join(t.TempDir(), "junk.squashfs")
	_ = os.WriteFile(junk, make([]byte, 5242880), 0o600) // the right size, not a squashfs
	// wrong size: refused before any question; not a squashfs: refused by the structure check; a folder
	r := w.update("2\n" + small + "\n2\n" + t.TempDir() + "\n2\n" + junk + "\ny\n10\n")
	contains(t, r.out+r.err, "that file is 13 bytes; an image is exactly 5242880. It is refused.", "no such file: ",
		"this file is not a valid squashfs image")
	notContains(t, r.out+r.err, "WRITES THE CAMERA FLASH")
	w.unchanged(t)
}

func TestImageFlagOnTheCommandLineIsUnchanged(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	r := w.update("y\n\n10\n", "--image", w.img)
	expectRC(t, r, 0)
	notContains(t, r.out+r.err, "Which firmware?", "you take responsibility")
	if w.api.listHits != 0 {
		t.Error("the list was asked although a firmware was named")
	}
	if w.readState("fw_source") != "image:"+w.img {
		t.Errorf("source %q", w.readState("fw_source"))
	}
}

func TestStockOptionOnlyWithAVerifiedStockBackup(t *testing.T) {
	// no recorded backup
	w := newFwWorld(t, relList(), "")
	w.writeState(strings.ReplaceAll(readAll(t, filepath.Join(w.state, "state")), "backup_dir="+w.bk+"\n", ""))
	r := w.update("9\n10\n")
	notContains(t, r.out+r.err, "Stock firmware (from your backup)")
	// a damaged backup
	w = newFwWorld(t, relList(), "")
	if err := os.WriteFile(filepath.Join(w.bk, "mtdblock3.bin"), []byte("rotten"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = w.update("9\n10\n")
	notContains(t, r.out+r.err, "Stock firmware (from your backup)")
	// a backup of a camera that ran Akam is not the stock firmware
	w = newFwWorld(t, relList(), "")
	if err := os.WriteFile(filepath.Join(w.bk, "camera_info.txt"), []byte("kind=akam\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = w.update("9\n10\n")
	notContains(t, r.out+r.err, "Stock firmware (from your backup)")
	// a verified one: offered, and never in the first install
	w = newFwWorld(t, relList(), "")
	r = w.update("9\n10\n")
	contains(t, r.out+r.err, "Stock firmware (from your backup)  - never tested on a real camera")
}

// The stock option is the restore flow, with its gates: the stock APP is the one in the backup, the red box
// defaults to No, and a refused box writes nothing.
func TestStockChoiceGoesThroughTheRestoreFlow(t *testing.T) {
	// items: kit file, image file, stock(3), Back
	w := newFwWorld(t, relList(), "")
	r := w.update("3\ny\n\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "WARNING: the stock firmware restore has never been tried on a real camera.",
		"Restore the original (stock) APP from your backup", "Stock APP sha256: ", "Restoring: WRITES THE CAMERA FLASH", "not confirmed.")
	w.unchanged(t)
	if w.readState("fw_source") != "" {
		t.Error("recorded without a write")
	}
	// Enter at the route question is No too: the maintenance telnet is not reachable here, nothing is written
	w = newFwWorld(t, relList(), "")
	r = w.update("3\n\n\n10\n")
	w.unchanged(t)
	// yes: restored over SSH, recorded
	w = newFwWorld(t, relList(), "")
	r = w.update("3\ny\ny\n10\n")
	expectRC(t, r, 0)
	contains(t, r.out+r.err, "Restore written and verified")
	app, _ := os.ReadFile(filepath.Join(w.bk, "mtdblock7.bin"))
	if !bytes.Equal(w.cam.Mtd7(), app) {
		t.Fatal("the stock APP was not written")
	}
	if w.readState("fw_source") != "stock" || w.readState("fw_version") != "" {
		t.Errorf("source %q", w.readState("fw_source"))
	}
}

func TestStockFlagAndItsGates(t *testing.T) {
	w := newFwWorld(t, relList(), "")
	r := w.run("y\n\n", "--camera", "default", "update", "--stock")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "Restore the original (stock) APP from your backup", "Restoring: WRITES THE CAMERA FLASH", "not confirmed.")
	w.unchanged(t)
	if w.api.listHits != 0 {
		t.Error("asked the list")
	}
	// no stock backup: the restore refuses, nothing is written
	bad := newFwWorld(t, relList(), "")
	if err := os.WriteFile(filepath.Join(bad.bk, "mtdblock3.bin"), []byte("rotten"), 0o600); err != nil {
		t.Fatal(err)
	}
	r = bad.run("y\ny\n", "--camera", "default", "update", "--stock")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "the backup does not verify:")
	bad.unchanged(t)
	// the Akam-kind backup is refused too
	ak := newFwWorld(t, relList(), "")
	_ = os.WriteFile(filepath.Join(ak.bk, "camera_info.txt"), []byte("kind=akam\n"), 0o600)
	r = ak.run("y\ny\n", "--camera", "default", "update", "--stock")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "kind=akam")
	ak.unchanged(t)
	// the menu with --stock goes the same way
	m := newFwWorld(t, relList(), "")
	r = m.homeRun("2\ny\n\n10\n", "--stock")
	contains(t, r.out+r.err, "Restoring: WRITES THE CAMERA FLASH", "not confirmed.")
	notContains(t, r.out+r.err, "Which firmware?")
	m.unchanged(t)
}

// A rehearsal of every command-line choice sends nothing, downloads nothing and asks nothing.
func TestRehearsalOfTheFirmwareChoices(t *testing.T) {
	for _, args := range [][]string{
		{"update", "--version", "1.0.0"}, {"update", "--kit", "/no/such/kit.tar.xz"}, {"update", "--stock"}, {"update", "--image", "/no/such.squashfs"},
	} {
		w := newFwWorld(t, relList(relJSON("v1.0.0", false, false)), "")
		r := w.run("", append([]string{"--camera", "default", "--dry-run"}, args...)...)
		expectRC(t, r, 0)
		if w.api.listHits != 0 || w.api.kitHits() != 0 {
			t.Errorf("%v: the network was used", args)
		}
		if len(w.cam.Commands()) != 0 {
			t.Errorf("%v: sent %q", args, w.cam.Commands())
		}
		w.unchanged(t)
		if w.readState("fw_source") != "" || w.readState("fw_choice") != "" {
			t.Errorf("%v: the real state was changed", args)
		}
	}
}

func TestStockFlagUsage(t *testing.T) {
	e := newEnv(t)
	for _, c := range []struct {
		args []string
		want string
	}{
		{[]string{"--stock", "--kit", "x", "update"}, "akam: --stock cannot be combined with --kit, --version or --image: choose one firmware (see ./akam help)"},
		{[]string{"--stock", "--version", "1.0.0", "update"}, "--stock cannot be combined"},
		{[]string{"--stock", "--image", "x", "update"}, "--stock cannot be combined"},
		{[]string{"--stock", "restore"}, "akam: --stock is for the update command (see ./akam help)"},
		{[]string{"--stock", "get-kit"}, "akam: --stock is for the update command"},
	} {
		r := e.runRaw("", append([]string{"--state-dir", e.state}, c.args...)...)
		expectRC(t, r, 2)
		contains(t, r.err, c.want)
	}
}

// The first install offers the releases and the local choices but never the stock firmware, and the choice
// feeds the get-kit step.
func TestInstallChooserOffersNoStockAndFeedsGetKit(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nstep_doctor_done=" + doneAt)
	// item 1 = Akam 1.0.0; the flow then stops later (no input)
	r := e.run("1\n1\n", "--camera", "default")
	contains(t, r.out+r.err, "Which firmware?", "1) Akam 1.0.0 (latest)", "2) A kit file or folder...", "3) An image file you built (.squashfs)...", "4) Back")
	notContains(t, r.out+r.err, "Stock firmware")
	if got := strings.Join(a.hits, " "); !strings.HasPrefix(got, "/firmware-v1.0.0/akam-kit-lsc-indoor-1080p-ak3918-1.0.0.tar.xz") {
		t.Errorf("downloads: %q", got)
	}
	if e.readState("fw_source") != "" {
		t.Errorf("source %q recorded before any write", e.readState("fw_source"))
	}
	// Back: nothing happens
	a2 := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e2 := newEnv(t)
	useAPI(t, e2, a2)
	e2.writeState("camera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\n")
	r = e2.run("1\n4\n10\n", "--camera", "default")
	contains(t, r.out+r.err, "No firmware chosen. Nothing was changed.")
	if a2.kitHits() != 0 || e2.readState("step_get_kit_done") != "" {
		t.Error("something ran after Back")
	}
	// --stock belongs to the update
	r = e2.run("1\n10\n", "--camera", "default", "--stock")
	contains(t, r.out+r.err, "--stock puts the original firmware back on an Akam camera: it belongs to the update.")
}

func TestInstallWithAnImageChoiceAsksForAKit(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("camera_ip=127.0.0.1\ncamera_kind=stock\ncamera_telnet=1\nstep_doctor_done=" + doneAt)
	img := filepath.Join(t.TempDir(), "mine.squashfs")
	_ = os.WriteFile(img, validImage(t), 0o600)
	// 1 install; 3 = an image file; path; y = I take responsibility; then the kit chooser: 1 = Akam 1.0.0
	r := e.run("1\n3\n"+img+"\ny\n1\n", "--camera", "default")
	contains(t, r.out+r.err, "The image is written with the flash tools of a kit.", "Which kit? (it supplies the flash tools)", "1) Akam 1.0.0 (latest)")
	if n := strings.Count(r.out+r.err, "An image file you built"); n != 1 {
		t.Errorf("the kit chooser offers an image again (%d)", n)
	}
	if a.kitHits() == 0 {
		t.Error("the kit was not downloaded")
	}
}

// A source checkout has its own kit: the first install shows no chooser there.
func TestInstallInSourceModeShowsNoChooser(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("mode=source\ncamera_ip=127.0.0.1\ncamera_kind=stock\nstep_doctor_done=" + doneAt + "step_get_kit_done=" + doneAt)
	r := e.run("1\n10\n", "--camera", "default")
	notContains(t, r.out+r.err, "Which firmware?")
	if a.listHits != 0 {
		t.Error("the list was asked")
	}
}

func TestChoiceLabelsForTheFullScreenList(t *testing.T) {
	labels := cli.FirmwareLabels(true, "1.5.0", []string{"2.0.0", "1.5.0", "1.0.0"}, []string{"1.0.0"}, "/x/ak_rtsp_firmware.squashfs", true)
	want := []string{
		"Akam 2.0.0 (latest)", "Akam 1.5.0  - older  - on the camera now", "Akam 1.0.0  - older", "Kit already downloaded: 1.0.0",
		"The image already built from your backup (ak_rtsp_firmware.squashfs)", "A kit file or folder...",
		"An image file you built (.squashfs)...", "Stock firmware (from your backup)  - never tested on a real camera", "Back",
	}
	if strings.Join(labels, "\n") != strings.Join(want, "\n") {
		t.Errorf("labels:\n%s", strings.Join(labels, "\n"))
	}
	if l := cli.FirmwareLabels(false, "", []string{"1.0.0"}, nil, "", true); strings.Contains(strings.Join(l, "|"), "Stock") || strings.Contains(strings.Join(l, "|"), "already built") {
		t.Errorf("install list: %v", l)
	}
	// no builder here and an image already built: it comes first (the default); a failed list adds "Try again"
	l0 := cli.FirmwareLabels(true, "", []string{"2.0.0"}, nil, "/x/ak.squashfs", false, true, true)
	if l0[0] != "The image already built from your backup (ak.squashfs)" || l0[1] != "Akam 2.0.0 (latest)" || l0[len(l0)-2] != "Try again to read the releases" {
		t.Errorf("builtFirst/retry: %v", l0)
	}
	many := make([]string, 20)
	for i := range many {
		many[i] = fmt.Sprintf("1.%d.0", 20-i)
	}
	l := cli.FirmwareLabels(true, "", many, nil, "", false)
	if len(l) != 1+12+1+2+1 || !strings.Contains(l[13], "7 older releases not shown") {
		t.Errorf("%d labels: %v", len(l), l)
	}
}
