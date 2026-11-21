package appfs

import (
	"bytes"
	"context"
	"crypto/md5" //nolint:gosec // the camera has md5sum, not sha256sum: its read-back is compared by md5
	_ "embed"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/squashfs"
)

// The camera-side scripts, byte for byte those of flash_appfs.sh (a test compares them with it). @STAGE@ is the
// staging directory: the SD card (/mnt/sdcard/akam_flash) or, for a hot update, RAM (/tmp/akam_flash).
//
//go:embed scripts/flash_worker.sh.embed
var flashWorker string

//go:embed scripts/rollback_worker.sh.embed
var rollbackWorker string

//go:embed scripts/akam_hot.sh.embed
var hotHelper string

const (
	hotDir       = "/tmp/akam_hot"
	hotSh        = hotDir + "/akam_hot.sh"
	memMarginKB  = 3072
	hotNeedKB    = 2*PartitionSize/1024 + memMarginKB
	hotTmpKB     = 2*PartitionSize/1024 + 1024
	sdStage      = "/mnt/sdcard/akam_flash"
	ramStage     = "/tmp/akam_flash"
	defaultHotSP = 2222
)

// ErrNotConfirmed is returned when a live run is asked without a valid Confirmation for the image.
var ErrNotConfirmed = errors.New("appfs flash: not confirmed (a live run needs the recovery acknowledgement and the typed go for this exact image)")

// Confirmation is the user's explicit consent for ONE live run on ONE image. Only NewConfirmation makes a valid
// one. It stands for both gates of flash_appfs.sh: AKAM_I_HAVE_RECOVERY=yes (a UART or a CH341A programmer is at hand
// and the backup is verified) and --execute.
type Confirmation struct {
	sha string
	ok  bool
}

// NewConfirmation asks twice, in order, and returns a valid Confirmation only on two explicit yes: ackRecovery (the
// recovery means exist: only they bring back a half-written mtd3) and confirm (the go, naming sha, the sha256 of the
// image, and the partition mtd3). Both must default to No and refuse without a terminal.
func NewConfirmation(sha string, ackRecovery, confirm func() (bool, error)) (Confirmation, error) {
	for _, ask := range []func() (bool, error){ackRecovery, confirm} {
		ok, err := ask()
		if err != nil {
			return Confirmation{}, err
		}
		if !ok {
			return Confirmation{}, ErrNotConfirmed
		}
	}
	return Confirmation{sha: sha, ok: true}, nil
}

// For reports whether c confirms a run of the image with this sha256.
func (c Confirmation) For(sha string) bool { return c.ok && c.sha == sha }

// Conn is what the flash needs from an SSH connection (camssh.Client implements it).
type Conn interface {
	Run(ctx context.Context, cmd string, stdin io.Reader) (camssh.Result, error)
	Close()
}

// Deps connect Flash to the world; tests replace them.
type Deps struct {
	// Dial opens an SSH connection; port 0 is the camera's normal one, another is the temporary hot dropbear.
	Dial  func(port int) (Conn, error)
	Out   func(line string) // every line, already prefixed "flash_appfs: "
	Sleep func(time.Duration)
	Ctx   context.Context
}

// Plan is one run. The zero Execute is a DRY RUN: only local read-only checks, every camera step is printed.
type Plan struct {
	Image       []byte // the built appfs image, exactly PartitionSize bytes
	ImageSHA256 string // the sha256 recorded next to the image by Build; must match
	Sign        string // the appfs_sign recorded by Build; must match the image
	BackupDir   string // the verified full dump: mtd0..mtd5.bin + SHA256SUMS
	WorkDir     string // fresh dumps and read-backs go here; must not hold mtd*.bin already, must not be BackupDir
	Target      string // display name of the camera, e.g. root@192.0.2.10
	// Current is the image believed to be on mtd3 now (Update): the rollback source, instead of the stock backup.
	Current []byte
	Update  bool
	// Hot updates from a card-less boot (needs Update); HotPreflight is the reversible rehearsal that never writes.
	Hot, HotPreflight bool
	HotPort           int  // the temporary second dropbear (default 2222)
	NVRAMWrite        bool // step (e): the optional appfs_sign write
	// OwnReboot: the caller (the update flow) gives the one instruction for the power cycle, so the final text does not
	// add a second, different one (ssh reboot). The script keeps its own text for appfs-flash.
	OwnReboot bool
	Execute   bool // live
}

// FlashResult of a run.
type FlashResult struct {
	Live          bool
	WorkerResult  string // OK | ROLLED_BACK | ... (hot: the runner's worker result)
	ReadbackSHA   string // sha256 of the mtd3 read-back (live, after a verified write)
	Written       bool   // the new image is on mtd3 and verified
	CurrentProven bool   // update mode, live: a fresh dump of mtd3 equalled Plan.Current byte for byte (step (a))
	RollbackWords string // what to do next, when a failure needs the operator
}

type runner struct {
	p      Plan
	c      Confirmation
	d      Deps
	conn   Conn
	live   bool
	stage  string
	rollN  string
	rollF  []byte // rollback source bytes
	res    FlashResult
	hotRes string
}

func (r *runner) say(f string, a ...any) { r.d.Out("flash_appfs: " + fmt.Sprintf(f, a...)) }
func (r *runner) step(f string, a ...any) {
	r.d.Out("")
	r.say("== %s ==", fmt.Sprintf(f, a...))
}
func (r *runner) fail(f string, a ...any) error { return fmt.Errorf("flash_appfs: ERROR: "+f, a...) }

func (r *runner) sleep(t time.Duration) {
	if r.d.Sleep != nil {
		r.d.Sleep(t)
		return
	}
	time.Sleep(t)
}

func md5hex(b []byte) string {
	h := md5.Sum(b) //nolint:gosec
	return hex.EncodeToString(h[:])
}

// camq runs one command on the camera (live only). A transport error is retried once on a fresh connection.
func (r *runner) camq(cmd string) (camssh.Result, error) { return r.camqIn(cmd, nil) }

func (r *runner) camqIn(cmd string, stdin []byte) (camssh.Result, error) {
	for try := 0; ; try++ {
		if r.conn == nil {
			c, err := r.d.Dial(0)
			if err != nil {
				return camssh.Result{}, err
			}
			r.conn = c
		}
		var in io.Reader
		if stdin != nil {
			in = bytes.NewReader(stdin)
		}
		res, err := r.conn.Run(r.d.Ctx, cmd, in)
		if err == nil || try > 0 || r.d.Ctx.Err() != nil {
			return res, err
		}
		r.conn.Close()
		r.conn = nil
	}
}

// cam runs a command live, or prints it in a dry run.
func (r *runner) cam(cmd string) (camssh.Result, error) {
	if !r.live {
		r.say("[dry-run] ssh %s: %s", r.p.Target, cmd)
		return camssh.Result{}, nil
	}
	return r.camq(cmd)
}

func trim(b []byte) string { return strings.TrimSpace(string(b)) }

// must runs cmd live and turns a transport error or a non-zero status into the given refusal.
func (r *runner) must(cmd, refusal string) (string, error) {
	res, err := r.camq(cmd)
	if err != nil {
		return "", r.fail("%s: %v", refusal, err)
	}
	if res.Status != 0 {
		return "", r.fail("%s", refusal)
	}
	return trim(res.Stdout), nil
}

func (r *runner) dump(n int) ([]byte, error) {
	res, err := r.camq(fmt.Sprintf("dd if=/dev/mtd%d bs=65536 2>/dev/null", n))
	if err != nil {
		return nil, err
	}
	if res.Status != 0 {
		return nil, fmt.Errorf("dd of mtd%d exited %d", n, res.Status)
	}
	return res.Stdout, nil
}

func (r *runner) upload(data []byte, remote string) error {
	res, err := r.camqIn("cat > "+remote, data)
	if err != nil || res.Status != 0 {
		return r.fail("upload to %s failed: abort, nothing written", remote)
	}
	out, err := r.camq("md5sum " + remote)
	if err != nil {
		return r.fail("cannot read back the md5 of %s: abort, nothing written", remote)
	}
	if firstField(out.Stdout) != md5hex(data) {
		return r.fail("upload of %s corrupted (md5 differs): abort, nothing written", remote)
	}
	return nil
}

func (r *runner) putWorkers() error {
	rb := strings.ReplaceAll(rollbackWorker, "@STAGE@", r.stage)
	wk := strings.ReplaceAll(flashWorker, "@STAGE@", r.stage)
	for _, w := range []struct{ body, cmd string }{
		{rb, "cat > /tmp/akam_rollback_worker.sh && chmod 755 /tmp/akam_rollback_worker.sh && rm -f /tmp/akam_rollback.result"},
		{wk, "cat > /tmp/akam_flash_worker.sh && chmod 755 /tmp/akam_flash_worker.sh && rm -f /tmp/akam_flash.result"},
	} {
		if res, err := r.camqIn(w.cmd, []byte(w.body)); err != nil || res.Status != 0 {
			return r.fail("cannot upload the on-camera workers: abort, nothing written")
		}
	}
	return nil
}

var mtd3Re = regexp.MustCompile(`(?m)^mtd3: 00490000 .* "appfs"`)

// Flash is the port of tools/flash_appfs.sh: the ONE audited place of the appfs installer that can write a 2K's
// flash. Without Plan.Execute it is a dry run (local checks only, no connection at all). A live run needs a
// Confirmation made for this image. It never reboots: step (f) is the user's, printed at the end.
func Flash(p Plan, c Confirmation, d Deps) (*FlashResult, error) {
	if d.Out == nil {
		d.Out = func(string) {}
	}
	if d.Ctx == nil {
		d.Ctx = context.Background()
	}
	r := &runner{p: p, c: c, d: d, live: p.Execute}
	r.stage = sdStage
	if p.Hot {
		r.stage = ramStage
	}
	if p.HotPreflight {
		r.p.Hot = true
		r.stage = ramStage
	}
	if r.p.HotPort == 0 {
		r.p.HotPort = defaultHotSP
	}
	p = r.p
	if p.Hot && !p.Update {
		return nil, r.fail("--hot / --hot-preflight need Update (a hot update only exists for an already flashed appfs)")
	}
	if p.Execute && !c.For(p.ImageSHA256) {
		return nil, ErrNotConfirmed
	}
	if p.Execute && d.Dial == nil {
		return nil, r.fail("no way to connect to the camera")
	}
	backup, err := r.localChecks()
	if err != nil {
		return nil, err
	}
	if p.Update {
		r.rollN = "the previous image (CURRENT_IMAGE, fresh mtd3 dump)"
	} else {
		r.rollN = "the stock backup"
		r.rollF = backup[3]
	}
	defer func() {
		if r.conn != nil {
			r.conn.Close()
		}
	}()
	r.res.Live = r.live
	if r.live {
		if err := os.MkdirAll(p.WorkDir, 0o700); err != nil {
			return nil, r.fail("cannot create the work dir: %v", err)
		}
		r.say("LIVE run. Work dir %s", p.WorkDir)
		r.lastChance()
	} else {
		r.say("DRY RUN: nothing below is executed on the camera.")
	}
	if err := r.stepA(backup); err != nil {
		return &r.res, err
	}
	if err := r.stepB(); err != nil {
		return &r.res, err
	}
	if p.HotPreflight {
		r.step("rehearsal end: restore everything that was stopped (nothing was written)")
		if r.live {
			r.say("the runner already restored everything (nothing was written)")
			r.say("REHEARSAL DONE (live). No flash write was made or attempted. Only the real hot run writes.")
		} else {
			r.say("[dry-run] restart supervisor/ntp/keeper from the captured command lines; feeder, wpa_supplicant, dropbear stay on their /tmp copies")
			r.say("dry run finished, nothing was changed. No flash write was made or attempted. Only the real hot run writes.")
		}
		return &r.res, nil
	}
	if err := r.stepC(); err != nil {
		return &r.res, err
	}
	if err := r.stepD(); err != nil {
		return &r.res, err
	}
	if err := r.stepE(); err != nil {
		return &r.res, err
	}
	r.final()
	return &r.res, nil
}

func (r *runner) lastChance() {
	p := r.p
	if p.Hot {
		r.say("Last chance, stop within 15 s. Needs: camera on stable power, CARD-LESS boot. This stops the app, the supervisor, the stub and moves the feeder, Wi-Fi and SSH to /tmp. The whole sequence then runs as ONE detached job on the camera: a loss of this program is safe, the camera restores by itself within 120 s when a confirmation does not arrive (or by hand: sh %s restore; or power-cycle: nothing was written).", hotSh)
	} else {
		r.say("Last chance, stop within 15 s. Needs: camera on stable power, Akam SD card INSERTED (do not remove it until this run finishes).")
	}
	r.say("Recovery net: the SD card only recovers an appfs that MOUNTS. A power cut or crash during the 1-2 min write leaves mtd3 half written: then only UART (sf write) or a CH341A can recover it. The recovery acknowledgement means you accept that.")
	r.sleep(15 * time.Second)
}

// localChecks are the read-only checks that run in a dry run too. It returns the six backup partitions.
func (r *runner) localChecks() ([6][]byte, error) {
	var bk [6][]byte
	p := r.p
	if p.BackupDir == "" {
		return bk, r.fail("BACKUP_DIR is required (the verified full dump)")
	}
	if st, err := os.Stat(p.BackupDir); err != nil || !st.IsDir() {
		return bk, r.fail("BACKUP_DIR is required (the verified full dump)")
	}
	if p.WorkDir == "" {
		return bk, r.fail("a work dir is required")
	}
	// Aliasing guards: the fresh dump must never overwrite or be compared with itself, the backup or CURRENT_IMAGE.
	if st, err := os.Stat(p.WorkDir); err == nil {
		if bs, e := os.Stat(p.BackupDir); e == nil && os.SameFile(st, bs) {
			return bk, r.fail("the work dir is the backup dir: refused (the backup would be overwritten)")
		}
		if m, _ := filepath.Glob(filepath.Join(p.WorkDir, "mtd*.bin")); len(m) > 0 {
			return bk, r.fail("the work dir %s already holds mtd*.bin: use a fresh directory", p.WorkDir)
		}
	}
	img := p.Image
	if len(img) != PartitionSize {
		return bk, r.fail("the image is %d bytes, not %d", len(img), PartitionSize)
	}
	if string(img[:4]) != squashMagic {
		return bk, r.fail("the image is not a squashfs")
	}
	if sum(img) != p.ImageSHA256 {
		return bk, r.fail("the image does not match its recorded sha256 (rebuild it)")
	}
	sign, err := Sign(img)
	if err != nil || strings.TrimSpace(p.Sign) == "" || sign != strings.TrimSpace(p.Sign) {
		return bk, r.fail("appfs_sign does not match the image")
	}
	if fs, err := squashfs.Open(bytes.NewReader(img), int64(len(img))); err != nil {
		return bk, r.fail("the image does not parse as a squashfs: %v", err)
	} else if err := fs.Verify(); err != nil {
		return bk, r.fail("the image does not decode: %v", err)
	}
	sums, err := os.ReadFile(filepath.Join(p.BackupDir, "SHA256SUMS"))
	if err != nil {
		return bk, r.fail("the backup has no SHA256SUMS: %v", err)
	}
	listed, err := parseSums(sums)
	if err != nil {
		return bk, r.fail("the backup SHA256SUMS is malformed: %v", err)
	}
	for n := 0; n < 6; n++ {
		name := fmt.Sprintf("mtd%d.bin", n)
		b, err := os.ReadFile(filepath.Join(p.BackupDir, name))
		if err != nil {
			return bk, r.fail("%s/%s missing", p.BackupDir, name)
		}
		bk[n] = b
	}
	if want, ok := listed["mtd3.bin"]; !ok || want != sum(bk[3]) {
		return bk, r.fail("the backup does not match its SHA256SUMS (mtd3.bin must be listed and equal)")
	}
	for name, want := range listed {
		if f, err := os.ReadFile(filepath.Join(p.BackupDir, name)); err != nil || sum(f) != want {
			return bk, r.fail("the backup does not match %s/SHA256SUMS (%s)", p.BackupDir, name)
		}
	}
	if len(bk[3]) != PartitionSize || string(bk[3][:4]) != squashMagic {
		return bk, r.fail("backup mtd3.bin is not a full appfs dump")
	}
	if p.Update {
		if len(p.Current) != PartitionSize || string(p.Current[:min(4, len(p.Current))]) != squashMagic {
			return bk, r.fail("--update: CURRENT_IMAGE is not a full %d byte appfs squashfs", PartitionSize)
		}
		if bytes.Equal(p.Current, img) {
			return bk, r.fail("--update: CURRENT_IMAGE is identical to the new image: nothing to update")
		}
		r.rollF = p.Current
	}
	r.say("image   sha256 %s  appfs_sign %s", p.ImageSHA256, p.Sign)
	r.say("backup  mtd3 sha256 %s", sum(bk[3]))
	if p.Update {
		r.say("UPDATE MODE: mtd3 must equal CURRENT_IMAGE (sha256 %s); rollback source = that image, NOT the stock backup", sum(p.Current))
		if p.Hot {
			r.say("HOT MODE: card-less boot required (no SD card); staging in RAM (%s)", ramStage)
		} else {
			r.say("UPDATE MODE needs the Akam SD card inserted at boot (recovery): insert the Akam card and reboot first")
		}
	}
	return bk, nil
}

// stepA: full re-dump of mtd0..mtd5 and byte compare with the backup.
func (r *runner) stepA(bk [6][]byte) error {
	r.step("(a) re-dump mtd0..mtd5 and compare with the backup")
	p := r.p
	if p.HotPreflight {
		r.say("skipped in the rehearsal (the rehearsal never writes; the real hot run does this step first)")
		return nil
	}
	if !r.live {
		r.say("[dry-run] ssh %s: cat /proc/mtd   (mtd3 must be 00490000 \"appfs\")", p.Target)
		r.say("[dry-run] for n in 0..5: ssh %s 'dd if=/dev/mtdN bs=65536' > WORK_DIR/mtdN.bin ; compare with the backup (mtd4: warning only)", p.Target)
		if p.Update {
			r.say("[dry-run] update mode: mtd3 is NOT compared with the stock backup but with CURRENT_IMAGE (abort on any difference); mtd0/1/2/5 still vs the stock backup")
		}
		return nil
	}
	res, err := r.camq("cat /proc/mtd")
	if err != nil || res.Status != 0 {
		return r.fail("cannot read /proc/mtd: abort, nothing written")
	}
	_ = os.WriteFile(filepath.Join(p.WorkDir, "proc_mtd.txt"), res.Stdout, 0o600)
	if !mtd3Re.Match(res.Stdout) {
		return r.fail("mtd3 is not the 0x490000 appfs partition: abort, nothing written")
	}
	for n := 0; n < 6; n++ {
		got, err := r.dump(n)
		if err != nil {
			return r.fail("cannot dump mtd%d: %v: abort, nothing written", n, err)
		}
		if err := os.WriteFile(filepath.Join(p.WorkDir, fmt.Sprintf("mtd%d.bin", n)), got, 0o600); err != nil {
			return r.fail("cannot save the dump of mtd%d: %v", n, err)
		}
		switch {
		case n == 3 && p.Update:
			if !bytes.Equal(got, p.Current) {
				return r.fail("mtd3 differs from CURRENT_IMAGE: abort, nothing written (wrong image given, or a flaky read; the dump is %s)", filepath.Join(p.WorkDir, "mtd3.bin"))
			}
			r.res.CurrentProven = true
			r.say("mtd3 identical to CURRENT_IMAGE (rollback source for this run)")
		case bytes.Equal(got, bk[n]):
			r.say("mtd%d identical to the backup", n)
		case n == 4:
			r.say("WARNING: mtd4 (jffs2 config) differs from the backup: normal if the config changed since. The fresh dump holds your Wi-Fi settings and is deleted when the run ends (it is never needed to go back)")
		default:
			return r.fail("mtd%d differs from the backup: abort, nothing written (a camera that is not the one that was backed up, or a flaky read)", n)
		}
	}
	return nil
}

// stepB: camera preflight, stop the app (the watchdog feeder stays); hot: the detached on-camera runner.
func (r *runner) stepB() error {
	r.step("(b) camera preflight, stop supervisor + app (the watchdog feeder stays)")
	p := r.p
	if p.Hot {
		if r.live {
			mode := "hot"
			if p.HotPreflight {
				mode = "rehearsal"
			}
			return r.hotRun(mode)
		}
		r.hotDry()
		return nil
	}
	if !r.live {
		r.say("[dry-run] ssh %s: ps | grep '[b]usybox watchdog'  (abort when absent); SD /mnt/sdcard/akam_flash free > 20 MB, MemFree > 4 MB", p.Target)
		r.say("[dry-run] ssh %s: kill the own_app_run.sh supervisor (NOT the stub, NOT the feeder), wait for akam2k_rtsp to exit", p.Target)
		r.say("[dry-run] ssh %s: no process may map /appfs/lib (grep /proc/*/maps) or abort", p.Target)
		return nil
	}
	const feeder = `ps | grep '[b]usybox watchdog'`
	if res, err := r.camq(feeder); err != nil || res.Status != 0 {
		return r.fail("no watchdog feeder running: abort (a flash of 1-2 minutes without a feeder = reset mid-write)")
	}
	if _, err := r.must(`for t in flashcp md5sum nohup dd; do command -v $t >/dev/null || { echo "missing on the camera: $t"; exit 1; }; done`,
		"a tool the on-camera worker needs is missing: abort, nothing written"); err != nil {
		return err
	}
	// the feeder must not execute from the appfs squashfs that is about to be erased
	if _, err := r.must(`for p in $(ps | grep "[b]usybox watchdog" | while read a b; do echo $a; done); do grep -q "/appfs/" /proc/$p/maps 2>/dev/null && exit 1; done; exit 0`,
		"the watchdog feeder runs from /appfs (it would die when mtd3 is erased): boot with the SD card overlay (Akam card inserted) and retry. Nothing written"); err != nil {
		return err
	}
	// /mnt/sdcard must be the REAL card, never the RAM bind-mount of a card-less boot: staging in RAM would
	// reintroduce the out-of-memory-mid-write risk
	sd, err := r.must(`awk "\$2==\"/mnt/sdcard\"{print \$1\" \"\$3}" /proc/mounts`, "cannot read /proc/mounts")
	if err != nil {
		return err
	}
	if !realSD(sd) {
		hint := ""
		if p.Update {
			hint = "UPDATE MODE: insert the Akam card and reboot first (a card-less boot binds a tmpfs there). "
		}
		return r.fail("/mnt/sdcard is not a real SD card mount (got '%s'): refusing to stage in RAM. %sBoot on the SD overlay. Nothing written.", sd, hint)
	}
	r.say("SD card: %s", sd)
	if _, err := r.must("mkdir -p "+sdStage, "cannot create "+sdStage+" on the card: abort, nothing written"); err != nil {
		return err
	}
	free, _ := r.camq(`df /mnt/sdcard | awk 'END{print $4}'`)
	if n, _ := strconv.Atoi(trim(free.Stdout)); n <= 20000 {
		return r.fail("SD free %s kB: need > 20 MB on the card for the two images", trim(free.Stdout))
	}
	mem, _ := r.camq(`grep MemFree /proc/meminfo | tr -cd '0-9'`)
	if n, _ := strconv.Atoi(trim(mem.Stdout)); n <= 4096 {
		return r.fail("MemFree %s kB: too low even for flashcp (need > 4 MB)", trim(mem.Stdout))
	}
	r.say("stopping the supervisor (it stops the app); the stub and the feeder keep running")
	if _, err := r.must(`for p in $(ps | grep "[o]wn_app_run.sh" | while read a b; do echo $a; done); do kill $p; done; i=0; while ps | grep -q "[/]tmp/akam2k_rtsp" && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done; ps | grep -q "[/]tmp/akam2k_rtsp" && exit 1; exit 0`,
		"the app did not stop: abort, nothing written"); err != nil {
		return err
	}
	busy, _ := r.camq(`for p in /proc/[0-9]*; do grep -q "/appfs/lib" "$p/maps" 2>/dev/null && echo "${p#/proc/}"; done; true`)
	if b := trim(busy.Stdout); b != "" {
		return r.fail("/appfs/lib is still mapped by pid(s): %s. Abort, nothing written. (A reboot returns to the normal state.)", strings.Join(strings.Fields(b), " "))
	}
	if res, err := r.camq(feeder); err != nil || res.Status != 0 {
		return r.fail("the feeder is gone")
	}
	r.say("/appfs/lib is free, feeder alive")
	return nil
}

func realSD(mount string) bool {
	f := strings.Fields(mount)
	if len(f) != 2 || !strings.HasPrefix(f[0], "/dev/mmcblk") {
		return false
	}
	return f[1] == "vfat" || f[1] == "exfat" || f[1] == "msdos"
}

// rollbackCmds are the operator's emergency copy-paste lines (the detached rollback worker, never a foreground flashcp).
func (r *runner) rollbackCmds() (string, string) {
	ssh := "ssh -o BatchMode=yes -o ConnectTimeout=10 " + r.p.Target
	return ssh + " 'nohup sh /tmp/akam_rollback_worker.sh >/tmp/akam_rollback.log 2>&1 </dev/null &'",
		ssh + " 'cat /tmp/akam_rollback.result /tmp/akam_rollback.log'"
}

// stepC: stage the two images, run the detached worker (flashcp, md5 verify, own rollback), poll its result.
func (r *runner) stepC() error {
	r.step("(c) upload to the staging dir (%s) on the camera, flashcp -v (detached worker with its own verify and rollback; hot: done by the on-camera runner, see (b))", r.stage)
	p := r.p
	if !r.live {
		r.say("[dry-run] ssh %s: cat > %s/appfs-new.bin < image ; md5 compare   (4.7 MB)", p.Target, r.stage)
		r.say("[dry-run] ssh %s: cat > %s/appfs-backup.bin < the rollback source (%s) ; md5 compare", p.Target, r.stage, r.rollN)
		r.say("[dry-run] ssh %s: also uploads /tmp/akam_rollback_worker.sh (the reusable detached rollback)", p.Target)
		r.say("[dry-run] ssh %s: detached worker (refuses to flash if an md5 is empty): flashcp -v %s/appfs-new.bin /dev/mtd3 ; md5 of /dev/mtd3 == md5 of the file ; else detached rollback (3 tries: flashcp then dd to /dev/mtdblock3, md5 verified); result in /tmp/akam_flash.result (polled up to 600 s)", p.Target, r.stage)
		return nil
	}
	rollCmd, pollCmd := r.rollbackCmds()
	nores := "no result from the worker (the camera may still be writing: do NOT power it off; look at /tmp/akam_flash.result and /tmp/akam_flash.log over ssh; a re-run is safe, step (a) aborts when mtd3 != the expected image). Only when mtd3 is really not the intended image and no worker runs, launch the DETACHED rollback worker: " + rollCmd + "  then poll: " + pollCmd
	res := ""
	if p.Hot {
		res = r.hotRes
		if res == "" {
			return r.fail("%s", nores)
		}
	} else {
		if err := r.upload(p.Image, r.stage+"/appfs-new.bin"); err != nil {
			return err
		}
		if err := r.upload(r.rollF, r.stage+"/appfs-backup.bin"); err != nil {
			return err
		}
		_, _ = r.camq("sync")
		if err := r.putWorkers(); err != nil {
			return err
		}
		r.say("WRITING mtd3 NOW. Do not power the camera off until this run says so.")
		if rs, err := r.camq("nohup sh /tmp/akam_flash_worker.sh >/tmp/akam_flash.log 2>&1 </dev/null &"); err != nil || rs.Status != 0 {
			r.say("WARNING: the ssh that launched the worker returned an error; the worker MAY be running. Do NOT power off.")
			r.say("  check over ssh: cat /tmp/akam_flash.result /tmp/akam_flash.log   (a re-run is safe: step (a) aborts when mtd3 != the expected image)")
		}
		for t := 0; t < 600 && res == ""; t += 5 {
			r.sleep(5 * time.Second)
			if rs, err := r.camq("cat /tmp/akam_flash.result 2>/dev/null"); err == nil {
				res = trim(rs.Stdout)
			}
		}
		if lg, err := r.camq("cat /tmp/akam_flash.log"); err == nil {
			r.say("camera log: %s", strings.ReplaceAll(trim(lg.Stdout), "\n", " | "))
		}
		if res == "" {
			return r.fail("%s", nores)
		}
	}
	r.res.WorkerResult = res
	r.say("worker result: %s", res)
	switch res {
	case "OK":
		return nil
	case "ROLLED_BACK":
		if back, err := r.dump(3); err == nil && bytes.Equal(back, r.rollF) {
			return r.fail("the new image failed its on-camera verify; %s was re-flashed and verified on the PC too (byte compare). Camera still booted. Aborting.", r.rollN)
		}
		return r.fail("the worker says ROLLED_BACK but the PC read-back of mtd3 is NOT %s (maybe only a flaky link). Do NOT reboot, do NOT power off. Check over ssh: dd if=/dev/mtd3 bs=65536 | md5sum  vs  md5sum %s/appfs-backup.bin. Only on a genuine difference launch the DETACHED rollback worker: %s  then poll: %s ; else UART/CH341A.", r.rollN, r.stage, rollCmd, pollCmd)
	case "WORKER_BAD_INPUT":
		return r.fail("the worker found no md5 for the image or the backup and did NOT flash anything (mtd3 untouched). Check /tmp/akam_flash.log; fix the cause and re-run.")
	default:
		return r.fail("ROLLBACK FAILED. Do NOT reboot, do NOT power off. Launch the DETACHED rollback worker NOW: %s  then poll: %s (result ROLLED_BACK|ROLLBACK_FAILED). Never a foreground flashcp. If the link is gone: UART/CH341A.", rollCmd, pollCmd)
	}
}

// stepD: read mtd3 back to the PC and byte-compare, before anything else. NON-DESTRUCTIVE: it never writes flash.
func (r *runner) stepD() error {
	r.step("(d) read mtd3 back and byte-compare with the intended image (before any reboot)")
	p := r.p
	if !r.live {
		r.say("[dry-run] ssh %s: up to 3x (sleep 5 between): dd if=/dev/mtd3 bs=65536 > WORK_DIR/mtd3.readback.bin ; the dump must be the full %d bytes, then compare with the image", p.Target, PartitionSize)
		r.say("[dry-run] complete + equal = success. complete + different = stop, NO auto-flash (launch by hand). 3 failed/short reads = stop, NO rollback (the worker md5 is authoritative; compare md5 over ssh first)")
		r.say("[dry-run] step (d) never writes flash")
		return nil
	}
	rollCmd, pollCmd := r.rollbackCmds()
	ok, complete := false, false
	var back []byte
	for try := 1; try <= 3; try++ {
		b, err := r.dump(3)
		if err == nil && len(b) == PartitionSize {
			complete, back = true, b
			ok = bytes.Equal(b, p.Image)
			break
		}
		r.say("read-back attempt %d/3 failed or short (transport error?)", try)
		if try < 3 {
			r.sleep(5 * time.Second)
		}
	}
	switch {
	case ok:
		_ = os.WriteFile(filepath.Join(p.WorkDir, "mtd3.readback.bin"), back, 0o600)
		r.res.ReadbackSHA = sum(back)
		r.res.Written = true
		r.say("mtd3 == intended image, byte for byte (sha256 %s)", r.res.ReadbackSHA)
		return nil
	case complete:
		return r.fail("a COMPLETE PC read-back of mtd3 differs from the intended image, although the worker reported %q (its own md5 verify). A genuine mismatch is unexpected. NOTHING was flashed by this step. Do NOT reboot, do NOT power off. Re-check over ssh: dd if=/dev/mtd3 bs=65536 | md5sum (image md5 %s). To roll back, run the DETACHED rollback worker, never a foreground flashcp: %s  then poll: %s (ROLLED_BACK|ROLLBACK_FAILED). Last resort: UART (sf write) or CH341A.", r.res.WorkerResult, md5hex(p.Image), rollCmd, pollCmd)
	default:
		return r.fail("3 PC read-backs of mtd3 failed or were short: a transport problem over the link, NOT a verified mismatch, so NO rollback was started. The on-camera worker already md5-verified mtd3 == the new image (worker result %q), which is authoritative. Do NOT power off. Over ssh compare: dd if=/dev/mtd3 bs=65536 | md5sum with the new image md5 %s. Only on a genuine difference run the DETACHED rollback worker: %s  then poll: %s", r.res.WorkerResult, md5hex(p.Image), rollCmd, pollCmd)
	}
}

var hexSign = regexp.MustCompile(`^[0-9a-f]+$`)

// stepE: the optional appfs_sign nvram write (the FIRST nvram write ever done on this camera).
func (r *runner) stepE() error {
	r.step("(e) appfs_sign in nvram (the FIRST nvram write ever done on this camera; optional, the boot path does not check it)")
	p := r.p
	sign := strings.TrimSpace(p.Sign)
	if !hexSign.MatchString(sign) {
		return r.fail("the appfs_sign is not hex")
	}
	if r.live && p.NVRAMWrite {
		before, err := r.camq("nvram get appfs_sign")
		if err != nil {
			return r.fail("cannot read the nvram appfs_sign: %v", err)
		}
		_ = os.WriteFile(filepath.Join(p.WorkDir, "appfs_sign.before"), []byte(trim(before.Stdout)+"\n"), 0o600)
		r.say("previous appfs_sign saved to %s (restore with: nvram set appfs_sign <old>; nvram commit)", filepath.Join(p.WorkDir, "appfs_sign.before"))
		if _, err := r.must("nvram set appfs_sign "+sign+" && nvram commit", "nvram set failed"); err != nil {
			return err
		}
		after, _ := r.camq("nvram get appfs_sign")
		if trim(after.Stdout) != sign {
			return r.fail("nvram appfs_sign reads back '%s', expected %s", trim(after.Stdout), sign)
		}
		r.say("appfs_sign set and read back")
		return nil
	}
	r.say("not done (opt in with the NVRAM option). The exact commands would be:")
	r.say("  ssh %s 'nvram get appfs_sign'   # save the old value first", p.Target)
	r.say("  ssh %s 'nvram set appfs_sign %s && nvram commit'", p.Target, sign)
	return nil
}

func (r *runner) final() {
	done := "dry run finished, nothing was written."
	if r.live {
		done = "DONE (live)."
	}
	r.d.Out("")
	r.say("%s", done)
	if r.p.OwnReboot {
		r.say("(f) NOT done by this program: the power cycle is yours, and its one instruction is given below.")
		r.say("  Make NO network change (hostname, Wi-Fi band/country) before it. Keep the Akam SD card at hand: inserted at power-up")
		r.say("  it overrides the flashed boot (recovery). A half-written mtd3 needs UART/CH341A.")
		return
	}
	if r.p.Hot {
		r.say("HOT UPDATE, (f) NOT done by this program:")
		r.say("  The new appfs is on mtd3 but this boot still has the OLD squashfs mounted: do not start anything from /appfs.")
		r.say("  Make NO network change (hostname, Wi-Fi band/country) before the reboot. Reboot it yourself now (ssh %s reboot): the camera boots card-less into the new image. Keep the Akam SD card at hand:", r.p.Target)
		r.say("  inserted at power-up it overrides the flashed boot (recovery). A half-written mtd3 needs UART/CH341A.")
		return
	}
	r.say("(f) NOT done by this program, staged reboot:")
	r.say("  0. Make NO network change (hostname, Wi-Fi band/country) before the reboot.")
	r.say("  1. Seed /config/akam over SSH BEFORE flashing (wpa.conf, authorized_keys; do it before this run if not done yet).")
	r.say("  2. Reboot ONCE with the Akam SD card INSERTED: confirms the new appfs mounts and the SD override (recovery) works.")
	r.say("  3. Only then power off, remove the card, power on: card-less boot. Keep the card at hand for recovery.")
}
