package appfs

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"
)

// ImageStore keeps the appfs images the assistant flashed, on the PC (never on the camera).
//
// Images are content addressed: <sha256>.squashfs and <sha256>.json (the metadata), written once and never changed.
// Which image plays which role is ONE small file, store.json {current, previous, pending}, replaced by a single
// atomic rename (write a temporary file, fsync it, rename it over the old one). A role change is therefore one
// rename that either happened or did not: there is no state with a new image and an old record, and a failure in the
// middle (antivirus holding a file on Windows, disk full) leaves the store exactly as it was.
//
//	current   believed to be on mtd3 (the rollback source of the next update)
//	previous  what ran before it (the rollback target)
//	pending   an image being flashed: promoted only after the camera was verified (or reconciled by the next run)
type ImageStore struct{ Dir string }

const storeIndexName = "store.json"

// rename is a seam of the tests (a failing rename: antivirus on Windows, full disk).
var rename = os.Rename

type storeIndex struct {
	Version  int    `json:"version"`
	Current  string `json:"current,omitempty"`
	Previous string `json:"previous,omitempty"`
	Pending  string `json:"pending,omitempty"`
	// PreviousOf is the current image when previous was recorded: a rollback target that was not the predecessor of
	// the current image (the current one was replaced through --current-image) is said so.
	PreviousOf string `json:"previous_of,omitempty"`
}

var imageFileRe = regexp.MustCompile(`^[0-9a-f]{64}\.(squashfs|json)$`)

func (s ImageStore) path(name string) string { return filepath.Join(s.Dir, name) }

func (s ImageStore) readIndex() (storeIndex, error) {
	var idx storeIndex
	b, err := os.ReadFile(s.path(storeIndexName))
	if errors.Is(err, fs.ErrNotExist) {
		return idx, nil
	}
	if err != nil {
		return idx, err
	}
	if err := json.Unmarshal(b, &idx); err != nil || idx.Version != 1 {
		return storeIndex{}, fmt.Errorf("%s of the image store is unreadable: delete the folder %s and start again (the images are rebuilt from your kit and your backup)", storeIndexName, s.Dir)
	}
	return idx, nil
}

func (s ImageStore) writeIndex(idx storeIndex) error {
	idx.Version = 1
	b, err := json.MarshalIndent(idx, "", "  ")
	if err != nil {
		return err
	}
	return writeFileAtomic(s.path(storeIndexName), append(b, '\n'))
}

// writeFileAtomic writes b to a temporary file next to path, flushes it to the disk, then renames it over path.
// On Windows a rename over an existing file can fail for a moment (indexer, antivirus): it is retried briefly.
func writeFileAtomic(path string, b []byte) error {
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	if _, err := f.Write(b); err != nil {
		f.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	tries := 1
	if runtime.GOOS == "windows" {
		tries = 10
	}
	for i := 0; ; i++ {
		if err = rename(tmp, path); err == nil {
			break
		}
		if i+1 >= tries {
			_ = os.Remove(tmp)
			return err
		}
		time.Sleep(50 * time.Millisecond)
	}
	syncDir(filepath.Dir(path)) // best effort: a directory cannot be opened for sync on Windows
	return nil
}

func syncDir(dir string) {
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		d.Close()
	}
}

// putImage stores an image under its sha256 (the metadata is written for that hash). It returns the hash.
func (s ImageStore) putImage(img []byte, m ImageMeta) (string, error) {
	if len(img) != PartitionSize {
		return "", fmt.Errorf("an appfs image is %d bytes, got %d", PartitionSize, len(img))
	}
	if err := os.MkdirAll(s.Dir, 0o700); err != nil {
		return "", err
	}
	h := sum(img)
	m.SHA256 = h
	if st, err := os.Stat(s.path(h + ".squashfs")); err != nil || st.Size() != int64(len(img)) {
		if err := writeFileAtomic(s.path(h+".squashfs"), img); err != nil {
			return "", err
		}
	}
	mb, _ := json.MarshalIndent(m, "", "  ")
	if err := writeFileAtomic(s.path(h+".json"), append(mb, '\n')); err != nil {
		return "", err
	}
	return h, nil
}

func (idx storeIndex) role(name string) (string, error) {
	switch name {
	case "current":
		return idx.Current, nil
	case "previous":
		return idx.Previous, nil
	case "pending":
		return idx.Pending, nil
	}
	return "", fmt.Errorf("internal: no image role %q", name)
}

// Load returns the image playing a role (current, previous, pending) after checking its size and its sha256 against
// the metadata. An empty role is fs.ErrNotExist.
func (s ImageStore) Load(name string) ([]byte, ImageMeta, error) {
	var m ImageMeta
	idx, err := s.readIndex()
	if err != nil {
		return nil, m, err
	}
	h, err := idx.role(name)
	if err != nil {
		return nil, m, err
	}
	if h == "" {
		return nil, m, fs.ErrNotExist
	}
	b, err := os.ReadFile(s.path(h + ".squashfs"))
	if err != nil {
		return nil, m, fmt.Errorf("the stored %s image is missing or unreadable (%v): not used", name, err)
	}
	mb, err := os.ReadFile(s.path(h + ".json"))
	if err != nil {
		return nil, m, fmt.Errorf("the stored %s image has no metadata (%v): not used", name, err)
	}
	if err := json.Unmarshal(mb, &m); err != nil {
		return nil, m, fmt.Errorf("stored image %s: unreadable metadata", name)
	}
	if len(b) != PartitionSize || sum(b) != h || m.SHA256 != h {
		return nil, m, fmt.Errorf("the stored %s image does not match its checksum (changed or damaged): not used", name)
	}
	return b, m, nil
}

// SetCurrent records img as the image on the camera. Call it only for an image that was PROVEN to be on mtd3 (the
// byte for byte comparison of the flash step (a)), never for a file that was merely named by the operator.
func (s ImageStore) SetCurrent(img []byte, m ImageMeta) error {
	h, err := s.putImage(img, m)
	if err != nil {
		return err
	}
	idx, err := s.readIndex()
	if err != nil {
		return err
	}
	if idx.Current == h {
		return nil
	}
	idx.Current = h
	if err := s.writeIndex(idx); err != nil {
		return err
	}
	s.gc()
	return nil
}

// Stage records the image about to be flashed as pending.
func (s ImageStore) Stage(img []byte, m ImageMeta) error {
	h, err := s.putImage(img, m)
	if err != nil {
		return err
	}
	idx, err := s.readIndex()
	if err != nil {
		return err
	}
	idx.Pending = h
	return s.writeIndex(idx)
}

// Discard drops the pending record (and the image files nothing refers to any more).
func (s ImageStore) Discard() {
	idx, err := s.readIndex()
	if err != nil || idx.Pending == "" {
		return
	}
	idx.Pending = ""
	if s.writeIndex(idx) == nil {
		s.gc()
	}
}

// Promote makes pending the current image and the old current the previous one (rollback target), in one atomic
// replacement of store.json.
func (s ImageStore) Promote() error {
	idx, err := s.readIndex()
	if err != nil {
		return err
	}
	if idx.Pending == "" {
		return errors.New("no pending image to promote")
	}
	if idx.Current != "" && idx.Current != idx.Pending {
		idx.Previous, idx.PreviousOf = idx.Current, idx.Pending
	}
	idx.Current, idx.Pending = idx.Pending, ""
	if err := s.writeIndex(idx); err != nil {
		return err
	}
	s.gc()
	return nil
}

// PreviousIsPredecessor reports whether the previous image is the one that ran just before the current image (false
// when the current image was replaced through --current-image after the last update).
func (s ImageStore) PreviousIsPredecessor() bool {
	idx, err := s.readIndex()
	return err == nil && idx.Previous != "" && idx.PreviousOf == idx.Current
}

// RecordFlashed records an image that a write VERIFIED on mtd3 (appfs-flash after its read-back): it becomes the
// current image. before is the image that was on the camera when the write started (nil for a first install from the
// stock backup); the byte for byte gate of the write proved it, so it is recorded as the rollback target.
func (s ImageStore) RecordFlashed(before []byte, beforeMeta ImageMeta, img []byte, m ImageMeta) error {
	if len(img) != PartitionSize || sum(img) != m.SHA256 {
		return errors.New("the flashed image does not match its checksum: not recorded")
	}
	if before != nil {
		if cur, _, err := s.Load("current"); err != nil || sum(cur) != sum(before) {
			if err := s.SetCurrent(before, beforeMeta); err != nil {
				return err
			}
		}
	}
	if err := s.Stage(img, m); err != nil {
		return err
	}
	return s.Promote()
}

// gc removes the image files no role refers to, and leftover temporary files. Best effort: a file that cannot be
// removed (held by an antivirus) is left for the next time.
func (s ImageStore) gc() {
	idx, err := s.readIndex()
	if err != nil {
		return
	}
	keep := map[string]bool{}
	for _, h := range []string{idx.Current, idx.Previous, idx.Pending} {
		if h != "" {
			keep[h+".squashfs"], keep[h+".json"] = true, true
		}
	}
	ents, err := os.ReadDir(s.Dir)
	if err != nil {
		return
	}
	for _, e := range ents {
		n := e.Name()
		if (imageFileRe.MatchString(n) && !keep[n]) || strings.HasSuffix(n, ".tmp") {
			_ = os.Remove(s.path(n))
		}
	}
}
