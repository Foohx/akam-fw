package appfs

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Output file names of Build, the same as build_appfs.sh (so that flash_appfs.sh can read a directory the assistant
// wrote and the other way round).
const (
	ImageName    = "appfs-akam.squashfs"
	ImageSumName = "appfs-akam.squashfs.sha256"
	SignName     = "appfs_sign.txt"
	ManifestName = "MANIFEST.txt"
)

// WriteOutputs writes the image, its .sha256, appfs_sign.txt and MANIFEST.txt into dir (created, private). It refuses
// to overwrite a directory that already holds an image: a build never silently replaces what a flash may rely on.
func WriteOutputs(dir string, r *BuildResult) error {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	if _, err := os.Stat(filepath.Join(dir, ImageName)); err == nil {
		return fmt.Errorf("%s already holds %s: use a fresh output directory", dir, ImageName)
	}
	for name, b := range map[string][]byte{
		ImageName:    r.Image,
		ImageSumName: []byte(r.SHA256 + "  " + ImageName + "\n"),
		SignName:     []byte(r.Sign + "\n"),
		ManifestName: []byte(r.Manifest),
	} {
		if err := os.WriteFile(filepath.Join(dir, name), b, 0o600); err != nil {
			return err
		}
	}
	return nil
}

// ReadImageDir reads what WriteOutputs (or build_appfs.sh) wrote and returns the Plan fields that describe the image:
// Image, ImageSHA256 (from the .sha256 file, which Flash checks against the bytes) and Sign.
func ReadImageDir(dir string) (img []byte, sha, sign string, err error) {
	if img, err = os.ReadFile(filepath.Join(dir, ImageName)); err != nil {
		return nil, "", "", fmt.Errorf("%s not found (build the appfs image first): %w", filepath.Join(dir, ImageName), err)
	}
	b, err := os.ReadFile(filepath.Join(dir, ImageSumName))
	if err != nil {
		return nil, "", "", err
	}
	f := strings.Fields(string(b))
	if len(f) == 0 || len(f[0]) != 64 {
		return nil, "", "", errors.New(ImageSumName + " is malformed")
	}
	s, err := os.ReadFile(filepath.Join(dir, SignName))
	if err != nil {
		return nil, "", "", err
	}
	return img, f[0], string(bytes.TrimSpace(s)), nil
}
