package kit

import (
	"context"
	"io"
)

// Test seams of verify.go, visible to the external test package only.

// SetSymlink makes os.Symlink fail (or be replaced) for this verification.
func (o *Options) SetSymlink(f func(oldname, newname string) error) { o.symlink = f }

// SetIsLocal replaces filepath.IsLocal (a fake for Windows rules).
func (o *Options) SetIsLocal(f func(string) bool) { o.isLocal = f }

// SetModesMatter switches the mode comparison of directory mode and returns the previous value.
func SetModesMatter(v bool) bool { old := modesMatter; modesMatter = v; return old }

// NewLimitReader is the 1 GiB backstop with a small limit.
func NewLimitReader(r io.Reader, left int64) io.Reader { return &limitReader{r: r, left: left} }

// Get does one GET (redirects followed) and reads the body, capped at max bytes.
func Get(ctx context.Context, c *Client, url string, max int64) (int, []byte, error) {
	resp, err := c.get(ctx, url)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, max+1))
	return resp.StatusCode, b, err
}
