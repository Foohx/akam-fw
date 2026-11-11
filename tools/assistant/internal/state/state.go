// Package state keeps the resumable progress of the assistant in the same files, with the same
// format, as the bash wizard (tools/wizard/lib/state.sh, cameras.sh, log.sh): a plain KEY=VALUE
// file that is never executed and never holds a secret, one directory per camera, an akam.log.
// The bash wizard and this binary can be used one after the other on the same directory.
package state

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// DefaultDir returns the state directory: AKAM_STATE_DIR, else $XDG_STATE_HOME/akam, else
// ~/.local/state/akam (state_default_dir in state.sh).
func DefaultDir() string {
	if d := os.Getenv("AKAM_STATE_DIR"); d != "" {
		return d
	}
	if x := os.Getenv("XDG_STATE_HOME"); x != "" {
		return filepath.Join(x, "akam")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "state", "akam")
}

var keyRe = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// Secrets is a set of values that must never be written to a state file or a log.
type Secrets struct {
	mu   sync.Mutex
	list []string
}

// Add registers a secret (empty values are ignored).
func (s *Secrets) Add(v string) {
	if s == nil || v == "" {
		return
	}
	s.mu.Lock()
	s.list = append(s.list, v)
	s.mu.Unlock()
}

// Has reports whether text contains a registered secret.
func (s *Secrets) Has(text string) bool {
	if s == nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, v := range s.list {
		if strings.Contains(text, v) {
			return true
		}
	}
	return false
}

var assignRe = regexp.MustCompile(`(?i)((?:pass(?:word)?|psk)=)[^ ]*`)
var sdPassRe = regexp.MustCompile(`(AKAM_SD_PASS=)[^ ]*`)

// Redact replaces registered secrets and pass/password/psk assignments by ***.
func (s *Secrets) Redact(text string) string {
	if s != nil {
		s.mu.Lock()
		for _, v := range s.list {
			text = strings.ReplaceAll(text, v, "***")
		}
		s.mu.Unlock()
	}
	text = sdPassRe.ReplaceAllString(text, "${1}***")
	return assignRe.ReplaceAllString(text, "${1}***")
}

// Log appends time-stamped, redacted lines to akam.log (no-op when nil or not opened).
type Log struct {
	mu      sync.Mutex
	path    string
	secrets *Secrets
}

// NewLog opens (creating, mode 0600) dir/akam.log.
func NewLog(dir string, secrets *Secrets) (*Log, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	_ = os.Chmod(dir, 0o700)
	p := filepath.Join(dir, "akam.log")
	f, err := os.OpenFile(p, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return nil, err
	}
	f.Close()
	_ = os.Chmod(p, 0o600)
	return &Log{path: p, secrets: secrets}, nil
}

// Path returns the log file path.
func (l *Log) Path() string {
	if l == nil {
		return ""
	}
	return l.path
}

// Line appends one redacted line.
func (l *Log) Line(format string, args ...any) {
	if l == nil || l.path == "" {
		return
	}
	msg := fmt.Sprintf(format, args...)
	msg = l.secrets.Redact(msg)
	l.mu.Lock()
	defer l.mu.Unlock()
	f, err := os.OpenFile(l.path, os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s %s\n", time.Now().UTC().Format("2006-01-02T15:04:05Z"), msg)
}

// Store is one KEY=VALUE state file.
type Store struct {
	Dir     string
	File    string
	Dry     bool
	secrets *Secrets
	mu      sync.Mutex
}

// Open selects dir/state (dir/state.dry for a rehearsal), creating it (0600) when missing.
func Open(dir string, dry bool, secrets *Secrets) (*Store, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	_ = os.Chmod(dir, 0o700)
	name := "state"
	if dry {
		name = "state.dry"
	}
	s := &Store{Dir: dir, File: filepath.Join(dir, name), Dry: dry, secrets: secrets}
	if _, err := os.Stat(s.File); err != nil {
		if err := os.WriteFile(s.File, nil, 0o600); err != nil {
			return nil, err
		}
	}
	_ = os.Chmod(s.File, 0o600)
	return s, nil
}

// ResetDry forgets a previous rehearsal.
func (s *Store) ResetDry() error {
	p := filepath.Join(s.Dir, "state.dry")
	if err := os.WriteFile(p, nil, 0o600); err != nil {
		return err
	}
	return os.Chmod(p, 0o600)
}

func (s *Store) lines() []string {
	b, err := os.ReadFile(s.File)
	if err != nil || len(b) == 0 {
		return nil
	}
	return strings.Split(strings.TrimRight(string(b), "\n"), "\n")
}

// Get returns the value of key (first match, like grep -m1), "" when unset.
func (s *Store) Get(key string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	prefix := key + "="
	for _, l := range s.lines() {
		if strings.HasPrefix(l, prefix) {
			return l[len(prefix):]
		}
	}
	return ""
}

func (s *Store) write(lines []string) error {
	tmp := fmt.Sprintf("%s.tmp.%d", s.File, os.Getpid())
	data := ""
	if len(lines) > 0 {
		data = strings.Join(lines, "\n") + "\n"
	}
	if err := os.WriteFile(tmp, []byte(data), 0o600); err != nil {
		return err
	}
	if err := os.Chmod(tmp, 0o600); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, s.File)
}

// Set replaces key (atomic). It refuses a bad key, a multi-line value or a secret.
func (s *Store) Set(key, val string) error {
	if !keyRe.MatchString(key) {
		return fmt.Errorf("state_set: bad key '%s'", key)
	}
	if strings.Contains(val, "\n") {
		return fmt.Errorf("state_set: multi-line value for '%s'", key)
	}
	if s.secrets.Has(val) {
		return fmt.Errorf("state_set: refusing to store a secret in '%s'", key)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []string
	prefix := key + "="
	for _, l := range s.lines() {
		if !strings.HasPrefix(l, prefix) {
			out = append(out, l)
		}
	}
	out = append(out, prefix+val)
	return s.write(out)
}

// Unset removes key.
func (s *Store) Unset(key string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []string
	prefix := key + "="
	for _, l := range s.lines() {
		if !strings.HasPrefix(l, prefix) {
			out = append(out, l)
		}
	}
	return s.write(out)
}

// StepKey maps a step name to its state key prefix (get-kit -> get_kit).
func StepKey(step string) string { return strings.ReplaceAll(step, "-", "_") }

// MarkDone records that a step is done now.
func (s *Store) MarkDone(step string) error {
	return s.Set("step_"+StepKey(step)+"_done", time.Now().UTC().Format("2006-01-02T15:04:05Z"))
}

// ClearDone forgets a step.
func (s *Store) ClearDone(step string) error { return s.Unset("step_" + StepKey(step) + "_done") }

// IsMarked reports whether a step is recorded as done.
func (s *Store) IsMarked(step string) bool { return s.Get("step_"+StepKey(step)+"_done") != "" }

// ErrNoState is returned when a directory holds no state.
var ErrNoState = errors.New("no state")
