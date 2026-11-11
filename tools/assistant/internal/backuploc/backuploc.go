// Package backuploc decides where the stock-camera backup goes (spec 11, 3.2 and 4.7). It is pure:
// every fact about the computer (OS, home, environment, files, free space) is injected, so the
// rules run in tests on any host.
package backuploc

import (
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"unicode"
)

// Facts are what the rules need to know about the computer.
type Facts struct {
	GOOS         string
	Home         string
	Getenv       func(string) string
	ReadFile     func(string) ([]byte, error) // for ~/.config/user-dirs.dirs
	DirExists    func(string) bool
	WinDocuments string                       // the Documents known folder (Windows), "" when unknown
	EvalSymlinks func(string) (string, error) // optional: a Documents folder that is a link into a synced folder
}

func (f Facts) env(k string) string {
	if f.Getenv == nil {
		return ""
	}
	return f.Getenv(k)
}

// Folder is the subfolder made under the Documents root.
const Folder = "Akam backups"

// MinFreeBytes is the room kept free besides the dump itself.
const MinFreeBytes int64 = 64 << 20

// DocumentsDir is the root of the default suggestion (3.2).
func DocumentsDir(f Facts) string {
	switch f.GOOS {
	case "windows":
		if f.WinDocuments != "" {
			return f.WinDocuments
		}
		return filepath.Join(f.Home, "Documents")
	case "darwin":
		return filepath.Join(f.Home, "Documents")
	}
	if f.ReadFile != nil {
		if b, err := f.ReadFile(filepath.Join(f.Home, ".config", "user-dirs.dirs")); err == nil {
			if d := parseUserDirs(string(b), f.Home); d != "" {
				return d
			}
		}
	}
	if d := filepath.Join(f.Home, "Documents"); f.DirExists != nil && f.DirExists(d) {
		return d
	}
	return f.Home
}

// parseUserDirs reads XDG_DOCUMENTS_DIR="$HOME/..." from a user-dirs.dirs file. A value equal to
// $HOME means "disabled" in the XDG convention and is ignored, as is a relative path.
func parseUserDirs(text, home string) string {
	for _, l := range strings.Split(text, "\n") {
		l = strings.TrimSpace(l)
		if !strings.HasPrefix(l, "XDG_DOCUMENTS_DIR=") {
			continue
		}
		v := strings.Trim(strings.TrimPrefix(l, "XDG_DOCUMENTS_DIR="), `"'`)
		v = strings.ReplaceAll(v, "${HOME}", home)
		v = strings.ReplaceAll(v, "$HOME", home)
		if !filepath.IsAbs(v) || filepath.Clean(v) == filepath.Clean(home) {
			return ""
		}
		return filepath.Clean(v)
	}
	return ""
}

// CloudReason says why path looks like a cloud-synced folder ("" when it does not).
func CloudReason(p string, f Facts) string {
	lp := strings.ToLower(strings.ReplaceAll(p, `\`, "/"))
	if f.GOOS == "windows" {
		for _, k := range []string{"OneDrive", "OneDriveConsumer", "OneDriveCommercial"} {
			if r := f.env(k); r != "" && within(lp, strings.ToLower(strings.ReplaceAll(r, `\`, "/"))) {
				return "OneDrive"
			}
		}
	}
	if strings.Contains(lp, "/library/mobile documents/") || strings.Contains(lp, "com~apple~clouddocs") {
		return "iCloud Drive"
	}
	for _, seg := range strings.Split(lp, "/") {
		switch {
		case strings.HasPrefix(seg, "onedrive"):
			return "OneDrive"
		case seg == "dropbox" || strings.HasPrefix(seg, "dropbox ") || strings.HasPrefix(seg, "dropbox ("):
			return "Dropbox"
		case seg == "google drive" || seg == "googledrive" || seg == "my drive":
			return "Google Drive"
		case seg == "icloud drive" || seg == "iclouddrive":
			return "iCloud Drive"
		case seg == "box" || seg == "box sync" || seg == "nextcloud" || seg == "owncloud" || seg == "pcloud drive":
			return "a synced folder"
		}
	}
	return ""
}

func within(p, root string) bool {
	root = strings.TrimRight(root, "/")
	return root != "" && (p == root || strings.HasPrefix(p, root+"/"))
}

// Suggestion is the default folder and the reasons shown with it.
type Suggestion struct {
	Dir   string // the folder to create (before NextFree)
	Root  string // the Documents root, or the home folder after a cloud fallback
	Cloud string // non-empty when Documents is synced and Dir moved to the home folder
	// Parents are the folders above Dir that are made private too: Akam backups, then the camera.
	Parents []string
}

// CameraDir is the folder of one camera: the name the user chose (state.NameValid); an empty or
// "default" camera is called "camera". Never the hostname the camera reported.
func CameraDir(camera string) string {
	if camera == "" || camera == "default" {
		return "camera"
	}
	return camera
}

// DateName is the folder of one backup, <YYYYMMDD> (a -N suffix is added by NextFree).
func DateName(now time.Time) string { return now.Format("20060102") }

// Suggest is the default location of 3.2: <Documents>/Akam backups/<camera>/<date>, or
// <home>/Akam backups/... when Documents is inside a cloud-synced folder.
func Suggest(f Facts, camera string, now time.Time) Suggestion {
	root := DocumentsDir(f)
	s := Suggestion{Root: root}
	why := CloudReason(root, f)
	if why == "" && f.EvalSymlinks != nil {
		if r, err := f.EvalSymlinks(root); err == nil {
			why = CloudReason(r, f)
		}
	}
	if why != "" {
		s.Cloud = why
		s.Root = f.Home
	}
	top := filepath.Join(s.Root, Folder)
	cam := filepath.Join(top, CameraDir(camera))
	s.Parents = []string{top, cam}
	s.Dir = filepath.Join(cam, DateName(now))
	return s
}

// NextFree returns base, or base-2, base-3... the first that does not exist. On case-insensitive
// systems (Windows, macOS) pass an exists function that folds case (FoldedSet).
func NextFree(base string, exists func(string) bool) string {
	d := base
	for n := 2; exists(d); n++ {
		d = base + "-" + strconv.Itoa(n)
	}
	return d
}

// FoldedSet makes an exists function over a list of names that compares case-folded.
func FoldedSet(names []string) func(string) bool {
	m := map[string]bool{}
	for _, n := range names {
		m[strings.ToLower(n)] = true
	}
	return func(p string) bool { return m[strings.ToLower(p)] }
}

// Problem is one reason a folder is refused.
type Problem struct{ Msg string }

// State is what the caller measured on the file system for Validate.
type State struct {
	Exists    bool
	Empty     bool  // when Exists: the folder holds nothing
	FreeBytes int64 // < 0 when unknown
	NeedBytes int64 // the dump size
	InRepo    bool
}

// Validate checks a folder the user may use (3.2 "Validation").
func Validate(p string, f Facts, st State) []Problem {
	var out []Problem
	add := func(m string) { out = append(out, Problem{m}) }
	if st.Exists && !st.Empty {
		add("the folder already exists and is not empty")
	}
	if st.InRepo {
		add("the folder is inside the repository")
	}
	if st.FreeBytes >= 0 && st.FreeBytes < st.NeedBytes+MinFreeBytes {
		add("not enough free space")
	}
	if f.GOOS == "windows" {
		for _, m := range WindowsNameProblems(p) {
			add(m)
		}
	}
	return out
}

// WindowsNameProblems lists the reasons Windows would refuse the path: reserved device names, a
// trailing dot or space, characters <>:"|?* (a drive colon and a \\?\ prefix are allowed).
func WindowsNameProblems(p string) []string {
	var out []string
	rest := p
	switch {
	case strings.HasPrefix(rest, `\\?\`):
		rest = rest[4:]
	case strings.HasPrefix(rest, `\\`):
		rest = rest[2:]
	}
	if len(rest) >= 2 && rest[1] == ':' && unicode.IsLetter(rune(rest[0])) {
		rest = rest[2:]
	}
	for _, seg := range strings.FieldsFunc(rest, func(r rune) bool { return r == '\\' || r == '/' }) {
		if seg == "." || seg == ".." {
			continue
		}
		if strings.ContainsAny(seg, `<>:"|?*`) {
			out = append(out, `the name "`+seg+`" has a character Windows refuses (<>:"|?*)`)
		}
		if strings.HasSuffix(seg, ".") || strings.HasSuffix(seg, " ") {
			out = append(out, `the name "`+seg+`" ends with a dot or a space`)
		}
		if reservedWin(seg) {
			out = append(out, `the name "`+seg+`" is a Windows device name`)
		}
	}
	return out
}

func reservedWin(name string) bool {
	base := name
	if i := strings.IndexByte(base, '.'); i >= 0 {
		base = base[:i]
	}
	b := strings.ToLower(strings.TrimRight(base, " "))
	switch b {
	case "con", "prn", "aux", "nul":
		return true
	}
	return len(b) == 4 && (b[:3] == "com" || b[:3] == "lpt") && b[3] >= '1' && b[3] <= '9'
}

// LongPath returns p with the \\?\ prefix when it is an absolute Windows path of 248 characters
// or more (the limit for a directory), so that the file calls accept it.
func LongPath(p string) string {
	if len(p) < 248 || strings.HasPrefix(p, `\\?\`) {
		return p
	}
	if strings.HasPrefix(p, `\\`) {
		return `\\?\UNC\` + p[2:]
	}
	if len(p) >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/') {
		return `\\?\` + strings.ReplaceAll(p, "/", `\`)
	}
	return p
}
