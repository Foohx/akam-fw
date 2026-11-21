package appfs

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// fakeCam is a scripted 2K: six partitions, a file store for what is uploaded, the on-camera worker and (for the hot
// path) a status feed. Every command it receives is recorded, so a test can prove what was NOT sent.
type fakeCam struct {
	t     testing.TB
	mtd   [6][]byte
	files map[string][]byte
	cmds  []string
	dials []int
	// knobs
	noFeeder, feederInAppfs bool
	sdMount                 string
	sdFreeKB, memFreeKB     string
	appStays                bool
	busyLib                 string
	corruptUploads          bool
	worker                  string // OK | ROLLED_BACK | ROLLBACK_FAILED | WORKER_BAD_INPUT | SILENT
	badReadback             bool   // the PC read-back of mtd3 is not the image
	shortReads              bool
	nvram                   string
	nvramReadback           string
	status                  []string // hot: statuses returned one per poll (the last one repeats)
	hotResult               string
	workerRuns              int
}

func newCam(t testing.TB, backup [6][]byte) *fakeCam {
	return &fakeCam{t: t, mtd: backup, files: map[string][]byte{}, sdMount: "/dev/mmcblk0p1 vfat", sdFreeKB: "900000", memFreeKB: "12000",
		worker: "OK", nvram: "oldsign", nvramReadback: ""}
}

func (c *fakeCam) Close() {}

func (c *fakeCam) count(sub string) (n int) {
	for _, x := range c.cmds {
		if strings.Contains(x, sub) {
			n++
		}
	}
	return
}

func ok(s string) (camssh.Result, error) { return camssh.Result{Stdout: []byte(s)}, nil }
func st(n int) (camssh.Result, error)    { return camssh.Result{Status: n}, nil }

func (c *fakeCam) Run(_ context.Context, cmd string, stdin io.Reader) (camssh.Result, error) {
	c.cmds = append(c.cmds, cmd)
	var in []byte
	if stdin != nil {
		in, _ = io.ReadAll(stdin)
	}
	switch {
	case cmd == "cat /proc/mtd":
		return ok("dev:    size   erasesize  name\nmtd0: 00040000 00010000 \"boot\"\nmtd3: 00490000 00010000 \"appfs\"\n")
	case strings.HasPrefix(cmd, "dd if=/dev/mtd"):
		var n int
		fmt.Sscanf(cmd, "dd if=/dev/mtd%d", &n)
		if n == 3 && c.worker != "" && c.workerRuns > 0 {
			if c.shortReads {
				return ok("short")
			}
			if c.badReadback {
				b := append([]byte(nil), c.mtd[3]...)
				b[100] ^= 0xff
				return ok(string(b))
			}
		}
		return camssh.Result{Stdout: c.mtd[n]}, nil
	case strings.HasPrefix(cmd, "cat > "):
		p := strings.Fields(cmd)[2]
		c.files[p] = in
		return st(0)
	case strings.HasPrefix(cmd, "md5sum "):
		p := strings.TrimPrefix(strings.TrimPrefix(cmd, "md5sum "), "<")
		b := c.files[p]
		if c.corruptUploads && strings.HasSuffix(p, ".bin") {
			b = append([]byte("x"), b...)
		}
		return ok(md5hex(b) + "  " + p + "\n")
	case cmd == `ps | grep '[b]usybox watchdog'`:
		if c.noFeeder {
			return st(1)
		}
		return ok("123 root busybox watchdog\n")
	case strings.HasPrefix(cmd, "for t in flashcp"):
		return st(0)
	case strings.Contains(cmd, `grep -q "/appfs/" /proc/$p/maps`):
		if c.feederInAppfs {
			return st(1)
		}
		return st(0)
	case strings.HasPrefix(cmd, "awk"):
		return ok(c.sdMount + "\n")
	case cmd == "mkdir -p "+sdStage:
		return st(0)
	case strings.HasPrefix(cmd, "df "):
		return ok(c.sdFreeKB + "\n")
	case strings.HasPrefix(cmd, "grep MemFree"):
		return ok(c.memFreeKB)
	case strings.Contains(cmd, "[o]wn_app_run.sh"):
		if c.appStays {
			return st(1)
		}
		return st(0)
	case strings.Contains(cmd, `"/appfs/lib"`):
		return ok(c.busyLib)
	case cmd == "sync":
		return st(0)
	case strings.HasPrefix(cmd, "nohup sh /tmp/akam_flash_worker.sh"):
		c.workerRuns++
		switch c.worker {
		case "OK":
			c.mtd[3] = c.files[sdStage+"/appfs-new.bin"]
			if c.status != nil {
				c.mtd[3] = c.files[ramStage+"/appfs-new.bin"]
			}
		case "ROLLED_BACK":
			c.mtd[3] = c.files[sdStage+"/appfs-backup.bin"]
		}
		return st(0)
	case cmd == "cat /tmp/akam_flash.result 2>/dev/null":
		if c.worker == "SILENT" {
			return ok("")
		}
		return ok(c.worker + "\n")
	case cmd == "cat /tmp/akam_flash.log":
		return ok("flashing new image\n")
	case cmd == "nvram get appfs_sign":
		if c.nvramReadback != "" && c.count("nvram set") > 0 {
			return ok(c.nvramReadback + "\n")
		}
		return ok(c.nvram + "\n")
	case strings.HasPrefix(cmd, "nvram set appfs_sign "):
		c.nvram = strings.Fields(cmd)[3]
		return st(0)
	// ---- hot
	case strings.HasPrefix(cmd, "mkdir -p "+hotDir):
		return st(0)
	case strings.Contains(cmd, "run.lock/pid") && !strings.Contains(cmd, "cat /tmp/akam_flash.result"):
		return st(1) // no runner alive
	case strings.HasPrefix(cmd, "rm -f "+hotDir):
		return st(0)
	case strings.Contains(cmd, "sh "+hotSh+" run "):
		return st(0)
	case strings.HasPrefix(cmd, "cat "+hotDir+"/status"):
		if len(c.status) == 0 {
			return ok("")
		}
		s := c.status[0]
		if len(c.status) > 1 {
			c.status = c.status[1:]
		}
		return ok(s + "\n")
	case strings.HasPrefix(cmd, "touch "+hotDir+"/confirm."):
		if strings.HasSuffix(cmd, "confirm.go") && c.hotResult != "" {
			c.workerRuns++
			c.mtd[3] = c.files[ramStage+"/appfs-new.bin"]
		}
		return st(0)
	case strings.HasPrefix(cmd, "cat /tmp/akam_flash.result 2>/dev/null; echo ---"):
		if c.workerRuns > 0 {
			return ok(c.hotResult + "\n---\nalive\n")
		}
		return ok("\n---\nalive\n")
	case strings.HasPrefix(cmd, "cat "+hotDir+"/run.log"):
		return ok("runner log line\n")
	case cmd == "true":
		return st(0)
	case strings.Contains(cmd, "akam_rollback_worker") || strings.Contains(cmd, "akam_flash_worker"):
		return st(0)
	}
	c.t.Errorf("the fake camera got an unexpected command: %q", cmd)
	return st(127)
}

type world struct {
	t       *testing.T
	cam     *fakeCam
	plan    Plan
	backup  [6][]byte
	out     []string
	conf    Confirmation
	dials   int
	slept   time.Duration
	workDir string
}

func yes() (bool, error) { return true, nil }

// newWorld builds a stock backup directory, an image built from it and a fake camera that holds the stock flash.
func newWorld(t *testing.T) *world {
	t.Helper()
	f := kitFiles()
	useBusyBox(t, f)
	stock := stockAppfs(t, stockInit)
	res, err := build(t, stock, f)
	if err != nil {
		t.Fatal(err)
	}
	w := &world{t: t}
	for i := range w.backup {
		w.backup[i] = bytes.Repeat([]byte{byte(0x10 + i)}, 64*1024)
	}
	w.backup[3] = stock
	dir := t.TempDir()
	bdir := filepath.Join(dir, "backup")
	if err := os.MkdirAll(bdir, 0o700); err != nil {
		t.Fatal(err)
	}
	var sums bytes.Buffer
	for i, b := range w.backup {
		name := fmt.Sprintf("mtd%d.bin", i)
		if err := os.WriteFile(filepath.Join(bdir, name), b, 0o600); err != nil {
			t.Fatal(err)
		}
		fmt.Fprintf(&sums, "%s  %s\n", shaHex(b), name)
	}
	if err := os.WriteFile(filepath.Join(bdir, "SHA256SUMS"), sums.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}
	w.cam = newCam(t, w.backup)
	w.workDir = filepath.Join(dir, "work")
	w.plan = Plan{Image: res.Image, ImageSHA256: res.SHA256, Sign: res.Sign, BackupDir: bdir, WorkDir: w.workDir, Target: "root@192.0.2.10", Execute: true}
	c, err := NewConfirmation(res.SHA256, yes, yes)
	if err != nil {
		t.Fatal(err)
	}
	w.conf = c
	return w
}

func (w *world) run() (*FlashResult, error) {
	deps := Deps{
		Dial: func(port int) (Conn, error) { w.cam.dials = append(w.cam.dials, port); return w.cam, nil },
		Out:  func(l string) { w.out = append(w.out, l) },
		Sleep: func(d time.Duration) {
			w.slept += d
		},
		Ctx: context.Background(),
	}
	return Flash(w.plan, w.conf, deps)
}

func (w *world) outText() string { return strings.Join(w.out, "\n") }

// assertNothingWritten: the camera got no upload, no worker, no nvram write, no reboot, and mtd3 is as it was.
func (w *world) assertNothingWritten() {
	w.t.Helper()
	for _, bad := range []string{"cat > ", "nohup sh /tmp/akam_flash_worker", "nvram set", "reboot", "poweroff", "flashcp -v"} {
		if n := w.cam.count(bad); n != 0 {
			w.t.Errorf("%d command(s) with %q were sent, nothing may be written: %q", n, bad, w.cam.cmds)
		}
	}
	if !bytes.Equal(w.cam.mtd[3], w.backup[3]) {
		w.t.Error("mtd3 changed")
	}
}

func (w *world) assertNeverRebooted() {
	w.t.Helper()
	for _, c := range w.cam.cmds {
		for _, bad := range []string{"reboot", "poweroff", "halt", "kill -9 1", "nvram commit"} {
			if strings.Contains(c, bad) && !strings.Contains(c, "nvram set appfs_sign") {
				w.t.Errorf("forbidden command %q sent: %q", bad, c)
			}
		}
	}
}

func TestDryRunTouchesNothing(t *testing.T) {
	for _, mode := range []struct {
		name string
		mod  func(*Plan, *world)
	}{
		{"fresh", func(*Plan, *world) {}},
		{"update", func(p *Plan, w *world) { p.Update, p.Current = true, w.backup[3][:len(w.backup[3]):len(w.backup[3])] }},
		{"hot", func(p *Plan, w *world) { p.Update, p.Hot, p.Current = true, true, w.backup[3] }},
		{"hot preflight", func(p *Plan, w *world) { p.Update, p.HotPreflight, p.Current = true, true, w.backup[3] }},
	} {
		t.Run(mode.name, func(t *testing.T) {
			w := newWorld(t)
			w.plan.Execute = false
			w.conf = Confirmation{} // a dry run needs no confirmation
			mode.mod(&w.plan, w)
			w.plan.Current = append([]byte(nil), w.plan.Current...)
			if len(w.plan.Current) > 0 {
				w.plan.Current[200] ^= 1 // differs from the new image
			}
			if _, err := w.run(); err != nil {
				t.Fatal(err)
			}
			if len(w.cam.dials) != 0 || len(w.cam.cmds) != 0 {
				t.Errorf("a dry run reached the camera: dials %v cmds %q", w.cam.dials, w.cam.cmds)
			}
			if _, err := os.Stat(w.workDir); err == nil {
				t.Error("a dry run created the work dir")
			}
			if !strings.Contains(w.outText(), "[dry-run]") || !strings.Contains(w.outText(), "dry run finished") {
				t.Errorf("no plan printed:\n%s", w.outText())
			}
			if strings.Contains(w.outText(), "LIVE run") {
				t.Error("a dry run said LIVE")
			}
		})
	}
}

func TestLiveNeedsAConfirmationForThisImage(t *testing.T) {
	w := newWorld(t)
	w.conf = Confirmation{}
	if _, err := w.run(); !errors.Is(err, ErrNotConfirmed) {
		t.Fatalf("got %v", err)
	}
	other, _ := NewConfirmation(strings.Repeat("0", 64), yes, yes)
	w.conf = other
	if _, err := w.run(); !errors.Is(err, ErrNotConfirmed) {
		t.Fatalf("a confirmation of another image was accepted: %v", err)
	}
	if len(w.cam.dials) != 0 {
		t.Error("connected without a valid confirmation")
	}
}

func TestConfirmationNeedsBothAnswers(t *testing.T) {
	no := func() (bool, error) { return false, nil }
	boom := func() (bool, error) { return false, errors.New("no terminal") }
	asked := 0
	counted := func() (bool, error) { asked++; return true, nil }
	for name, c := range map[string]struct{ rec, go_ func() (bool, error) }{
		"recovery refused": {no, yes}, "go refused": {yes, no}, "recovery error": {boom, yes}, "go error": {yes, boom},
	} {
		if _, err := NewConfirmation("a", c.rec, c.go_); err == nil {
			t.Errorf("%s: confirmed", name)
		}
	}
	// the typed go is not even asked when the recovery means are not acknowledged
	if _, err := NewConfirmation("a", no, counted); err == nil || asked != 0 {
		t.Errorf("asked %d, err %v", asked, err)
	}
	c, err := NewConfirmation("abc", yes, yes)
	if err != nil || !c.For("abc") || c.For("abd") || (Confirmation{}).For("") {
		t.Errorf("%v", err)
	}
}

func TestLocalGatesRefuseBeforeAnyConnection(t *testing.T) {
	cases := map[string]func(*world){
		"image of the wrong size": func(w *world) { w.plan.Image = w.plan.Image[:len(w.plan.Image)-1] },
		"image sha does not match": func(w *world) {
			w.plan.Image = append([]byte(nil), w.plan.Image...)
			w.plan.Image[len(w.plan.Image)-1] = 1
		},
		"image not a squashfs": func(w *world) {
			w.plan.Image = append([]byte(nil), w.plan.Image...)
			w.plan.Image[0] = 'x'
			w.plan.ImageSHA256 = sum(w.plan.Image)
			w.conf, _ = NewConfirmation(w.plan.ImageSHA256, yes, yes)
		},
		"sign does not match":  func(w *world) { w.plan.Sign = strings.Repeat("0", 32) },
		"sign empty":           func(w *world) { w.plan.Sign = "" },
		"backup dir missing":   func(w *world) { w.plan.BackupDir = filepath.Join(w.plan.BackupDir, "nope") },
		"backup dir not given": func(w *world) { w.plan.BackupDir = "" },
		"backup partition missing": func(w *world) {
			_ = os.Remove(filepath.Join(w.plan.BackupDir, "mtd5.bin"))
		},
		"backup does not match SHA256SUMS": func(w *world) {
			_ = os.WriteFile(filepath.Join(w.plan.BackupDir, "mtd0.bin"), []byte("tampered"), 0o600)
		},
		"backup mtd3 tampered": func(w *world) {
			b := append([]byte(nil), w.backup[3]...)
			b[500] ^= 1
			_ = os.WriteFile(filepath.Join(w.plan.BackupDir, "mtd3.bin"), b, 0o600)
		},
		"backup has no SHA256SUMS": func(w *world) { _ = os.Remove(filepath.Join(w.plan.BackupDir, "SHA256SUMS")) },
		"SHA256SUMS does not list mtd3": func(w *world) {
			_ = os.WriteFile(filepath.Join(w.plan.BackupDir, "SHA256SUMS"), []byte(shaHex(w.backup[0])+"  mtd0.bin\n"), 0o600)
		},
		"work dir is the backup dir": func(w *world) { w.plan.WorkDir = w.plan.BackupDir },
		"work dir already holds a dump": func(w *world) {
			_ = os.MkdirAll(w.plan.WorkDir, 0o700)
			_ = os.WriteFile(filepath.Join(w.plan.WorkDir, "mtd1.bin"), []byte("x"), 0o600)
		},
		"hot without update": func(w *world) { w.plan.Hot = true },
		"update with a short current image": func(w *world) {
			w.plan.Update, w.plan.Current = true, []byte("short")
		},
		"update with the new image as current": func(w *world) {
			w.plan.Update, w.plan.Current = true, w.plan.Image
		},
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			w := newWorld(t)
			mutate(w)
			res, err := w.run()
			if err == nil {
				t.Fatalf("ran: %+v", res)
			}
			if len(w.cam.dials) != 0 || len(w.cam.cmds) != 0 {
				t.Errorf("reached the camera after a local refusal: %q", w.cam.cmds)
			}
		})
	}
}

func TestLiveHappyPath(t *testing.T) {
	w := newWorld(t)
	res, err := w.run()
	if err != nil {
		t.Fatalf("%v\n%s", err, w.outText())
	}
	if !res.Written || res.WorkerResult != "OK" || res.ReadbackSHA != w.plan.ImageSHA256 {
		t.Errorf("%+v", res)
	}
	if !bytes.Equal(w.cam.mtd[3], w.plan.Image) {
		t.Error("mtd3 is not the image")
	}
	// the rollback source staged next to the image is the STOCK backup, uploaded md5-checked before the worker started
	if !bytes.Equal(w.cam.files[sdStage+"/appfs-backup.bin"], w.backup[3]) || !bytes.Equal(w.cam.files[sdStage+"/appfs-new.bin"], w.plan.Image) {
		t.Error("staged files are wrong")
	}
	idx := func(sub string) int {
		for i, c := range w.cam.cmds {
			if strings.Contains(c, sub) {
				return i
			}
		}
		return -1
	}
	if !(idx("cat /proc/mtd") < idx("dd if=/dev/mtd5") && idx("dd if=/dev/mtd5") < idx("cat > "+sdStage+"/appfs-new.bin") &&
		idx("cat > "+sdStage+"/appfs-backup.bin") < idx("nohup sh /tmp/akam_flash_worker.sh")) {
		t.Errorf("wrong order: %q", w.cam.cmds)
	}
	w.assertNeverRebooted()
	if w.cam.count("nvram set") != 0 {
		t.Error("nvram written without the opt-in")
	}
	for n := 0; n < 6; n++ {
		if _, err := os.Stat(filepath.Join(w.workDir, fmt.Sprintf("mtd%d.bin", n))); err != nil {
			t.Errorf("fresh dump of mtd%d not kept", n)
		}
	}
	if w.slept < 15*time.Second {
		t.Error("no last-chance pause")
	}
	for _, want := range []string{"LIVE run", "Recovery net", "(f) NOT done by this program", "Reboot ONCE with the Akam SD card INSERTED", "mtd3 == intended image"} {
		if !strings.Contains(w.outText(), want) {
			t.Errorf("output lacks %q", want)
		}
	}
}

func TestStepAAbortsOnAnyDifferenceBeforeAnyWrite(t *testing.T) {
	for n := 0; n < 6; n++ {
		t.Run(fmt.Sprintf("mtd%d", n), func(t *testing.T) {
			w := newWorld(t)
			w.cam.mtd[n] = append(append([]byte(nil), w.backup[n]...), 0xaa)
			_, err := w.run()
			if n == 4 { // the jffs2 config may differ legitimately: a warning, the run goes on
				if err != nil || !strings.Contains(w.outText(), "WARNING: mtd4") {
					t.Fatalf("mtd4: %v\n%s", err, w.outText())
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), fmt.Sprintf("mtd%d differs", n)) {
				t.Fatalf("got %v", err)
			}
			if n == 3 {
				w.cam.mtd[3] = w.backup[3] // the fake holds the modified copy: restore for the unchanged check
			}
			w.assertNothingWritten()
		})
	}
	t.Run("mtd3 is not the 0x490000 appfs", func(t *testing.T) {
		w := newWorld(t)
		w.cam.mtd = w.backup
		c := &mtdLiar{fakeCam: w.cam}
		w.run2(c)
		w.assertNothingWritten()
	})
}

// mtdLiar answers /proc/mtd with another layout.
type mtdLiar struct{ *fakeCam }

func (m *mtdLiar) Run(ctx context.Context, cmd string, in io.Reader) (camssh.Result, error) {
	if cmd == "cat /proc/mtd" {
		m.cmds = append(m.cmds, cmd)
		return ok("mtd3: 00500000 00010000 \"appfs\"\n")
	}
	return m.fakeCam.Run(ctx, cmd, in)
}

func (w *world) run2(c Conn) error {
	_, err := Flash(w.plan, w.conf, Deps{Dial: func(int) (Conn, error) { return c, nil }, Out: func(l string) { w.out = append(w.out, l) },
		Sleep: func(time.Duration) {}, Ctx: context.Background()})
	if err == nil || !strings.Contains(err.Error(), "not the 0x490000 appfs partition") {
		w.t.Errorf("got %v", err)
	}
	return err
}

func TestStepBGatesAbortBeforeAnyUpload(t *testing.T) {
	cases := map[string]struct {
		mutate func(*fakeCam)
		want   string
	}{
		"no watchdog feeder":         {func(c *fakeCam) { c.noFeeder = true }, "no watchdog feeder"},
		"feeder runs from /appfs":    {func(c *fakeCam) { c.feederInAppfs = true }, "feeder runs from /appfs"},
		"card-less tmpfs bind":       {func(c *fakeCam) { c.sdMount = "tmpfs tmpfs" }, "not a real SD card mount"},
		"sd mount of another device": {func(c *fakeCam) { c.sdMount = "/dev/mtdblock4 jffs2" }, "not a real SD card mount"},
		"sd with a foreign fs":       {func(c *fakeCam) { c.sdMount = "/dev/mmcblk0p1 ext4" }, "not a real SD card mount"},
		"no sd mount at all":         {func(c *fakeCam) { c.sdMount = "" }, "not a real SD card mount"},
		"sd too full":                {func(c *fakeCam) { c.sdFreeKB = "20000" }, "need > 20 MB"},
		"memory too low":             {func(c *fakeCam) { c.memFreeKB = "4096" }, "too low"},
		"app does not stop":          {func(c *fakeCam) { c.appStays = true }, "did not stop"},
		"/appfs/lib still mapped":    {func(c *fakeCam) { c.busyLib = "812\n977\n" }, "still mapped by pid(s): 812 977"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			w := newWorld(t)
			c.mutate(w.cam)
			_, err := w.run()
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("got %v, want %q", err, c.want)
			}
			w.assertNothingWritten()
		})
	}
}

func TestUploadCorruptionAbortsBeforeTheWorker(t *testing.T) {
	w := newWorld(t)
	w.cam.corruptUploads = true
	if _, err := w.run(); err == nil || !strings.Contains(err.Error(), "corrupted") {
		t.Fatalf("got %v", err)
	}
	if w.cam.count("nohup sh /tmp/akam_flash_worker") != 0 || !bytes.Equal(w.cam.mtd[3], w.backup[3]) {
		t.Error("the worker ran after a corrupted upload")
	}
}

func TestWorkerResults(t *testing.T) {
	cases := map[string]struct {
		worker string
		want   string
	}{
		"rolled back and verified on the PC": {"ROLLED_BACK", "re-flashed and verified on the PC too"},
		"rollback failed":                    {"ROLLBACK_FAILED", "ROLLBACK FAILED. Do NOT reboot, do NOT power off"},
		"bad input":                          {"WORKER_BAD_INPUT", "did NOT flash anything"},
		"silent worker":                      {"SILENT", "no result from the worker"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			w := newWorld(t)
			w.cam.worker = c.worker
			_, err := w.run()
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("got %v, want %q", err, c.want)
			}
			// never retried, never a foreground flash, never a reboot, no step (d) / (e) after a failure
			if w.cam.workerRuns != 1 || w.cam.count("flashcp -v") != 0 || w.cam.count("nvram set") != 0 {
				t.Errorf("runs %d, cmds %q", w.cam.workerRuns, w.cam.cmds)
			}
			w.assertNeverRebooted()
			if c.worker == "ROLLBACK_FAILED" || c.worker == "SILENT" {
				if !strings.Contains(err.Error(), "akam_rollback_worker.sh") {
					t.Error("no rollback instructions")
				}
			}
		})
	}
	t.Run("the silent worker is polled for 600 s, not forever", func(t *testing.T) {
		w := newWorld(t)
		w.cam.worker = "SILENT"
		_, _ = w.run()
		if w.slept < 600*time.Second || w.slept > 700*time.Second {
			t.Errorf("slept %v", w.slept)
		}
	})
}

func TestStepDNeverRollsBackByItself(t *testing.T) {
	t.Run("complete read-back that differs", func(t *testing.T) {
		w := newWorld(t)
		w.cam.badReadback = true
		_, err := w.run()
		if err == nil || !strings.Contains(err.Error(), "COMPLETE PC read-back") || !strings.Contains(err.Error(), "NOTHING was flashed by this step") {
			t.Fatalf("got %v", err)
		}
		if w.cam.workerRuns != 1 || w.cam.count("akam_rollback_worker.sh >") != 0 {
			t.Errorf("a rollback or a second write was started: %q", w.cam.cmds)
		}
		if w.cam.count("nvram set") != 0 {
			t.Error("nvram written after a mismatch")
		}
	})
	t.Run("three short reads", func(t *testing.T) {
		w := newWorld(t)
		w.cam.shortReads = true
		_, err := w.run()
		if err == nil || !strings.Contains(err.Error(), "NO rollback was started") {
			t.Fatalf("got %v", err)
		}
		if n := w.cam.count("dd if=/dev/mtd3"); n != 1+3 {
			t.Errorf("%d dumps of mtd3 (1 in step (a), 3 read-backs expected)", n)
		}
	})
}

func TestNVRAMStepIsOptInAndVerified(t *testing.T) {
	w := newWorld(t)
	w.plan.NVRAMWrite = true
	if _, err := w.run(); err != nil {
		t.Fatalf("%v\n%s", err, w.outText())
	}
	if w.cam.nvram != w.plan.Sign {
		t.Errorf("nvram %q", w.cam.nvram)
	}
	if b, err := os.ReadFile(filepath.Join(w.workDir, "appfs_sign.before")); err != nil || strings.TrimSpace(string(b)) != "oldsign" {
		t.Errorf("the previous value was not saved: %q %v", b, err)
	}
	w = newWorld(t)
	w.plan.NVRAMWrite = true
	w.cam.nvramReadback = "other"
	if _, err := w.run(); err == nil || !strings.Contains(err.Error(), "reads back") {
		t.Fatalf("a nvram mismatch was accepted: %v", err)
	}
}

func TestUpdateMode(t *testing.T) {
	w := newWorld(t)
	// the camera already runs an Akam appfs (the "current" one)
	cur := append([]byte(nil), w.plan.Image...)
	cur[300] ^= 0xff
	cur = append(cur[:0:0], cur...)
	w.cam.mtd[3] = cur
	w.backup[3] = cur // for the helper assertions below
	w.plan.Update, w.plan.Current = true, cur
	res, err := w.run()
	if err != nil {
		t.Fatalf("%v\n%s", err, w.outText())
	}
	if !res.Written || !bytes.Equal(w.cam.files[sdStage+"/appfs-backup.bin"], cur) {
		t.Error("the rollback source of an update is the current image, not the stock backup")
	}
	t.Run("mtd3 differs from CURRENT_IMAGE", func(t *testing.T) {
		w := newWorld(t)
		cur := append([]byte(nil), w.plan.Image...)
		cur[300] ^= 0xff
		w.plan.Update, w.plan.Current = true, cur // the camera still holds the stock image
		if _, err := w.run(); err == nil || !strings.Contains(err.Error(), "differs from CURRENT_IMAGE") {
			t.Fatalf("got %v", err)
		}
		w.assertNothingWritten()
	})
	t.Run("card-less boot is refused with the update hint", func(t *testing.T) {
		w := newWorld(t)
		cur := append([]byte(nil), w.plan.Image...)
		cur[300] ^= 0xff
		w.cam.mtd[3] = cur
		w.plan.Update, w.plan.Current = true, cur
		w.cam.sdMount = "tmpfs tmpfs"
		if _, err := w.run(); err == nil || !strings.Contains(err.Error(), "UPDATE MODE: insert the Akam card") {
			t.Fatalf("got %v", err)
		}
	})
}

func hotWorld(t *testing.T) *world {
	w := newWorld(t)
	cur := append([]byte(nil), w.plan.Image...)
	cur[300] ^= 0xff
	w.cam.mtd[3] = cur
	w.plan.Update, w.plan.Hot, w.plan.Current = true, true, cur
	w.cam.hotResult = "OK"
	w.cam.status = []string{"", "H1|scan", "WAIT_wpa|x", "WAIT_dbalt|x", "WAIT_db22|x", "WAIT_upload|x", "WAIT_go|x", "WRITING|x"}
	return w
}

func TestHotUpdate(t *testing.T) {
	w := hotWorld(t)
	res, err := w.run()
	if err != nil {
		t.Fatalf("%v\n%s", err, w.outText())
	}
	if !res.Written || !bytes.Equal(w.cam.mtd[3], w.plan.Image) {
		t.Fatalf("%+v", res)
	}
	// the helper is uploaded verbatim and the runner started with the memory budget of the shell tool
	if !bytes.Equal(w.cam.files[hotSh], []byte(hotHelper)) {
		t.Error("the helper was not uploaded verbatim")
	}
	start := ""
	for _, c := range w.cam.cmds {
		if strings.Contains(c, " run hot ") {
			start = c
		}
	}
	if !strings.Contains(start, fmt.Sprintf("run hot %d %d %d", hotNeedKB, hotTmpKB, memMarginKB-1024)) || !strings.Contains(start, "AKAM_HOT_PORT=2222") {
		t.Errorf("runner start: %q", start)
	}
	// every checkpoint answered, images staged in RAM (never on a card), the stock backup is NOT the rollback source
	for _, cp := range []string{"wpa", "dbalt", "db22", "upload", "go"} {
		if w.cam.count("touch "+hotDir+"/confirm."+cp) == 0 {
			t.Errorf("checkpoint %s not answered", cp)
		}
	}
	if !bytes.Equal(w.cam.files[ramStage+"/appfs-backup.bin"], w.plan.Current) {
		t.Error("hot rollback source is not CURRENT_IMAGE")
	}
	if w.cam.count(sdStage) != 0 || w.cam.count("mmcblk") != 0 {
		t.Error("a hot update touched the SD card path")
	}
	if w.cam.count("nohup sh /tmp/akam_flash_worker") != 0 {
		t.Error("the PC started a worker: in a hot update the on-camera runner does")
	}
	foundAlt := false
	for _, p := range w.cam.dials {
		foundAlt = foundAlt || p == 2222
	}
	if !foundAlt {
		t.Error("the alternate dropbear was never used for the dbalt checkpoint")
	}
	if !strings.Contains(w.outText(), "HOT UPDATE, (f) NOT done by this program") {
		t.Error("no reboot instructions")
	}
	w.assertNeverRebooted()
}

func TestHotEndStates(t *testing.T) {
	cases := map[string]struct {
		status []string
		want   string
	}{
		"aborted and restored":    {[]string{"ABORTED_RESTORED|a process touches /appfs"}, "runner aborted: a process touches /appfs"},
		"aborted unchanged":       {[]string{"ABORTED_UNCHANGED|memory"}, "runner aborted: memory"},
		"restored without wifi":   {[]string{"ABORTED_RESTORED_NO_WIFI|no wpa"}, "NO wpa_supplicant runs"},
		"restore failed":          {[]string{"DONE_RESTORE_FAILED|x"}, "Wait 3 minutes"},
		"no status for 5 minutes": {[]string{""}, "no status from the camera for 5 minutes"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			w := hotWorld(t)
			w.cam.status = c.status
			_, err := w.run()
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("got %v, want %q", err, c.want)
			}
			if (name == "restored without wifi" || name == "restore failed") && !strings.Contains(err.Error(), "NOTHING was written") {
				t.Errorf("does not say nothing was written: %v", err)
			}
			if !bytes.Equal(w.cam.mtd[3], w.plan.Current) || w.cam.count("touch "+hotDir+"/confirm.go") != 0 {
				t.Error("a write or the go checkpoint happened")
			}
		})
	}
}

func TestHotPreflightNeverWrites(t *testing.T) {
	w := hotWorld(t)
	w.plan.Hot, w.plan.HotPreflight = false, true
	w.cam.status = []string{"WAIT_wpa|x", "WAIT_dbalt|x", "WAIT_db22|x", "DONE_RESTORED|rehearsal"}
	if _, err := w.run(); err != nil {
		t.Fatalf("%v\n%s", err, w.outText())
	}
	if w.cam.count(" run rehearsal ") != 1 || w.cam.count(" run hot ") != 0 {
		t.Errorf("mode: %q", w.cam.cmds)
	}
	for _, bad := range []string{"appfs-new.bin", "touch " + hotDir + "/confirm.go", "touch " + hotDir + "/confirm.upload", "akam_flash_worker", "dd if=/dev/mtd", "nvram set"} {
		if w.cam.count(bad) != 0 {
			t.Errorf("the rehearsal sent %q", bad)
		}
	}
	if !bytes.Equal(w.cam.mtd[3], w.plan.Current) {
		t.Error("mtd3 changed in a rehearsal")
	}
	if !strings.Contains(w.outText(), "REHEARSAL DONE") {
		t.Error("no rehearsal verdict")
	}
}

func TestHotRefusesASecondRunner(t *testing.T) {
	w := hotWorld(t)
	c := &aliveRunner{fakeCam: w.cam}
	_, err := Flash(w.plan, w.conf, Deps{Dial: func(int) (Conn, error) { return c, nil }, Out: func(string) {}, Sleep: func(time.Duration) {}, Ctx: context.Background()})
	if err == nil || !strings.Contains(err.Error(), "already alive") {
		t.Fatalf("got %v", err)
	}
	if w.cam.count(" run hot ") != 0 || w.cam.count("cat > "+hotSh) != 0 {
		t.Error("a second runner was started or the helper overwritten")
	}
}

type aliveRunner struct{ *fakeCam }

func (a *aliveRunner) Run(ctx context.Context, cmd string, in io.Reader) (camssh.Result, error) {
	if strings.Contains(cmd, "run.lock/pid") && !strings.Contains(cmd, "akam_flash.result") {
		a.cmds = append(a.cmds, cmd)
		return st(0)
	}
	return a.fakeCam.Run(ctx, cmd, in)
}

// A transport error on the first try is retried once on a fresh connection; a second one is an error.
func TestTransportRetry(t *testing.T) {
	w := newWorld(t)
	flaky := &flakyOnce{fakeCam: w.cam}
	dials := 0
	_, err := Flash(w.plan, w.conf, Deps{Dial: func(int) (Conn, error) { dials++; return flaky, nil }, Out: func(string) {}, Sleep: func(time.Duration) {}, Ctx: context.Background()})
	if err != nil {
		t.Fatal(err)
	}
	if dials < 2 {
		t.Errorf("no re-dial after a transport error (%d dials)", dials)
	}
}

type flakyOnce struct {
	*fakeCam
	done bool
}

func (f *flakyOnce) Run(ctx context.Context, cmd string, in io.Reader) (camssh.Result, error) {
	if !f.done && cmd == "cat /proc/mtd" {
		f.done = true
		return camssh.Result{}, errors.New("connection reset")
	}
	return f.fakeCam.Run(ctx, cmd, in)
}
