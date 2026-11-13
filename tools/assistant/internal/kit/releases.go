package kit

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// The list of the releases published on GitHub. It is ADVICE, never trust: it only tells the user which
// versions exist. The kit that is finally chosen is downloaded and verified (SHA256SUMS and signature)
// exactly as when a version is typed by hand, and nothing the list says (name, date, text) is shown
// except a version number that passed the strict tag rule below.

// MaxReleaseList caps the answer of the list (30 releases are about 100 KiB in practice).
const MaxReleaseList = 1 << 20

// ReleasesPerPage is the page size asked from the API: the newest 30 releases.
const ReleasesPerPage = 30

// ErrBadReleaseList: the answer is not the JSON list of releases that was asked for.
var ErrBadReleaseList = errors.New("the answer is not a list of releases")

// Release is one usable release: a published, non-draft release whose tag is vX.Y.Z and that carries the
// three files of a kit.
type Release struct {
	Version    string // X.Y.Z, without the v
	Tag        string // vX.Y.Z
	Prerelease bool
	Published  string // YYYY-MM-DD, or "" when the date did not look like one
	Parts      [3]int
}

var tagRe = regexp.MustCompile(`^v(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})$`)
var verRe = regexp.MustCompile(`(?:^|[^0-9.])(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(?:$|[^0-9.]|\.(?:$|[^0-9]))`)
var dateRe = regexp.MustCompile(`^[0-9]{4}-[0-9]{2}-[0-9]{2}`)

// ParseVersion reads "X.Y.Z" (an optional leading v) as three numbers; ok is false for anything else.
func ParseVersion(s string) (p [3]int, ok bool) {
	m := tagRe.FindStringSubmatch("v" + strings.TrimPrefix(s, "v"))
	if m == nil {
		return p, false
	}
	for i := range p {
		p[i], _ = strconv.Atoi(m[i+1])
	}
	return p, true
}

// ExtractVersion finds the first X.Y.Z inside free text such as the version a camera reports ("0.1.0",
// "akam-0.1.0-3-gabc"); ok is false when there is none.
func ExtractVersion(s string) (string, bool) {
	m := verRe.FindStringSubmatch(s)
	if m == nil {
		return "", false
	}
	return m[1] + "." + m[2] + "." + m[3], true
}

// CompareVersions is -1, 0 or 1 for a older than, equal to or newer than b; ok is false when either side
// is not a plain X.Y.Z.
func CompareVersions(a, b string) (int, bool) {
	pa, ok1 := ParseVersion(a)
	pb, ok2 := ParseVersion(b)
	if !ok1 || !ok2 {
		return 0, false
	}
	for i := range pa {
		switch {
		case pa[i] < pb[i]:
			return -1, true
		case pa[i] > pb[i]:
			return 1, true
		}
	}
	return 0, true
}

type apiRelease struct {
	Tag        string `json:"tag_name"`
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	Published  string `json:"published_at"`
	Assets     []struct {
		Name string `json:"name"`
	} `json:"assets"`
}

// ParseReleases keeps the usable releases of the API answer, newest version first. A release is kept
// when it is not a draft, its tag is exactly vX.Y.Z, it carries akam-kit-X.Y.Z.tar.xz, SHA256SUMS and
// SHA256SUMS.sig, and (unless includePre) it is not marked as a prerelease. Anything else is skipped
// silently: a malformed entry never hides the good ones, but an answer that is not a JSON array is an error.
func ParseReleases(data []byte, includePre bool) ([]Release, error) {
	if len(data) > MaxReleaseList {
		return nil, ErrTooLarge
	}
	var raw []json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("%w (%s)", ErrBadReleaseList, "not a JSON array")
	}
	var out []Release
	seen := map[string]bool{}
	for _, item := range raw {
		var r apiRelease
		if json.Unmarshal(item, &r) != nil {
			continue // one malformed entry never hides the good ones
		}
		m := tagRe.FindStringSubmatch(r.Tag)
		if m == nil || r.Draft || (r.Prerelease && !includePre) {
			continue
		}
		ver := r.Tag[1:]
		if seen[ver] {
			continue
		}
		want := map[string]bool{"akam-kit-" + ver + ".tar.xz": true, "SHA256SUMS": true, "SHA256SUMS.sig": true}
		for _, a := range r.Assets {
			delete(want, a.Name)
		}
		if len(want) > 0 {
			continue
		}
		seen[ver] = true
		rel := Release{Version: ver, Tag: r.Tag, Prerelease: r.Prerelease}
		for i := range rel.Parts {
			rel.Parts[i], _ = strconv.Atoi(m[i+1])
		}
		if d := dateRe.FindString(r.Published); d != "" {
			rel.Published = d
		}
		out = append(out, rel)
	}
	sort.SliceStable(out, func(i, j int) bool {
		a, b := out[i].Parts, out[j].Parts
		for k := range a {
			if a[k] != b[k] {
				return a[k] > b[k]
			}
		}
		return false
	})
	return out, nil
}

// FetchBytes is one bounded HTTPS GET kept in memory (an answer longer than max is ErrTooLarge). Unlike
// FetchLimit it never retries a 4xx or 429 answer (a rate limit would only be made worse) and tries a
// network or 5xx failure twice. accept, when set, is sent as the Accept header.
func FetchBytes(ctx context.Context, c *Client, url string, max int64, accept string) ([]byte, error) {
	if !strings.HasPrefix(url, "https://") {
		return nil, errors.New("refusing a non-HTTPS address")
	}
	if c == nil {
		c = NewHTTPClient()
	}
	cc := *c
	cc.Accept = accept
	var last error
	for try := 0; try < 2; try++ {
		var b []byte
		b, last = fetchBytesOnce(ctx, &cc, url, max)
		var he *HTTPError
		var pe *policyError
		if last == nil || errors.As(last, &pe) || errors.Is(last, ErrTooLarge) || errors.Is(last, context.Canceled) || (errors.As(last, &he) && he.Code < 500) {
			return b, last
		}
	}
	return nil, last
}

func fetchBytesOnce(ctx context.Context, c *Client, url string, max int64) ([]byte, error) {
	resp, err := c.get(ctx, url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, &HTTPError{Code: resp.StatusCode, URL: url}
	}
	if resp.ContentLength > max {
		return nil, ErrTooLarge
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, max+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > max {
		return nil, ErrTooLarge
	}
	return b, nil
}

// ListReleases asks the API at apiBase (https://api.github.com) for the newest releases of repo
// ("owner/name") and returns the usable ones, newest first.
func ListReleases(ctx context.Context, c *Client, apiBase, repo string, includePre bool) ([]Release, error) {
	url := fmt.Sprintf("%s/repos/%s/releases?per_page=%d", strings.TrimRight(apiBase, "/"), repo, ReleasesPerPage)
	b, err := FetchBytes(ctx, c, url, MaxReleaseList, "application/vnd.github+json")
	if err != nil {
		return nil, err
	}
	return ParseReleases(b, includePre)
}
