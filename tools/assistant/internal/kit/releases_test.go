package kit_test

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

// rel builds one release object of the API answer (synthetic data only).
func rel(tag string, draft, pre bool, assets ...string) string {
	var as []string
	for _, a := range assets {
		as = append(as, fmt.Sprintf(`{"name":%q}`, a))
	}
	return fmt.Sprintf(`{"tag_name":%q,"name":"Release %s","draft":%t,"prerelease":%t,"published_at":"2026-03-04T05:06:07Z","assets":[%s]}`,
		tag, tag, draft, pre, strings.Join(as, ","))
}

const model = "lsc-indoor-1080p-ak3918"

func full(ver string) []string {
	return []string{"akam-kit-" + model + "-" + ver + ".tar.xz", "SHA256SUMS", "SHA256SUMS.sig"}
}

func list(items ...string) []byte { return []byte("[" + strings.Join(items, ",") + "]") }

func versions(rs []kit.Release) string {
	var v []string
	for _, r := range rs {
		v = append(v, r.Version)
	}
	return strings.Join(v, " ")
}

func TestParseReleasesKeepsOnlyUsableOnesNewestFirst(t *testing.T) {
	data := list(
		rel("firmware-v0.2.0", false, false, full("0.2.0")...),
		rel("firmware-v0.10.0", false, false, full("0.10.0")...),
		rel("firmware-v0.9.1", false, false, full("0.9.1")...),
		rel("firmware-v1.0.0", true, false, full("1.0.0")...),                                                           // draft
		rel("firmware-v0.3.0-rc1", false, false, "akam-kit-"+model+"-0.3.0-rc1.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"), // not plain semver
		rel("firmware-0.4.0", false, false, full("0.4.0")...),                                                           // no v
		rel("v0.8.0", false, false, full("0.8.0")...),                                                                   // old single-tag release
		rel("assistant-v0.8.0", false, false, full("0.8.0")...),                                                         // an assistant release
		rel("firmware-v0.8.1", false, false, "akam-kit-other-model-0.8.1.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"),       // no kit for this model
		rel("firmware-v01.0.0", false, false, full("01.0.0")...),                                                        // leading zero
		rel("firmware-v0.5.0", false, false, "akam-kit-"+model+"-0.5.0.tar.xz", "SHA256SUMS"),                           // no signature
		rel("firmware-v0.6.0", false, false, "akam-kit-"+model+"-0.6.1.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"),         // kit of another version
		rel("firmware-v0.7.0", false, false),                                                                            // no assets
		rel("firmware-v0.2.0", false, false, full("0.2.0")...),                                                          // duplicate
		rel("firmware-v0.1.0", false, true, full("0.1.0")...),                                                           // prerelease
		`{"tag_name":"v9.9.9"}`, // an object that is not a release list entry is simply skipped
		`"text"`, `42`, `null`,
	)
	got, err := kit.ParseReleases(data, false, model)
	if err != nil {
		t.Fatal(err)
	}
	if v := versions(got); v != "0.10.0 0.9.1 0.2.0" {
		t.Errorf("without prereleases: %q", v)
	}
	if got[0].Tag != "firmware-v0.10.0" || got[0].Published != "2026-03-04" || got[0].Prerelease {
		t.Errorf("fields: %+v", got[0])
	}
	got, err = kit.ParseReleases(data, true, model)
	if err != nil {
		t.Fatal(err)
	}
	if v := versions(got); v != "0.10.0 0.9.1 0.2.0 0.1.0" {
		t.Errorf("with prereleases: %q", v)
	}
	if !got[3].Prerelease {
		t.Error("the prerelease flag is lost")
	}
}

func TestParseReleasesRefusesGarbage(t *testing.T) {
	for name, in := range map[string]string{
		"empty": "", "text": "<html>rate limit</html>", "object": `{"message":"Not Found"}`,
		"truncated": `[{"tag_name":"firmware-v1.0.0"`, "number": "7",
	} {
		if _, err := kit.ParseReleases([]byte(in), false, model); !errors.Is(err, kit.ErrBadReleaseList) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if _, err := kit.ParseReleases(make([]byte, kit.MaxReleaseList+1), false, model); !errors.Is(err, kit.ErrTooLarge) {
		t.Errorf("oversized: %v", err)
	}
	if got, err := kit.ParseReleases([]byte("[]"), false, model); err != nil || len(got) != 0 {
		t.Errorf("empty list: %v %v", got, err)
	}
}

// A hostile name or date never reaches the user: only the version (strict rule) and a YYYY-MM-DD date do.
func TestParseReleasesShowsNothingElseOfTheAnswer(t *testing.T) {
	data := []byte(`[{"tag_name":"firmware-v1.2.3","name":"\u001b[31mevil","published_at":"\u001b[2Jnot a date","assets":[` +
		`{"name":"akam-kit-lsc-indoor-1080p-ak3918-1.2.3.tar.xz"},{"name":"SHA256SUMS"},{"name":"SHA256SUMS.sig"}]}]`)
	got, err := kit.ParseReleases(data, false, model)
	if err != nil || len(got) != 1 {
		t.Fatal(got, err)
	}
	if got[0].Published != "" || got[0].Version != "1.2.3" {
		t.Errorf("%+v", got[0])
	}
}

func TestVersionHelpers(t *testing.T) {
	for _, c := range []struct {
		a, b string
		want int
		ok   bool
	}{
		{"0.1.0", "0.2.0", -1, true}, {"1.0.0", "0.99.99", 1, true}, {"v1.2.3", "1.2.3", 0, true},
		{"0.10.0", "0.9.0", 1, true}, {"b35-2af81c5", "0.1.0", 0, false}, {"1.0", "1.0.0", 0, false},
		{"1.0.0-rc1", "1.0.0", 0, false},
	} {
		got, ok := kit.CompareVersions(c.a, c.b)
		if got != c.want || ok != c.ok {
			t.Errorf("%s vs %s: %d %v", c.a, c.b, got, ok)
		}
	}
	for in, want := range map[string]string{
		"0.1.0": "0.1.0", "akam-0.2.3-4-gabc": "0.2.3", "b35-2af81c5": "", "v1.2": "", "build 10.20.30 x": "10.20.30", "1.2.3.4": "",
	} {
		if got, _ := kit.ExtractVersion(in); got != want {
			t.Errorf("ExtractVersion(%q) = %q, want %q", in, got, want)
		}
	}
}

type apiServer struct {
	srv  *httptest.Server
	mu   sync.Mutex
	reqs []*http.Request
}

func newAPI(t *testing.T, code int, body []byte) *apiServer {
	t.Helper()
	s := &apiServer{}
	s.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.reqs = append(s.reqs, r)
		s.mu.Unlock()
		w.WriteHeader(code)
		_, _ = w.Write(body)
	}))
	t.Cleanup(s.srv.Close)
	return s
}

func TestListReleasesAsksTheAPIAndParses(t *testing.T) {
	s := newAPI(t, 200, list(rel("firmware-v0.1.0", false, false, full("0.1.0")...)))
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model)
	if err != nil || versions(got) != "0.1.0" {
		t.Fatal(got, err)
	}
	r := s.reqs[0]
	if r.URL.Path != "/repos/Foohx/akam-fw/releases" || r.URL.Query().Get("per_page") != "50" || r.URL.Query().Get("page") != "1" || r.Method != "GET" {
		t.Errorf("request %s %s", r.Method, r.URL)
	}
	if r.Header.Get("Accept") != "application/vnd.github+json" || r.Header.Get("User-Agent") != "akam" {
		t.Errorf("headers %v", r.Header)
	}
}

func TestListReleasesErrors(t *testing.T) {
	for _, code := range []int{403, 429, 404, 500} {
		s := newAPI(t, code, []byte(`{"message":"API rate limit exceeded"}`))
		_, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model)
		var he *kit.HTTPError
		if !errors.As(err, &he) || he.Code != code {
			t.Errorf("%d: %v", code, err)
		}
		want := 1
		if code >= 500 {
			want = 2 // a server failure is tried twice, a refusal never twice
		}
		if len(s.reqs) != want {
			t.Errorf("%d: %d requests, want %d", code, len(s.reqs), want)
		}
	}
	s := newAPI(t, 200, []byte(strings.Repeat("x", kit.MaxReleaseList+10)))
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model); !errors.Is(err, kit.ErrTooLarge) {
		t.Errorf("oversized answer: %v", err)
	}
	s = newAPI(t, 200, []byte("not json"))
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model); !errors.Is(err, kit.ErrBadReleaseList) {
		t.Errorf("garbage answer: %v", err)
	}
	if _, err := kit.ListReleases(context.Background(), nil, "http://api.example/", "Foohx/akam-fw", false, model); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("plain http: %v", err)
	}
}

// A redirect to plain http is refused, as for the kit downloads.
func TestListReleasesRefusesAnHTTPRedirect(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://example.invalid/x", http.StatusFound)
	}))
	defer srv.Close()
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(srv), srv.URL, "Foohx/akam-fw", false, model); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("redirect: %v", err)
	}
}

func TestParseReleasesIsPerModel(t *testing.T) {
	other := "akam-kit-other-model-0.2.0.tar.xz"
	data := list(
		rel("firmware-v0.1.0", false, false, full("0.1.0")...),
		rel("firmware-v0.2.0", false, false, other, "SHA256SUMS", "SHA256SUMS.sig"),
	)
	got, err := kit.ParseReleases(data, false, model)
	if err != nil || versions(got) != "0.1.0" {
		t.Errorf("model of the camera: %v %v", versions(got), err)
	}
	got, err = kit.ParseReleases(data, false, "other-model")
	if err != nil || versions(got) != "0.2.0" {
		t.Errorf("another model: %v %v", versions(got), err)
	}
	// a model dropped from a newer release just does not see it
	got, _ = kit.ParseReleases(list(rel("firmware-v0.1.0", false, false, full("0.1.0")...)), false, "other-model")
	if len(got) != 0 {
		t.Errorf("a release without a kit for the model must not be listed: %v", got)
	}
	for _, bad := range []string{"", "Bad_Model", "ab", "-x-", "a--b", strings.Repeat("a", 41)} {
		if _, err := kit.ParseReleases(data, false, bad); !errors.Is(err, kit.ErrBadModel) {
			t.Errorf("model %q: %v", bad, err)
		}
	}
	if kit.ReleaseTag("0.1.0") != "firmware-v0.1.0" || kit.KitFile(model, "0.1.0") != "akam-kit-lsc-indoor-1080p-ak3918-0.1.0.tar.xz" {
		t.Error("tag and file names")
	}
}

// pagedAPI serves one JSON page per request, by the page= parameter; an unknown page is an empty list.
func pagedAPI(t *testing.T, pages ...[]byte) *apiServer {
	t.Helper()
	s := &apiServer{}
	s.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.reqs = append(s.reqs, r)
		s.mu.Unlock()
		p, _ := strconv.Atoi(r.URL.Query().Get("page"))
		if p >= 1 && p <= len(pages) {
			_, _ = w.Write(pages[p-1])
			return
		}
		_, _ = w.Write([]byte("[]"))
	}))
	t.Cleanup(s.srv.Close)
	return s
}

// fullPage is ReleasesPerPage assistant releases (never usable as a kit) with an optional firmware release last.
func fullPage(extra ...string) []byte {
	var items []string
	for i := 0; i < kit.ReleasesPerPage-len(extra); i++ {
		items = append(items, rel(fmt.Sprintf("assistant-v1.0.%d", i), false, false, "SHA256SUMS"))
	}
	items = append(items, extra...)
	return list(items...)
}

func TestListReleasesFollowsThePages(t *testing.T) {
	// page 1: only assistant releases; the firmware releases are behind them
	s := pagedAPI(t, fullPage(), fullPage(rel("firmware-v0.2.0", false, false, full("0.2.0")...)), list(rel("firmware-v0.1.0", false, false, full("0.1.0")...)))
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model)
	if err != nil || versions(got) != "0.2.0 0.1.0" {
		t.Fatal(versions(got), err)
	}
	if len(s.reqs) != 3 || s.reqs[1].URL.Query().Get("page") != "2" || s.reqs[2].URL.Query().Get("page") != "3" {
		t.Errorf("%d requests", len(s.reqs))
	}
}

func TestListReleasesStopsAtAShortPageAndAtTheBound(t *testing.T) {
	s := pagedAPI(t, list(rel("firmware-v0.1.0", false, false, full("0.1.0")...)))
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model); err != nil || len(s.reqs) != 1 {
		t.Errorf("short page: %d requests, %v", len(s.reqs), err)
	}
	// endless full pages without a usable release: at most MaxReleasePages requests
	var pages [][]byte
	for i := 0; i < kit.MaxReleasePages+3; i++ {
		pages = append(pages, fullPage())
	}
	s = pagedAPI(t, pages...)
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model)
	if err != nil || len(got) != 0 || len(s.reqs) != kit.MaxReleasePages {
		t.Errorf("bound: %d releases, %d requests, %v", len(got), len(s.reqs), err)
	}
}

func TestListReleasesStopsOnceEnoughAreFound(t *testing.T) {
	var items []string
	for i := 0; i < kit.ReleasesPerPage; i++ {
		v := fmt.Sprintf("0.%d.0", i+1)
		items = append(items, rel("firmware-v"+v, false, false, full(v)...))
	}
	page := list(items...)
	s := pagedAPI(t, page, page, page)
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false, model)
	if err != nil || len(got) < kit.EnoughReleases || len(s.reqs) != 1 {
		t.Errorf("%d releases, %d requests, %v", len(got), len(s.reqs), err)
	}
}

func TestListReleasesALaterPageFailureKeepsWhatWasFound(t *testing.T) {
	calls := 0
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.URL.Query().Get("page") == "1" {
			_, _ = w.Write(fullPage(rel("firmware-v0.1.0", false, false, full("0.1.0")...)))
			return
		}
		w.WriteHeader(403)
	}))
	defer srv.Close()
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(srv), srv.URL, "Foohx/akam-fw", false, model)
	if err != nil || versions(got) != "0.1.0" || calls != 2 {
		t.Errorf("%v %v %d", versions(got), err, calls)
	}
}
