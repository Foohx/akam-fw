package state

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestStoreFormatIsTheBashFormat(t *testing.T) {
	dir := t.TempDir()
	s, err := Open(dir, false, &Secrets{})
	if err != nil {
		t.Fatal(err)
	}
	for k, v := range map[string]string{"camera_ip": "192.168.1.50", "kit_path": "/home/me/my kit/x=y"} {
		if err := s.Set(k, v); err != nil {
			t.Fatal(err)
		}
	}
	_ = s.Set("camera_ip", "10.0.0.2") // replaced, moved to the end like state_set
	b, _ := os.ReadFile(filepath.Join(dir, "state"))
	if string(b) != "kit_path=/home/me/my kit/x=y\ncamera_ip=10.0.0.2\n" {
		t.Errorf("file: %q", b)
	}
	if s.Get("kit_path") != "/home/me/my kit/x=y" || s.Get("camera_ip") != "10.0.0.2" || s.Get("nope") != "" {
		t.Error("get")
	}
	if st, _ := os.Stat(filepath.Join(dir, "state")); st.Mode().Perm() != 0o600 {
		t.Error("mode")
	}
	// a value that contains an equals sign and keys that are prefixes of one another
	_ = s.Set("camera", "x")
	if s.Get("camera") != "x" || s.Get("camera_ip") != "10.0.0.2" {
		t.Error("prefix keys")
	}
	_ = s.Unset("camera_ip")
	if s.Get("camera_ip") != "" || s.Get("camera") != "x" {
		t.Error("unset")
	}
	// bash-made files with extra keys are read and kept
	if err := os.WriteFile(filepath.Join(dir, "state"), []byte("wizard_version=1.0.0-dev\nbin_future=1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_ = s.Set("tool", "go")
	b, _ = os.ReadFile(filepath.Join(dir, "state"))
	if string(b) != "wizard_version=1.0.0-dev\nbin_future=1\ntool=go\n" {
		t.Errorf("unknown keys not preserved: %q", b)
	}
}

func TestStoreRefusals(t *testing.T) {
	sec := &Secrets{}
	sec.Add("hunter2hunter2")
	s, _ := Open(t.TempDir(), false, sec)
	for _, c := range []struct{ k, v string }{
		{"Bad", "x"}, {"1bad", "x"}, {"bad-key", "x"}, {"", "x"}, {"ok", "two\nlines"}, {"ok", "my hunter2hunter2 is here"},
	} {
		if err := s.Set(c.k, c.v); err == nil {
			t.Errorf("Set(%q, %q) accepted", c.k, c.v)
		}
	}
}

func TestDryStoreIsSeparate(t *testing.T) {
	dir := t.TempDir()
	real, _ := Open(dir, false, nil)
	dry, _ := Open(dir, true, nil)
	_ = dry.MarkDone("find-camera")
	if real.IsMarked("find-camera") || !dry.IsMarked("find-camera") {
		t.Error("a rehearsal marked a real step")
	}
	if filepath.Base(dry.File) != "state.dry" {
		t.Error(dry.File)
	}
	_ = dry.ResetDry()
	if dry.IsMarked("find-camera") {
		t.Error("reset")
	}
	if StepKey("get-kit") != "get_kit" {
		t.Error("step key")
	}
}

func TestRedaction(t *testing.T) {
	sec := &Secrets{}
	sec.Add("topsecret")
	for in, want := range map[string]string{
		"x topsecret y":                    "x *** y",
		"AKAM_SD_PASS=abc run":             "AKAM_SD_PASS=*** run",
		"wifi password=abc psk=def Pass=g": "wifi password=*** psk=*** Pass=***",
		"nothing here":                     "nothing here",
	} {
		if got := sec.Redact(in); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
	l, err := NewLog(t.TempDir(), sec)
	if err != nil {
		t.Fatal(err)
	}
	l.Line("CMD something with %s", "topsecret")
	b, _ := os.ReadFile(l.Path())
	if strings.Contains(string(b), "topsecret") || !strings.Contains(string(b), "CMD something with ***") {
		t.Errorf("log: %q", b)
	}
	if st, _ := os.Stat(l.Path()); st.Mode().Perm() != 0o600 {
		t.Error("log mode")
	}
}

func TestNamesAndCameras(t *testing.T) {
	for _, ok := range []string{"salon", "a", "a-b", "cam1", strings.Repeat("a", 20)} {
		if !NameValid(ok) {
			t.Errorf("%q", ok)
		}
	}
	for _, bad := range []string{"", "-a", "a-", "A", "a_b", "a b", strings.Repeat("a", 21), "../x"} {
		if NameValid(bad) {
			t.Errorf("%q accepted", bad)
		}
	}
	base := t.TempDir()
	c, _ := InitCameras(base, "", false)
	if !c.Fresh || c.Name != "default" {
		t.Errorf("%+v", c)
	}
	if c.DirOf("default") != base || c.DirOf("salon") != filepath.Join(base, "cameras", "salon") || c.Slug() != "" {
		t.Error("dirs")
	}
	_ = c.Remember("salon")
	c2, _ := InitCameras(base, "", false)
	if c2.Name != "salon" || c2.Fresh || c2.Slug() != "salon-" {
		t.Errorf("%+v", c2)
	}
	if c3, _ := InitCameras(base, "grenier", false); c3.Name != "grenier" {
		t.Error("--camera wins")
	}
	// an invalid current_camera is ignored
	_ = os.WriteFile(filepath.Join(base, "current_camera"), []byte("Bad Name\n"), 0o600)
	if c4, _ := InitCameras(base, "", false); c4.Name != "default" {
		t.Error("invalid current_camera")
	}
	// a rehearsal never writes current_camera
	d, _ := InitCameras(t.TempDir(), "", true)
	_ = d.Remember("x")
	if _, err := os.Stat(filepath.Join(d.Base, "current_camera")); err == nil {
		t.Error("written in a rehearsal")
	}
}

func TestLockIsExclusive(t *testing.T) {
	dir := t.TempDir()
	l1, ok, err := Acquire(dir)
	if err != nil || !ok {
		t.Fatalf("%v %v", ok, err)
	}
	if _, ok2, _ := Acquire(dir); ok2 {
		t.Error("two holders")
	}
	l1.Release()
	l3, ok, _ := Acquire(dir)
	if !ok {
		t.Error("not released")
	}
	l3.Release()
}

func TestBackupBeforeBinary(t *testing.T) {
	dir := t.TempDir()
	if BackupBeforeBinary(dir) {
		t.Error("copied nothing")
	}
	_ = os.WriteFile(filepath.Join(dir, "state"), []byte("step_doctor_done=x\n"), 0o600)
	if !BackupBeforeBinary(dir) || BackupBeforeBinary(dir) {
		t.Error("exactly one copy")
	}
	b, _ := os.ReadFile(filepath.Join(dir, "state.bak-before-binary"))
	if string(b) != "step_doctor_done=x\n" {
		t.Error("content")
	}
}

func TestMoveFilesKeepsAnUntouchedCopy(t *testing.T) {
	base := t.TempDir()
	c, _ := InitCameras(base, "", false)
	_ = os.WriteFile(filepath.Join(base, "state"), []byte("step_doctor_done=x\n"), 0o600)
	_ = os.WriteFile(filepath.Join(base, "akam.log"), []byte("log\n"), 0o600)
	if err := c.MoveFiles("default", "salon"); err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"cameras/salon/state", "cameras/salon/akam.log", "state.before-rename.bak"} {
		if _, err := os.Stat(filepath.Join(base, p)); err != nil {
			t.Errorf("%s: %v", p, err)
		}
	}
	if err := c.MoveFiles("default", "salon"); err == nil {
		t.Error("overwrote an existing camera")
	}
	if got := c.Summary("salon"); got != "1/9 steps done  last: doctor" {
		t.Errorf("summary %q", got)
	}
}
