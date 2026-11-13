package kit_test

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
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

func full(ver string) []string {
	return []string{"akam-kit-" + ver + ".tar.xz", "SHA256SUMS", "SHA256SUMS.sig"}
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
		rel("v0.2.0", false, false, full("0.2.0")...),
		rel("v0.10.0", false, false, full("0.10.0")...),
		rel("v0.9.1", false, false, full("0.9.1")...),
		rel("v1.0.0", true, false, full("1.0.0")...),                                                 // draft
		rel("v0.3.0-rc1", false, false, "akam-kit-0.3.0-rc1.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"), // not plain semver
		rel("0.4.0", false, false, full("0.4.0")...),                                                 // no v
		rel("v01.0.0", false, false, full("01.0.0")...),                                              // leading zero
		rel("v0.5.0", false, false, "akam-kit-0.5.0.tar.xz", "SHA256SUMS"),                           // no signature
		rel("v0.6.0", false, false, "akam-kit-0.6.1.tar.xz", "SHA256SUMS", "SHA256SUMS.sig"),         // kit of another version
		rel("v0.7.0", false, false),                                                                  // no assets
		rel("v0.2.0", false, false, full("0.2.0")...),                                                // duplicate
		rel("v0.1.0", false, true, full("0.1.0")...),                                                 // prerelease
		`{"tag_name":"v9.9.9"}`,                                                                      // an object that is not a release list entry is simply skipped
		`"text"`, `42`, `null`,
	)
	got, err := kit.ParseReleases(data, false)
	if err != nil {
		t.Fatal(err)
	}
	if v := versions(got); v != "0.10.0 0.9.1 0.2.0" {
		t.Errorf("without prereleases: %q", v)
	}
	if got[0].Tag != "v0.10.0" || got[0].Published != "2026-03-04" || got[0].Prerelease {
		t.Errorf("fields: %+v", got[0])
	}
	got, err = kit.ParseReleases(data, true)
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
		"truncated": `[{"tag_name":"v1.0.0"`, "number": "7",
	} {
		if _, err := kit.ParseReleases([]byte(in), false); !errors.Is(err, kit.ErrBadReleaseList) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if _, err := kit.ParseReleases(make([]byte, kit.MaxReleaseList+1), false); !errors.Is(err, kit.ErrTooLarge) {
		t.Errorf("oversized: %v", err)
	}
	if got, err := kit.ParseReleases([]byte("[]"), false); err != nil || len(got) != 0 {
		t.Errorf("empty list: %v %v", got, err)
	}
}

// A hostile name or date never reaches the user: only the version (strict rule) and a YYYY-MM-DD date do.
func TestParseReleasesShowsNothingElseOfTheAnswer(t *testing.T) {
	data := []byte(`[{"tag_name":"v1.2.3","name":"\u001b[31mevil","published_at":"\u001b[2Jnot a date","assets":[` +
		`{"name":"akam-kit-1.2.3.tar.xz"},{"name":"SHA256SUMS"},{"name":"SHA256SUMS.sig"}]}]`)
	got, err := kit.ParseReleases(data, false)
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
	s := newAPI(t, 200, list(rel("v0.1.0", false, false, full("0.1.0")...)))
	got, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false)
	if err != nil || versions(got) != "0.1.0" {
		t.Fatal(got, err)
	}
	r := s.reqs[0]
	if r.URL.Path != "/repos/Foohx/akam-fw/releases" || r.URL.Query().Get("per_page") != "30" || r.Method != "GET" {
		t.Errorf("request %s %s", r.Method, r.URL)
	}
	if r.Header.Get("Accept") != "application/vnd.github+json" || r.Header.Get("User-Agent") != "akam" {
		t.Errorf("headers %v", r.Header)
	}
}

func TestListReleasesErrors(t *testing.T) {
	for _, code := range []int{403, 429, 404, 500} {
		s := newAPI(t, code, []byte(`{"message":"API rate limit exceeded"}`))
		_, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false)
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
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false); !errors.Is(err, kit.ErrTooLarge) {
		t.Errorf("oversized answer: %v", err)
	}
	s = newAPI(t, 200, []byte("not json"))
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(s.srv), s.srv.URL, "Foohx/akam-fw", false); !errors.Is(err, kit.ErrBadReleaseList) {
		t.Errorf("garbage answer: %v", err)
	}
	if _, err := kit.ListReleases(context.Background(), nil, "http://api.example/", "Foohx/akam-fw", false); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("plain http: %v", err)
	}
}

// A redirect to plain http is refused, as for the kit downloads.
func TestListReleasesRefusesAnHTTPRedirect(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://example.invalid/x", http.StatusFound)
	}))
	defer srv.Close()
	if _, err := kit.ListReleases(context.Background(), kittest.ClientFor(srv), srv.URL, "Foohx/akam-fw", false); err == nil || !strings.Contains(err.Error(), "non-HTTPS") {
		t.Errorf("redirect: %v", err)
	}
}
