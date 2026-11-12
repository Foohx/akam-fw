package cli_test

import (
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// formSession is a board session that also implements ui.FormKeyer (the session UI does): each form
// the flow opens is played by a script that edits through spec.Apply like the widget does, and
// answers with the key it pressed.
type formSession struct {
	*boardSession
	mu2     sync.Mutex
	script  func(n int, spec ui.FormSpec) ui.FormResult
	specs   []ui.FormSpec
	opened  bool
	applied []string
}

func (f *formSession) BoardKey() (string, int, error) {
	f.mu2.Lock()
	defer f.mu2.Unlock()
	if !f.opened {
		f.opened = true
		return "enter", 2, nil // Camera options
	}
	return "esc", 0, nil
}

func (f *formSession) FormKeys(spec ui.FormSpec) (ui.FormResult, error) {
	f.mu2.Lock()
	n := len(f.specs)
	f.specs = append(f.specs, spec)
	f.mu2.Unlock()
	return f.script(n, spec), nil
}

type formOuter struct {
	*ui.Plain
	s *formSession
}

func (o formOuter) Session(fn func(ui.UI) error) error { return fn(o.s) }

func runForm(t *testing.T, e *env, fs *formSession, stdin string) result {
	t.Helper()
	t.Setenv("AKAM_FORCE_TTY", "1")
	h := e.hooks()
	h.UI = func(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
		p := cli.NewPlainUI(o, st, log, secrets, dry)
		fs.boardSession = &boardSession{Plain: p}
		return formOuter{p, fs}
	}
	return runWith(e, h, stdin, append(e.base(), "--camera", "salon")...)
}

func formEnv(t *testing.T) *env {
	e := newEnv(t)
	e.needHelpers()
	e.rememberCamera("salon", "127.0.0.1")
	return e
}

func rowOf(spec ui.FormSpec, id string) ui.FormRow {
	for _, r := range spec.Rows {
		if r.ID == id {
			return r
		}
	}
	return ui.FormRow{}
}

func hasKey(spec ui.FormSpec, k string) bool {
	for _, fk := range spec.Keys {
		if fk.Key == k {
			return true
		}
	}
	return false
}

// The form has the six groups, the choices of the camera and no paragraph; an enum and a number are
// applied at once, the SET line is shown first, and nothing is asked.
func TestOptionsFormEditsEnumAndNumber(t *testing.T) {
	e := formEnv(t)
	var first ui.FormSpec
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		first = spec
		for _, c := range [][2]string{{"led.status", "off"}, {"isp.brightness", "10"}} {
			if _, err := spec.Apply(c[0], c[1]); err != nil {
				t.Errorf("apply %v: %v", c, err)
			}
		}
		return ui.FormResult{}
	}}
	r := runForm(t, e, fs, "")
	expectRC(t, r, 0)
	var groups []string
	for _, row := range first.Rows {
		if len(groups) == 0 || groups[len(groups)-1] != row.Group {
			groups = append(groups, row.Group)
		}
	}
	if strings.Join(groups, ",") != "Video,Picture,LED,Audio,Network,Security" {
		t.Errorf("groups: %v", groups)
	}
	if got := strings.Join(rowOf(first, "video.fps_cfg").Choices, ","); got != "10,15,20,25" {
		t.Errorf("fps choices: %s", got)
	}
	if rowOf(first, "isp.brightness").Kind != ui.FormInt || rowOf(first, "led.status").Kind != ui.FormEnum {
		t.Error("kinds")
	}
	if got := strings.Join(e.cam.CtlSets(), "|"); got != "SET led.status off|SET isp.brightness 10" {
		t.Errorf("sets: %s", got)
	}
	i, j := strings.Index(r.out, "SET led.status off"), strings.Index(r.out, "Front LED: off")
	if i < 0 || j < i {
		t.Errorf("the SET line must come before the result:\n%s", r.out)
	}
	contains(t, r.out, "Brightness: 10 (applies now; saved on the camera).")
	notContains(t, r.err, "[y/N]")
	notContains(t, r.out, "Clock:")
	if e.cam.VideoRestarts() != 0 {
		t.Error("restart without r")
	}
}

// A refused change returns an error (the form snaps the row back) and sends nothing for a value out of
// range; a camera that says ERR is reported with its text.
func TestOptionsFormRefusalSnapsBack(t *testing.T) {
	e := formEnv(t)
	var errs []error
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		_, err := spec.Apply("video.gop_cfg", "9")
		errs = append(errs, err)
		e.cam.SetCtl("led.status", "-") // the image lost the option meanwhile
		_, err = spec.Apply("led.status", "off")
		errs = append(errs, err)
		return ui.FormResult{}
	}}
	r := runForm(t, e, fs, "")
	expectRC(t, r, 0)
	if errs[0] == nil || errs[1] == nil {
		t.Fatalf("errors: %v", errs)
	}
	contains(t, r.err+r.out, "needs a whole number from 1 to 4", "the camera refused led.status off")
	if got := strings.Join(e.cam.CtlSets(), "|"); got != "SET led.status off" {
		t.Errorf("sets: %s", got)
	}
}

// NotSaved and Unavailable rows come from what the camera reports.
func TestOptionsFormNotSavedAndUnavailable(t *testing.T) {
	e := formEnv(t)
	e.cam.SetCtl("isp.sharpness", "-")
	var spec0 ui.FormSpec
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult { spec0 = spec; return ui.FormResult{} }}
	r := runForm(t, e, fs, "")
	expectRC(t, r, 0)
	if !rowOf(spec0, "night.mode").NotSaved {
		t.Error("night.mode is not saved at a reboot")
	}
	if rowOf(spec0, "isp.brightness").NotSaved {
		t.Error("brightness is saved")
	}
	if u := rowOf(spec0, "isp.sharpness"); !u.Unavailable || u.Value != "n/a" {
		t.Errorf("sharpness: %+v", u)
	}
}

// Video restart options are sent at once with the note; the question comes on r only, default No.
func TestOptionsFormRestartOnlyOnR(t *testing.T) {
	e := formEnv(t)
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			row, err := spec.Apply("video.fps_cfg", "20")
			if err != nil || !strings.Contains(row.Note, "r restarts it") {
				t.Errorf("row %+v err %v", row, err)
			}
			return ui.FormResult{Key: "r", Group: "Video"}
		}
		if spec.Group != "Video" {
			t.Errorf("the group is not kept: %q", spec.Group)
		}
		if !strings.Contains(rowOf(spec, "video.fps_cfg").Note, "r restarts it") {
			t.Error("the pending restart is not remembered")
		}
		return ui.FormResult{}
	}}
	r := runForm(t, e, fs, "\n") // r > Enter: default No
	expectRC(t, r, 0)
	contains(t, r.err, "Restart the video service now?")
	contains(t, r.out, "Not restarted")
	if e.cam.VideoRestarts() != 0 {
		t.Error("restarted without a yes")
	}
	// no r: no question at all
	fs = &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if _, err := spec.Apply("video.bitrate_cfg", "2000"); err != nil {
			t.Error(err)
		}
		return ui.FormResult{}
	}}
	r = runForm(t, e, fs, "")
	notContains(t, r.err, "Restart the video service")
	// r then yes: the single audited restart
	fs = &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "r"}
		}
		return ui.FormResult{}
	}}
	runForm(t, e, fs, "y\n")
	if e.cam.VideoRestarts() != 1 {
		t.Errorf("restarts: %d", e.cam.VideoRestarts())
	}
}

// The clock key is offered only when clockReport says so, and the question is asked on t only.
func TestOptionsFormClockOnlyOnT(t *testing.T) {
	e := formEnv(t)
	e.cam.SetClock(time.Date(2000, 1, 1, 0, 0, 30, 0, time.UTC), false)
	var offered bool
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		offered = hasKey(spec, "t")
		return ui.FormResult{}
	}}
	r := runForm(t, e, fs, "")
	expectRC(t, r, 0)
	if !offered {
		t.Error("no t key with the clock unset")
	}
	notContains(t, r.err, "Set the camera clock")
	if len(e.cam.ClockSets()) != 0 {
		t.Error("clock set unprompted")
	}
	// t: the question, default No
	fs = &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "t"}
		}
		return ui.FormResult{}
	}}
	r = runForm(t, e, fs, "\n")
	contains(t, r.err, "Set the camera clock from this computer now?")
	if len(e.cam.ClockSets()) != 0 {
		t.Error("set without a yes")
	}
	runForm(t, e, fs2(), "y\n")
	if len(e.cam.ClockSets()) != 1 {
		t.Errorf("clock sets: %v", e.cam.ClockSets())
	}
	// a right clock: no key
	e.cam.SetClock(time.Now(), true)
	fs = &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		offered = hasKey(spec, "t")
		return ui.FormResult{}
	}}
	runForm(t, e, fs, "")
	if offered {
		t.Error("t offered with a right clock")
	}
}

func fs2() *formSession {
	return &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			return ui.FormResult{Key: "t"}
		}
		return ui.FormResult{}
	}}
}

// Removing the video password keeps its red-box confirmation, default No.
func TestOptionsFormPasswordRemovalKeepsDanger(t *testing.T) {
	e := formEnv(t)
	ok := "éééé"
	expectRC(t, e.run("viewer\n"+ok+"\n"+ok+"\n", "rtsp-auth", "set"), 0)
	fs := &formSession{script: func(n int, spec ui.FormSpec) ui.FormResult {
		if n == 0 {
			if got := rowOf(spec, "ro.Security.Video password").Value; got != "on" {
				t.Errorf("password row: %q", got)
			}
			return ui.FormResult{Key: "p"}
		}
		return ui.FormResult{}
	}}
	r := runForm(t, e, fs, "2\n\n") // remove > Enter = No
	expectRC(t, r, 0)
	if out := e.run("", "--camera", "salon", "rtsp-auth", "status"); !strings.Contains(out.out, "enabled") {
		t.Errorf("the password was removed without a yes:\n%s%s", out.out, out.err)
	}
}
