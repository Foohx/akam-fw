// Package flash is the ONE audited place of the program that can write a camera's flash. It is the
// Go port of tools/flash_scripts/reflash_over_ssh.sh: it uploads the image, flash_tool and the
// camera-side scripts to the camera's RAM over SSH, has the camera start a second Dropbear on port
// 2222 from RAM, starts the flash fully detached on the camera, polls its log, verifies the
// read-back md5 of /dev/mtd7 and then tells the user to power-cycle BY HAND.
//
// Safety design (each point has a test):
//   - nothing else in the program may send a flash command: the P1 guard (cli.forbiddenRemote)
//     still refuses flash_tool, mtd writes and reboots on every other path, and a source test checks
//     that the "start" command and the confirmation flag exist in this package only;
//   - Flash needs a Confirmation, a value only NewConfirmation can make, and only from an explicit
//     yes (default No, refused without a terminal, bound to one image md5);
//   - nothing here ever reboots, powers off or kills anything: the command set is closed (see
//     the constants below) and a test checks every command the fake camera received;
//   - the image must be exactly 5242880 bytes and its md5 must equal the one given, checked on this
//     computer, then sha256 on the camera after the upload, then md5 again by the camera script
//     (prepare), then once more by the detached worker just before the erase;
//   - after the write the camera script verifies the read-back itself and this package asks for a
//     second, independent read-back: only when both equal the image md5 is the flash reported done;
//   - a failure after the flash started always says "DO NOT power-cycle".
package flash

import (
	"bytes"
	"context"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// ImageBytes is the size of the APP partition (mtd7): an image must have exactly this size.
const ImageBytes = 5242880

// DefaultRemoteDir is the RAM work directory on the camera.
const DefaultRemoteDir = "/tmp/akam_reflash"

const (
	confirmFlag = "--i-understand-no-uart"
	eolMark     = "=== akam-end-of-log"
	probeOK     = "second-session-ok"
)

// Conn is what the flash needs from an SSH connection (camssh.Client implements it).
type Conn interface {
	Run(ctx context.Context, cmd string, stdin io.Reader) (camssh.Result, error)
	Close()
}

// Deps connect the flash to the world; tests replace them.
type Deps struct {
	// Dial opens an SSH connection to the camera's port (the normal Dropbear or the RAM one).
	Dial func(port int) (Conn, error)
	// Out receives every line the reflash prints ("reflash_over_ssh: ..." and "camera: ...").
	Out func(line string)
	// Sleep waits between polls and retries (nil: time.Sleep).
	Sleep func(time.Duration)
	Ctx   context.Context
}

// Plan is one flash.
type Plan struct {
	Image      string // exactly ImageBytes bytes
	MD5        string // 32 lowercase hex digits: the md5 of Image
	FlashTool  string // the ARM flash_tool binary
	ScriptsDir string // "" = the embedded scripts (always, in a release build)
	Port       int    // normal Dropbear (default 22)
	SecondPort int    // RAM Dropbear (default 2222)
	RemoteDir  string // RAM work dir (default DefaultRemoteDir)
	Poll       time.Duration
	Timeout    time.Duration
	// RetrySleep is the wait between the logins on the second port (default 2 s).
	RetrySleep time.Duration
}

// Confirmation is the human's explicit yes for ONE flash. Only NewConfirmation makes a valid one.
type Confirmation struct {
	md5 string
	ok  bool
}

// NewConfirmation asks (through ask, which must be a yes/no whose default is No and that refuses
// without a terminal) and returns a valid Confirmation only on an explicit yes.
func NewConfirmation(md5 string, ask func() (bool, error)) (Confirmation, error) {
	yes, err := ask()
	if err != nil {
		return Confirmation{}, err
	}
	if !yes {
		return Confirmation{}, ErrNotConfirmed
	}
	return Confirmation{md5: md5, ok: true}, nil
}

// For reports whether c is the human's explicit yes for the image with this md5 (the telnet route
// uses it to gate the one "YES" typed to the camera's own flash script).
func (c Confirmation) For(md5 string) bool { return c.ok && c.md5 == md5 }

// ErrNotConfirmed: the user did not say yes.
var ErrNotConfirmed = errors.New("not confirmed")

// Error is a failure already worded for the user (the text of reflash_over_ssh.sh's die).
type Error struct{ Msg string }

func (e *Error) Error() string { return e.Msg }

func die(format string, a ...any) error { return &Error{Msg: fmt.Sprintf(format, a...)} }

var (
	md5Re   = regexp.MustCompile(`^[0-9a-f]{32}$`)
	rdirRe  = regexp.MustCompile(`^/[A-Za-z0-9_./-]+$`)
	progRe  = regexp.MustCompile(`^\s*([A-Za-z][A-Za-z +/-]*): \[[=> ]*\]\s*([0-9]{1,3})%`)
	resultR = regexp.MustCompile(`RESULT: `)
)

type runner struct {
	p     Plan
	d     Deps
	in    *inputs
	conns map[int]Conn

	progLabel string
	progLast  int
}

func (r *runner) say(format string, a ...any) {
	r.d.Out("reflash_over_ssh: " + fmt.Sprintf(format, a...))
}

func (r *runner) sleep(d time.Duration) {
	if r.d.Sleep != nil {
		r.d.Sleep(d)
		return
	}
	select {
	case <-time.After(d):
	case <-r.d.Ctx.Done():
	}
}

func (r *runner) close() {
	for _, c := range r.conns {
		c.Close()
	}
	r.conns = nil
}

// rssh runs one command line on the camera's port; the camera's /bin/sh interprets it.
func (r *runner) rssh(port int, cmd string, stdin io.Reader, timeout time.Duration) (camssh.Result, error) {
	c := r.conns[port]
	if c == nil {
		var err error
		if c, err = r.d.Dial(port); err != nil {
			return camssh.Result{}, err
		}
		r.conns[port] = c
	}
	ctx, cancel := context.WithTimeout(r.d.Ctx, timeout)
	defer cancel()
	res, err := c.Run(ctx, cmd, stdin)
	if err != nil {
		c.Close() // reconnect on the next command
		delete(r.conns, port)
	}
	return res, err
}

func (r *runner) rdir() string { return r.p.RemoteDir }

// validate is the local part of reflash_over_ssh.sh: nothing touches the camera before it passes.
func validate(p *Plan) (*inputs, error) {
	if p.Port == 0 {
		p.Port = 22
	}
	if p.SecondPort == 0 {
		p.SecondPort = 2222
	}
	if p.RemoteDir == "" {
		p.RemoteDir = DefaultRemoteDir
	}
	if p.Poll == 0 {
		p.Poll = 5 * time.Second
	}
	if p.Timeout == 0 {
		p.Timeout = 900 * time.Second
	}
	if p.RetrySleep == 0 {
		p.RetrySleep = 2 * time.Second
	}
	if !md5Re.MatchString(p.MD5) {
		return nil, die("--md5 must be 32 lowercase hex digits")
	}
	if !rdirRe.MatchString(p.RemoteDir) || strings.Contains(p.RemoteDir, "..") {
		return nil, die("--remote-dir must be a plain absolute path")
	}
	if !strings.HasPrefix(p.RemoteDir, "/tmp/") || len(p.RemoteDir) <= len("/tmp/") {
		return nil, die("--remote-dir must be below /tmp (the camera's RAM disk)")
	}
	st, err := os.Stat(p.Image)
	if err != nil || !st.Mode().IsRegular() {
		return nil, die("image not found: %s", p.Image)
	}
	if t, err := os.Stat(p.FlashTool); err != nil || !t.Mode().IsRegular() {
		return nil, die("flash tool not found: %s", p.FlashTool)
	}
	if p.ScriptsDir != "" { // test builds only; otherwise the scripts embedded in the binary are sent
		for _, f := range []string{"akam_reflash_ssh.sh", "flash_guard.sh"} {
			if _, err := os.Stat(filepath.Join(p.ScriptsDir, f)); err != nil {
				return nil, die("%s missing", filepath.Join(p.ScriptsDir, f))
			}
		}
	}
	if st.Size() != ImageBytes {
		return nil, die("image is %d bytes, expected exactly %d (full APP partition)", st.Size(), ImageBytes)
	}
	// Everything that will be sent is read ONCE, here, into memory; the md5 and the sha256 are
	// computed over these very bytes, so a file replaced after the check cannot change what is
	// uploaded (the camera also compares the md5 itself).
	in := &inputs{}
	if in.image, err = readLimited(p.Image, ImageBytes); err != nil {
		return nil, die("cannot read the image: %v", err)
	}
	if len(in.image) != ImageBytes {
		return nil, die("image is %d bytes, expected exactly %d (full APP partition)", len(in.image), ImageBytes)
	}
	sum := md5.Sum(in.image)
	if got := hex.EncodeToString(sum[:]); got != p.MD5 {
		return nil, die("image md5 is %s, you said %s: refusing", got, p.MD5)
	}
	if in.tool, err = readLimited(p.FlashTool, maxToolBytes); err != nil {
		return nil, die("cannot read %s: %v", p.FlashTool, err)
	}
	if in.script, err = p.script("akam_reflash_ssh.sh"); err != nil {
		return nil, die("cannot read akam_reflash_ssh.sh: %v", err)
	}
	if in.guard, err = p.script("flash_guard.sh"); err != nil {
		return nil, die("cannot read flash_guard.sh: %v", err)
	}
	return in, nil
}

// script is a camera-side script: embedded in the binary, or read from ScriptsDir (tests only).
func (p Plan) script(name string) ([]byte, error) {
	if p.ScriptsDir == "" {
		return assets.Script(name)
	}
	return readLimited(filepath.Join(p.ScriptsDir, name), maxScriptBytes)
}

const (
	maxToolBytes   = 8 << 20
	maxScriptBytes = 1 << 20
)

// inputs are the bytes that are sent to the camera.
type inputs struct{ image, tool, script, guard []byte }

// readLimited reads a regular file, at most max bytes (one more is read to notice an overflow, and
// a longer file is an error).
func readLimited(path string, max int64) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, max+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > max {
		return nil, fmt.Errorf("larger than %d bytes", max)
	}
	return b, nil
}

func newRunner(p Plan, d Deps) (*runner, error) {
	in, err := validate(&p)
	if err != nil {
		return nil, err
	}
	if d.Ctx == nil {
		d.Ctx = context.Background()
	}
	return &runner{p: p, d: d, in: in, conns: map[int]Conn{}, progLast: -1}, nil
}

// busySnippet is run on the camera before anything is uploaded: it refuses (exit 3) while a
// worker or its lock is alive and otherwise removes only the files this package uploads.
const busySnippet = `busy=0
for f in RDIR/worker.pid RDIR/worker.lock/pid; do
    p=; [ -s "$f" ] && read -r p <"$f"
    case "$p" in "" | *[!0-9]*) ;; *) kill -0 "$p" 2>/dev/null && busy=1 ;; esac
done
[ -d RDIR/worker.lock ] && [ ! -s RDIR/worker.lock/pid ] && busy=1
[ "$busy" = 0 ] || exit 3
umask 077
mkdir -p RDIR && cd RDIR || exit 1
rm -f akam_reflash_ssh.sh flash_guard.sh flash_tool image.squashfs flash.log worker.pid
rm -rf worker.lock`

func (r *runner) upload(data []byte, remote string) error {
	res, err := r.rssh(r.p.Port, fmt.Sprintf("umask 077; cat > '%s/%s'", r.rdir(), remote), bytes.NewReader(data), 5*time.Minute)
	if err != nil || res.Status != 0 {
		return die("upload of %s failed", remote)
	}
	return nil
}

func (r *runner) camSh(args string) (camssh.Result, error) {
	return r.rssh(r.p.Port, fmt.Sprintf("sh '%s/akam_reflash_ssh.sh' %s", r.rdir(), args), nil, 2*time.Minute)
}

// stage1 uploads the scripts and runs the camera pre-checks (nothing is written to flash).
func (r *runner) stage1(precheckOnly bool) error {
	rdir := r.rdir()
	r.say("uploading the camera-side scripts to %s", rdir)
	res, err := r.rssh(r.p.Port, strings.ReplaceAll(busySnippet, "RDIR", rdir), nil, time.Minute)
	switch {
	case err == nil && res.Status == 3:
		return die("a flash worker is still running or starting in %s on the camera: nothing touched. Follow %s/flash.log over port %d, do not power-cycle.", rdir, rdir, r.p.SecondPort)
	case err != nil || res.Status != 0:
		return die("cannot prepare %s on the camera", rdir)
	}
	if err := r.upload(r.in.script, "akam_reflash_ssh.sh"); err != nil {
		return err
	}
	if err := r.upload(r.in.guard, "flash_guard.sh"); err != nil {
		return err
	}
	r.say("camera pre-checks (mtd7 = APP 00500000, free RAM)")
	res, err = r.camSh("precheck")
	r.showRaw(res)
	if err != nil || res.Status != 0 {
		return die("camera pre-check failed, nothing was written")
	}
	if precheckOnly {
		r.say("precheck-only: done, nothing written. Re-run with --i-understand-no-uart to flash.")
	}
	return nil
}

// showRaw prints what a camera script said (the real script's lines are not prefixed).
func (r *runner) showRaw(res camssh.Result) {
	for _, l := range strings.Split(strings.TrimRight(res.Combined(), "\n"), "\n") {
		if l != "" {
			r.d.Out(l)
		}
	}
}

// Precheck is `--precheck-only`: it uploads the two small scripts to the camera's RAM and runs the
// camera pre-checks. Nothing is written to flash.
func Precheck(p Plan, d Deps) error {
	r, err := newRunner(p, d)
	if err != nil {
		return d.fail(err)
	}
	defer r.close()
	return d.fail(r.stage1(true))
}

func (d Deps) fail(err error) error {
	var e *Error
	if errors.As(err, &e) && d.Out != nil {
		d.Out("reflash_over_ssh: ERROR: " + e.Msg)
	}
	return err
}

// Flash writes the camera's APP partition. It needs the human's Confirmation for this image.
// It returns nil only when the camera's read-back md5 of /dev/mtd7 equals the image md5.
func Flash(p Plan, c Confirmation, d Deps) error {
	if !c.ok || c.md5 != p.MD5 {
		return d.fail(die("refusing to flash without an explicit confirmation for this image"))
	}
	r, err := newRunner(p, d)
	if err != nil {
		return d.fail(err)
	}
	defer r.close()
	return d.fail(r.flash())
}

func (r *runner) flash() error {
	p := r.p
	rdir := r.rdir()
	if err := r.stage1(false); err != nil {
		return err
	}
	// stage 2: image and tool to RAM, sha256 compared on both sides
	r.say("uploading flash_tool and the image to RAM")
	if err := r.upload(r.in.tool, "flash_tool"); err != nil {
		return err
	}
	if err := r.upload(r.in.image, "image.squashfs"); err != nil {
		return err
	}
	for _, pair := range []struct {
		data   []byte
		remote string
	}{{r.in.image, "image.squashfs"}, {r.in.tool, "flash_tool"}} {
		sum := sha256.Sum256(pair.data) // the bytes that were uploaded, not a second read of the file
		l := hex.EncodeToString(sum[:])
		res, err := r.rssh(p.Port, fmt.Sprintf("sha256sum '%s/%s'", rdir, pair.remote), nil, time.Minute)
		remote := ""
		if err == nil {
			if f := strings.Fields(string(res.Stdout)); len(f) > 0 {
				remote = f[0]
			}
		}
		if l != remote {
			none := remote
			if none == "" {
				none = "none"
			}
			return die("sha256 mismatch for %s (local %s, camera %s): nothing was written", pair.remote, l, none)
		}
		r.say("sha256 OK for %s (%s)", pair.remote, l)
	}
	r.say("preparing: busybox + dropbear copied to RAM, second Dropbear on port %d", p.SecondPort)
	res, err := r.camSh("prepare --md5 " + p.MD5)
	r.showRaw(res)
	if err != nil || res.Status != 0 {
		return die("prepare failed on the camera, nothing was written")
	}

	// stage 3: the independent session
	rbb := rdir + "/bin/busybox"
	ok2 := false
	for i := 0; i < 5; i++ {
		res, err := r.rssh(p.SecondPort, rbb+" echo "+probeOK, nil, 30*time.Second)
		if err == nil && res.Status == 0 && strings.Contains(string(res.Stdout), probeOK) {
			ok2 = true
			break
		}
		r.sleep(p.RetrySleep)
	}
	if !ok2 {
		return die("cannot log in on port %d (the RAM Dropbear): nothing was written, aborting before the flash", p.SecondPort)
	}
	r.say("port %d session works and does not depend on /usr", p.SecondPort)

	// stage 4: the flash, detached on the camera. From here on an interruption can leave APP half written.
	r.say("starting the detached flash (from now on an interruption can leave APP half written)")
	res, err = r.rssh(p.SecondPort, fmt.Sprintf("%s sh '%s/akam_reflash_ssh.sh' start --md5 '%s' %s", rbb, rdir, p.MD5, confirmFlag), nil, 2*time.Minute)
	if err != nil || res.Status != 0 {
		return die("start failed (see the camera log %s/flash.log); check the flash state before anything else", rdir)
	}

	// stage 5: poll
	result, waited, dead := r.poll(rbb)
	switch {
	case strings.Contains(result, "RESULT: OK"):
	case result == "":
		return die("no result from the flash worker (worker dead: %d, waited %ds). DO NOT power-cycle. Inspect %s/flash.log over port %d.", dead, int(waited.Seconds()), rdir, p.SecondPort)
	default:
		return die("the flash worker reported: %s. DO NOT power-cycle; re-run, or recover with UART.", result)
	}

	// stage 6: independent read-back
	var v string
	for i := 0; i < 3; i++ {
		res, err := r.rssh(p.SecondPort, fmt.Sprintf("%s sh '%s/akam_reflash_ssh.sh' verify --md5 '%s'", rbb, rdir, p.MD5), nil, 10*time.Minute)
		// As in the bash driver, the output of a failed attempt (a MISMATCH exits 1) is kept: the
		// last attempt's READBACK_MD5 line is what the error message reports.
		v = ""
		if err == nil {
			v = string(res.Stdout)
			if res.Status == 0 {
				break
			}
		}
		r.sleep(p.RetrySleep)
	}
	rb := ""
	for _, l := range strings.Split(v, "\n") {
		if s, ok := strings.CutPrefix(l, "READBACK_MD5="); ok {
			rb = s
		}
	}
	if rb == p.MD5 && strings.Contains(v, "VERIFY=MATCH") {
		r.say("read-back md5 of /dev/mtd7 = %s, equal to the image.", rb)
		r.say("VERIFIED. Now power-cycle BY HAND: unplug and replug USB-C (never use 'reboot').")
		return nil
	}
	if rb == "" {
		rb = "unavailable"
	}
	return die("independent read-back md5 is '%s', expected %s. DO NOT power-cycle.", rb, p.MD5)
}

// poll follows the camera's flash.log by byte offset and consumes only COMPLETE lines (flash_tool
// redraws its progress bar with carriage returns on one very long line).
func (r *runner) poll(rbb string) (result string, waited time.Duration, dead int) {
	p := r.p
	rdir := r.rdir()
	printed, fails := 0, 0
	for waited <= p.Timeout {
		cmd := fmt.Sprintf("%s tail -c +%d '%s/flash.log'; printf '\\n%s\\n'; %s sh '%s/akam_reflash_ssh.sh' status | %s grep '^worker='",
			rbb, printed+1, rdir, eolMark, rbb, rdir, rbb)
		res, err := r.rssh(p.SecondPort, cmd, nil, 30*time.Second)
		out := string(res.Stdout)
		if err == nil && strings.Contains(out, eolMark) {
			fails = 0
			tail := out[strings.LastIndex(out, eolMark)+len(eolMark):]
			wline := ""
			for _, l := range strings.Split(tail, "\n") {
				if strings.HasPrefix(l, "worker=") {
					wline = l
				}
			}
			chunk := out
			if i := strings.Index(out, "\n"+eolMark); i >= 0 {
				chunk = out[:i]
			}
			nw := chunk[:strings.LastIndex(chunk, "\n")+1]
			if nw != "" {
				r.showCamera(nw)
				printed += len(nw)
			}
			if resultR.MatchString(nw) {
				for _, l := range strings.Split(nw, "\n") {
					if resultR.MatchString(l) {
						result = l
					}
				}
				return result, waited, dead
			}
			if wline == "worker=dead" {
				dead++
				if dead >= 2 {
					return "", waited, dead
				}
			} else {
				dead = 0
			}
		} else {
			fails++
			r.say("poll failed (%d in a row); the camera may be busy or the session dropped, retrying", fails)
		}
		r.sleep(p.Poll)
		waited += p.Poll
	}
	return "", waited, dead
}

// showCamera prints a chunk of flash.log; each progress bar is folded to one line per 10 percent.
func (r *runner) showCamera(text string) {
	for _, l := range strings.Split(strings.ReplaceAll(text, "\r", "\n"), "\n") {
		if m := progRe.FindStringSubmatch(l); m != nil {
			pct, _ := strconv.Atoi(strings.TrimLeft(m[2], "0"))
			if m[1] != r.progLabel {
				r.progLabel, r.progLast = m[1], -1
			}
			if r.progLast < 0 || pct >= r.progLast+10 || (pct >= 100 && r.progLast < 100) {
				r.progLast = pct
				r.d.Out(fmt.Sprintf("camera: %s: %d%%", m[1], pct))
			}
		} else if l != "" {
			r.d.Out("camera: " + l)
		}
	}
}
