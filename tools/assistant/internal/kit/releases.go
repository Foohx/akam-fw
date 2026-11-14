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

// MaxReleaseList caps ONE page of the answer: 8 MiB. A page of 50 releases is a few hundred KiB, but each
// release carries its notes (and a firmware release lists the assets of every model), so the cap is generous
// on purpose; an answer longer than that is refused (ErrTooLarge), never truncated.
const MaxReleaseList = 8 << 20

// ReleasesPerPage is the page size asked from the API (firmware and assistant releases share the list; only the
// firmware ones are used). ListReleases follows the pages (page=1, 2, ...) while a page is full, up to
// MaxReleasePages pages, and stops earlier once EnoughReleases usable releases for the model were found: the
// newest firmware releases may sit behind a run of assistant releases.
const ReleasesPerPage = 50

// MaxReleasePages bounds the pagination; EnoughReleases ends it early.
const (
	MaxReleasePages = 5
	EnoughReleases  = 10
)

// FirmwareTagPrefix starts the tag of every firmware release (firmware-vX.Y.Z). The assistant has its own
// releases (assistant-vX.Y.Z), which are never offered as a kit.
const FirmwareTagPrefix = "firmware-v"

// ErrBadModel: a camera model id that does not follow the naming rule.
var ErrBadModel = errors.New("not a valid camera model id")

var modelRe = regexp.MustCompile(`^[a-z0-9]+(-[a-z0-9]+)*$`)

// ValidModel reports whether s is a camera model id: lowercase a-z, 0-9 and single '-', 3 to 40 characters
// (tools/release/models/README in docs/compatible-cameras.md).
func ValidModel(s string) bool { return len(s) >= 3 && len(s) <= 40 && modelRe.MatchString(s) }

// KitFile is the file name of the kit of a model: akam-kit-<model>-<version>.tar.xz.
func KitFile(model, ver string) string { return "akam-kit-" + model + "-" + ver + ".tar.xz" }

// KitTop is the top-level directory of that kit: akam-kit-<model>-<version>.
func KitTop(model, ver string) string { return "akam-kit-" + model + "-" + ver }

// ReleaseTag is the tag of a firmware release: firmware-vX.Y.Z.
func ReleaseTag(ver string) string { return FirmwareTagPrefix + strings.TrimPrefix(ver, "v") }

// ErrBadReleaseList: the answer is not the JSON list of releases that was asked for.
var ErrBadReleaseList = errors.New("the answer is not a list of releases")

// Release is one usable firmware release for a model: a published, non-draft release whose tag is
// firmware-vX.Y.Z and that carries the kit of that model, SHA256SUMS and its signature.
type Release struct {
	Version    string // X.Y.Z, without the prefix
	Tag        string // firmware-vX.Y.Z
	Prerelease bool
	Published  string // YYYY-MM-DD, or "" when the date did not look like one
	Parts      [3]int
}

var tagRe = regexp.MustCompile(`^v(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})$`)
var fwTagRe = regexp.MustCompile(`^firmware-v(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})$`)
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

// ParseReleases keeps the usable firmware releases of the API answer for one camera model, newest version
// first. A release is kept when it is not a draft, its tag is exactly firmware-vX.Y.Z, it carries
// akam-kit-<model>-X.Y.Z.tar.xz, SHA256SUMS and SHA256SUMS.sig, and (unless includePre) it is not marked as a
// prerelease. A release without a kit for this model (an older release, or a model that was dropped) is simply
// not listed. Anything else (assistant releases, malformed entries) is skipped silently: it never hides the
// good ones, but an answer that is not a JSON array is an error.
func ParseReleases(data []byte, includePre bool, model string) ([]Release, error) {
	out, _, err := parseReleasePage(data, includePre, model, map[string]bool{})
	sortReleases(out)
	return out, err
}

// parseReleasePage parses one page; seen (versions already kept, updated here) makes a version that shows up
// twice count once. n is the number of entries of the page (usable or not), to tell a short last page.
func parseReleasePage(data []byte, includePre bool, model string, seen map[string]bool) (out []Release, n int, err error) {
	if !ValidModel(model) {
		return nil, 0, ErrBadModel
	}
	if len(data) > MaxReleaseList {
		return nil, 0, ErrTooLarge
	}
	var raw []json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, 0, fmt.Errorf("%w (%s)", ErrBadReleaseList, "not a JSON array")
	}
	for _, item := range raw {
		var r apiRelease
		if json.Unmarshal(item, &r) != nil {
			continue // one malformed entry never hides the good ones
		}
		m := fwTagRe.FindStringSubmatch(r.Tag)
		if m == nil || r.Draft || (r.Prerelease && !includePre) {
			continue
		}
		ver := strings.TrimPrefix(r.Tag, FirmwareTagPrefix)
		if seen[ver] {
			continue
		}
		want := map[string]bool{KitFile(model, ver): true, "SHA256SUMS": true, "SHA256SUMS.sig": true}
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
	return out, len(raw), nil
}

func sortReleases(out []Release) {
	sort.SliceStable(out, func(i, j int) bool {
		a, b := out[i].Parts, out[j].Parts
		for k := range a {
			if a[k] != b[k] {
				return a[k] > b[k]
			}
		}
		return false
	})
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
// ("owner/name") and returns the usable firmware releases for the camera model, newest first. It follows the
// pages (page=N) while they are full, at most MaxReleasePages of them and not once EnoughReleases usable
// releases were found; an error on a later page is returned only when nothing usable was found yet.
func ListReleases(ctx context.Context, c *Client, apiBase, repo string, includePre bool, model string) ([]Release, error) {
	if !ValidModel(model) {
		return nil, ErrBadModel
	}
	var all []Release
	seen := map[string]bool{}
	for page := 1; page <= MaxReleasePages; page++ {
		url := fmt.Sprintf("%s/repos/%s/releases?per_page=%d&page=%d", strings.TrimRight(apiBase, "/"), repo, ReleasesPerPage, page)
		b, err := FetchBytes(ctx, c, url, MaxReleaseList, "application/vnd.github+json")
		if err != nil {
			if page > 1 && len(all) > 0 {
				break // the newest releases are already known: a failure further back does not hide them
			}
			return nil, err
		}
		got, n, err := parseReleasePage(b, includePre, model, seen)
		if err != nil {
			if page > 1 && len(all) > 0 {
				break
			}
			return nil, err
		}
		all = append(all, got...)
		if n < ReleasesPerPage || len(all) >= EnoughReleases {
			break
		}
	}
	sortReleases(all)
	return all, nil
}
