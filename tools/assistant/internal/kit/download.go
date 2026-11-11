package kit

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// ErrTooLarge: the download is longer than its limit.
var ErrTooLarge = errors.New("the download is larger than the allowed size")

// HTTPError is a server answer of 400 or more (curl's exit status 22).
type HTTPError struct {
	Code int
	URL  string
}

func (e *HTTPError) Error() string { return fmt.Sprintf("HTTP %d for %s", e.Code, e.URL) }

// NewHTTPClient is the download client: HTTPS only (also for redirects), TLS 1.2 or newer, and a
// generous total timeout (the kit is a few tens of MB).
func NewHTTPClient() *http.Client {
	return &http.Client{
		Timeout:       15 * time.Minute,
		Transport:     &http.Transport{Proxy: http.ProxyFromEnvironment, TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS12}},
		CheckRedirect: checkRedirect,
	}
}

func checkRedirect(req *http.Request, via []*http.Request) error {
	if req.URL.Scheme != "https" {
		return errors.New("refusing a redirect to a non-HTTPS address")
	}
	if len(via) >= 10 {
		return errors.New("too many redirects")
	}
	return nil
}

// Fetch downloads url into out (written to a temporary file next to it and renamed when complete).
// It retries three times on network errors and server errors, never on a 4xx answer.
func Fetch(ctx context.Context, c *http.Client, url, out string) error {
	return FetchLimit(ctx, c, url, out, MaxKitDownload)
}

// Download size limits: a server cannot fill the disk. The kit is a few tens of MB.
const (
	MaxKitDownload   = 256 << 20
	MaxSmallDownload = 1 << 20 // SHA256SUMS and its signature
)

// FetchLimit is Fetch with a cap on the body (an answer longer than max bytes is an error).
func FetchLimit(ctx context.Context, c *http.Client, url, out string, max int64) error {
	if !strings.HasPrefix(url, "https://") {
		return errors.New("refusing a non-HTTPS download address")
	}
	if c == nil {
		c = NewHTTPClient()
	}
	// A client given by a test may not carry the redirect rule: always enforce it.
	cc := *c
	cc.CheckRedirect = checkRedirect
	var last error
	for try := 0; try < 4; try++ {
		if try > 0 {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(time.Duration(try) * 500 * time.Millisecond):
			}
		}
		last = fetchOnce(ctx, &cc, url, out, max)
		var he *HTTPError
		if last == nil || errors.Is(last, ErrTooLarge) || (errors.As(last, &he) && he.Code < 500 && he.Code != 429) || errors.Is(last, context.Canceled) {
			return last
		}
	}
	return last
}

func fetchOnce(ctx context.Context, c *http.Client, url, out string, max int64) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		return &HTTPError{Code: resp.StatusCode, URL: url}
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(out), ".dl-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if resp.ContentLength > max {
		tmp.Close()
		return ErrTooLarge
	}
	if n, err := io.Copy(tmp, io.LimitReader(resp.Body, max+1)); err != nil || n > max {
		tmp.Close()
		if err == nil {
			err = ErrTooLarge
		}
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(name, 0o644); err != nil {
		return err
	}
	return os.Rename(name, out)
}
