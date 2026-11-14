package backuploc

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var now = time.Date(2026, 11, 11, 10, 0, 0, 0, time.UTC)

func facts(goos, home string, env map[string]string, files map[string]string, dirs ...string) Facts {
	return Facts{
		GOOS: goos, Home: home,
		Getenv: func(k string) string { return env[k] },
		ReadFile: func(p string) ([]byte, error) {
			if s, ok := files[p]; ok {
				return []byte(s), nil
			}
			return nil, errors.New("absent")
		},
		DirExists: func(p string) bool {
			for _, d := range dirs {
				if d == p {
					return true
				}
			}
			return false
		},
	}
}

func TestSuggestLinux(t *testing.T) {
	ud := "/home/u/.config/user-dirs.dirs"
	for _, c := range []struct {
		name string
		f    Facts
		want string
	}{
		{"user-dirs", facts("linux", "/home/u", nil, map[string]string{ud: "# c\nXDG_DOCUMENTS_DIR=\"$HOME/Docs\"\n"}), "/home/u/Docs/Akam backups/salon/20261111"},
		{"user-dirs braces", facts("linux", "/home/u", nil, map[string]string{ud: "XDG_DOCUMENTS_DIR=\"${HOME}/Papiers\""}), "/home/u/Papiers/Akam backups/salon/20261111"},
		{"disabled (= $HOME) falls back", facts("linux", "/home/u", nil, map[string]string{ud: "XDG_DOCUMENTS_DIR=\"$HOME/\""}, "/home/u/Documents"), "/home/u/Documents/Akam backups/salon/20261111"},
		{"no file, Documents exists", facts("linux", "/home/u", nil, nil, "/home/u/Documents"), "/home/u/Documents/Akam backups/salon/20261111"},
		{"no file, no Documents", facts("linux", "/home/u", nil, nil), "/home/u/Akam backups/salon/20261111"},
	} {
		if got := Suggest(c.f, "salon", now).Dir; got != filepath.FromSlash(c.want) {
			t.Errorf("%s: %s, want %s", c.name, got, c.want)
		}
	}
}

func TestSuggestMacAndICloud(t *testing.T) {
	f := facts("darwin", "/Users/u", nil, nil)
	s := Suggest(f, "salon", now)
	if s.Dir != "/Users/u/Documents/Akam backups/salon/20261111" || s.Cloud != "" {
		t.Errorf("%+v", s)
	}
	// Documents synced by iCloud: the folder is under ~/Library/Mobile Documents/com~apple~CloudDocs
	if why := CloudReason("/Users/u/Library/Mobile Documents/com~apple~CloudDocs/Documents", f); why != "iCloud Drive" {
		t.Errorf("icloud: %q", why)
	}
	f.WinDocuments = "" // unused on macOS
}

func TestSuggestWindows(t *testing.T) {
	f := facts("windows", `C:\Users\u`, nil, nil)
	f.WinDocuments = `C:\Users\u\Documents`
	if s := Suggest(f, "salon", now); !strings.HasSuffix(s.Dir, "salon"+string(filepath.Separator)+"20261111") && !strings.HasSuffix(s.Dir, `salon\20261111`) {
		t.Errorf("%+v", s)
	}
	// Documents redirected into OneDrive: fall back to the home folder and say why
	f = facts("windows", `C:\Users\u`, map[string]string{"OneDrive": `C:\Users\u\OneDrive`}, nil)
	f.WinDocuments = `C:\Users\u\OneDrive\Documents`
	s := Suggest(f, "salon", now)
	if s.Cloud != "OneDrive" || s.Root != `C:\Users\u` {
		t.Errorf("%+v", s)
	}
	f = facts("windows", `C:\Users\u`, map[string]string{"OneDriveCommercial": `D:\Work\OneDrive - Corp`}, nil)
	f.WinDocuments = `D:\Work\OneDrive - Corp\Documents`
	if Suggest(f, "x", now).Cloud != "OneDrive" {
		t.Error("OneDriveCommercial not detected")
	}
}

func TestCloudNames(t *testing.T) {
	f := facts("linux", "/home/u", nil, nil)
	for p, want := range map[string]string{
		"/home/u/Dropbox/Documents":    "Dropbox",
		"/home/u/Google Drive/x":       "Google Drive",
		"/home/u/OneDrive - Corp/x":    "OneDrive",
		"/home/u/Documents":            "",
		"/home/u/dropbox-notes/backup": "",
	} {
		if got := CloudReason(p, f); got != want {
			t.Errorf("%s: %q, want %q", p, got, want)
		}
	}
}

func TestCameraDirNeverUsesCameraText(t *testing.T) {
	// the name is the one chosen by the user; "default" and "" become "camera"
	if CameraDir("default") != "camera" || CameraDir("") != "camera" || CameraDir("garage") != "garage" || DateName(now) != "20261111" {
		t.Error("folder names")
	}
	s := Suggest(facts("linux", "/home/u", nil, nil), "garage", now)
	if len(s.Parents) != 2 || s.Parents[0] != filepath.FromSlash("/home/u/Akam backups") || s.Parents[1] != filepath.FromSlash("/home/u/Akam backups/garage") {
		t.Errorf("parents %v", s.Parents)
	}
}

func TestNextFreeCaseFolded(t *testing.T) {
	base := "/d/Akam backups/salon/20261111"
	if NextFree(base, func(string) bool { return false }) != base {
		t.Error("free base")
	}
	ex := FoldedSet([]string{"/D/AKAM BACKUPS/SALON/20261111", base + "-2"})
	if got := NextFree(base, ex); got != base+"-3" {
		t.Errorf("%s", got)
	}
}

func TestWindowsNames(t *testing.T) {
	for p, bad := range map[string]bool{
		`C:\Users\u\Documents\Akam backups\salon\20261111`: false,
		`C:\Users\u\nul`:      true,
		`C:\Users\u\COM1.txt`: true,
		`C:\Users\u\backup.`:  true,
		`C:\Users\u\backup `:  true,
		`C:\Users\u\a<b`:      true,
		`C:\Users\u\a?b`:      true,
		`\\?\C:\Users\u\ok`:   false,
		`\\server\share\ok`:   false,
		`C:\Users\u\com0`:     false,
	} {
		if got := len(WindowsNameProblems(p)) > 0; got != bad {
			t.Errorf("%s: problem=%v, want %v", p, got, bad)
		}
	}
}

func TestLongPath(t *testing.T) {
	long := `C:\` + strings.Repeat(`a\`, 130)
	if got := LongPath(long); !strings.HasPrefix(got, `\\?\C:\`) {
		t.Error(got)
	}
	if LongPath(`C:\short`) != `C:\short` || LongPath(`\\?\`+long) != `\\?\`+long {
		t.Error("short or prefixed path changed")
	}
	if !strings.HasPrefix(LongPath(`\\srv\sh\`+strings.Repeat("a", 260)), `\\?\UNC\srv`) {
		t.Error("UNC")
	}
}

func TestValidate(t *testing.T) {
	f := facts("linux", "/h", nil, nil)
	ok := State{Exists: false, FreeBytes: 10 << 30, NeedBytes: 8 << 20}
	if p := Validate("/h/b", f, ok); len(p) != 0 {
		t.Errorf("%v", p)
	}
	st := ok
	st.Exists, st.Empty = true, true
	if p := Validate("/h/b", f, st); len(p) != 0 {
		t.Errorf("an empty existing folder is allowed: %v", p)
	}
	st.Empty = false
	if len(Validate("/h/b", f, st)) != 1 {
		t.Error("non-empty folder")
	}
	st = ok
	st.FreeBytes = st.NeedBytes + MinFreeBytes - 1
	if len(Validate("/h/b", f, st)) != 1 {
		t.Error("free space")
	}
	st.FreeBytes = -1 // unknown: not a reason to refuse
	if len(Validate("/h/b", f, st)) != 0 {
		t.Error("unknown free space refused")
	}
	st = ok
	st.InRepo = true
	if len(Validate("/h/b", f, st)) != 1 {
		t.Error("in repo")
	}
	w := facts("windows", `C:\h`, nil, nil)
	if len(Validate(`C:\h\con`, w, ok)) != 1 {
		t.Error("reserved name on windows")
	}
}
