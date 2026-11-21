package sdcard

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// ToolkitURL is the GitHub tarball of the pinned commit (no git needed). Tests point it elsewhere.
var ToolkitURL = "https://github.com/tasarren/lsc-tuya-toolkit/archive/" + PinCommit + ".tar.gz"

const maxToolkitBytes = 64 << 20

// EnsureToolkit returns the verified sd_card tree of the toolkit in dir. When dir/sd_card is missing it
// downloads the pinned commit's tarball (HTTPS only, size capped), extracts only sd_card/ (plain files,
// local paths) and refuses unless the C-locale tree hash equals the pin.
func EnsureToolkit(ctx context.Context, hc *kit.Client, dir string, say func(string)) (map[string][]byte, error) {
	if st, err := os.Stat(filepath.Join(dir, "sd_card")); err == nil && st.IsDir() {
		files, err := LoadToolkit(dir)
		if err == nil && say != nil {
			say("toolkit verified: sd_card tree " + PinTreeSHA)
		}
		return files, err
	}
	if hc == nil {
		hc = kit.NewHTTPClient()
	}
	if say != nil {
		say("downloading the pinned toolkit (commit " + PinCommit[:12] + ") into " + dir)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	tmp, err := os.MkdirTemp(dir, ".dl-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	tgz := filepath.Join(tmp, "toolkit.tar.gz")
	if err := kit.FetchLimit(ctx, hc, ToolkitURL, tgz, maxToolkitBytes); err != nil {
		return nil, fmt.Errorf("download failed: %w", err)
	}
	stage := filepath.Join(tmp, "x")
	if err := extractSDCard(tgz, stage); err != nil {
		return nil, err
	}
	if _, err := LoadToolkit(stage); err != nil {
		return nil, err
	}
	if err := os.Rename(filepath.Join(stage, "sd_card"), filepath.Join(dir, "sd_card")); err != nil {
		return nil, err
	}
	return LoadToolkit(dir)
}

func extractSDCard(tgz, dst string) error {
	f, err := os.Open(tgz)
	if err != nil {
		return err
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	tr := tar.NewReader(zr)
	var total int64
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		// "<repo>-<commit>/sd_card/..."
		parts := strings.SplitN(h.Name, "/", 2)
		if len(parts) < 2 || !(parts[1] == "sd_card" || strings.HasPrefix(parts[1], "sd_card/")) {
			continue
		}
		rel := path.Clean(parts[1])
		if !filepath.IsLocal(rel) {
			return fmt.Errorf("unsafe path %q in the toolkit archive", h.Name)
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(filepath.Join(dst, rel), 0o755); err != nil {
				return err
			}
		case tar.TypeReg:
			total += h.Size
			if total > maxToolkitBytes {
				return errors.New("the toolkit archive is larger than allowed")
			}
			p := filepath.Join(dst, rel)
			if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
				return err
			}
			out, err := os.OpenFile(p, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
			if err != nil {
				return err
			}
			if _, err := io.Copy(out, io.LimitReader(tr, h.Size)); err != nil {
				out.Close()
				return err
			}
			out.Close()
		default:
			return fmt.Errorf("%s: only plain files are accepted in the toolkit archive", h.Name)
		}
	}
	return nil
}
