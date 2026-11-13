package profile

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
)

// Where the files live, under the state base of the assistant (the directory the process lock covers,
// so a second akam never writes them at the same time; mu covers the goroutines of one process):
//
//	<base>/profiles/global.conf      the global template: settings
//	<base>/profiles/global.secrets   the global template: passwords and Wi-Fi (optional, opt-in)
//	<camera dir>/camera.conf         what was last applied to, or read from, that camera
//	<camera dir>/camera.secrets      its passwords and Wi-Fi (optional, opt-in)
//
// <camera dir> is the state directory of the camera (state.Cameras.DirOf), so a renamed camera takes its
// files along and two cameras never share one.
const (
	globalDir      = "profiles"
	globalSettings = "global.conf"
	globalSecrets  = "global.secrets"
	cameraSettings = "camera.conf"
	cameraSecrets  = "camera.secrets"
)

// Place is one pair of files: the settings and the secrets.
type Place struct {
	Name     string // "global" or the camera name, for messages
	Settings string
	Secrets  string
}

// Global is the place of the global template.
func Global(base string) Place {
	d := filepath.Join(base, globalDir)
	return Place{Name: "global", Settings: filepath.Join(d, globalSettings), Secrets: filepath.Join(d, globalSecrets)}
}

// Camera is the place of the local config of a camera whose state directory is dir.
func Camera(name, dir string) Place {
	return Place{Name: name, Settings: filepath.Join(dir, cameraSettings), Secrets: filepath.Join(dir, cameraSecrets)}
}

func (p Place) path(k Kind) string {
	if k == Secrets {
		return p.Secrets
	}
	return p.Settings
}

var mu sync.Mutex

// ErrNone is returned when a file does not exist.
var ErrNone = errors.New("no saved config")

// Load reads one file of the place. A missing file is ErrNone; a file that does not parse is an error
// that names it (nothing is guessed).
func Load(p Place, k Kind) (*File, error) {
	mu.Lock()
	defer mu.Unlock()
	return load(p, k)
}

func load(p Place, k Kind) (*File, error) {
	fh, err := os.Open(p.path(k))
	if errors.Is(err, os.ErrNotExist) {
		return nil, ErrNone
	}
	if err != nil {
		return nil, err
	}
	defer fh.Close()
	// never read more than the format allows: a huge file is refused, not loaded
	b, err := io.ReadAll(io.LimitReader(fh, maxFile+1))
	if err != nil {
		return nil, err
	}
	if k == Secrets && !fsutil.IsPrivateFile(p.path(k)) {
		return nil, fmt.Errorf("%s is readable by other accounts: restrict it (chmod 600) or delete it with config forget-secrets", p.path(k))
	}
	f, err := Parse(b, k)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", p.path(k), err)
	}
	return f, nil
}

// LoadOrNew is Load with an empty file when there is none.
func LoadOrNew(p Place, k Kind) (*File, error) {
	f, err := Load(p, k)
	if errors.Is(err, ErrNone) {
		return New(k), nil
	}
	return f, err
}

// Exists reports whether the file is there (it may still be unreadable).
func Exists(p Place, k Kind) bool {
	_, err := os.Stat(p.path(k))
	return err == nil
}

// Save writes one file atomically, in a private directory, readable by this account only (0600; a
// protected ACL on Windows). When the access cannot be restricted the file is removed and the error
// returned: a secret is never left readable. An empty secrets file is removed instead of written.
func Save(p Place, k Kind, f *File) error {
	mu.Lock()
	defer mu.Unlock()
	path := p.path(k)
	if k == Secrets && f.Empty() && len(f.Extra) == 0 {
		return remove(path)
	}
	if err := fsutil.PrivateMkdir(filepath.Dir(path)); err != nil {
		return err
	}
	// private from the first byte, and the temporary file is restricted before it replaces the old one
	tmp := fmt.Sprintf("%s.new.%d", path, os.Getpid())
	if err := fsutil.WritePrivateFile(tmp, f.Marshal()); err != nil {
		return err
	}
	if err := fsutil.PrivateFileStrict(tmp); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if err := fsutil.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if !fsutil.IsPrivateFile(path) {
		_ = os.Remove(path)
		return fmt.Errorf("%s is not private after writing", path)
	}
	return nil
}

func remove(path string) error {
	err := os.Remove(path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

// Forget deletes the secrets file of the place (the passwords and the Wi-Fi). It reports whether
// there was one.
func Forget(p Place) (bool, error) {
	mu.Lock()
	defer mu.Unlock()
	had := Exists(p, Secrets)
	return had, remove(p.Secrets)
}

// Remove deletes both files of the place.
func Remove(p Place) error {
	mu.Lock()
	defer mu.Unlock()
	if err := remove(p.Settings); err != nil {
		return err
	}
	return remove(p.Secrets)
}
