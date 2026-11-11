// Package cli holds the command dispatcher and one file per sub-command. Commands only talk to
// the user through ui.UI and to the camera through App.dial, so they run unchanged against the
// plain UI, the bubbletea one and the fake camera of the tests.
package cli

import (
	"bufio"
	"context"
	_ "embed"
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/assets"
	"github.com/Foohx/akam-fw/tools/assistant/internal/audio"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/telnet"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

//go:embed help.txt
var helpText string

func usageErr(s string) error { return ui.Usage(s) }

// Streams are the process streams (replaced in tests).
type Streams struct {
	In  io.Reader
	Out io.Writer
	Err io.Writer
}

// Hooks replace the parts that touch the host (microphone, ffmpeg, terminal widgets) in tests.
type Hooks struct {
	// Capture starts the microphone (or file conversion); nil uses the real tools.
	Capture audio.Capturer
	// LookPath finds an executable; nil uses exec.LookPath.
	LookPath func(string) (string, error)
	// UI builds the user interface; nil builds the plain one (or the bubbletea one on a terminal).
	UI func(o *Options, st Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI
	// Discovery hooks (find-camera).
	Discover DiscoverHooks
	// HTTP is the client of get-kit and of the toolkit download; nil uses the real one (HTTPS only, TLS 1.2 or newer).
	HTTP *http.Client
	// SD is the microSD card layer; nil uses the real one (guards on /sys, writes through `akam sd-helper`).
	SD sdcard.Backend
}

// App is one run of the program.
type App struct {
	Opt     *Options
	Env     Env
	UI      ui.UI
	St      *state.Store
	Cams    *state.Cameras
	Log     *state.Log
	Secrets *state.Secrets
	Version string
	Streams Streams
	Hooks   Hooks
	Ctx     context.Context

	camIP        string
	tn           *telnet.Session // the maintenance telnet session (backup, flash)
	passphrase   *string
	exitOverride int
}

func (a *App) out() io.Writer { return a.Streams.Out }

func (a *App) lookPath(name string) (string, error) {
	if a.Hooks.LookPath != nil {
		return a.Hooks.LookPath(name)
	}
	return exec.LookPath(name)
}

func (a *App) have(name string) bool {
	_, err := a.lookPath(name)
	return err == nil
}

// Exit statuses: 0 ok, 1 failure, 2 usage.
const (
	ExitOK    = 0
	ExitFail  = 1
	ExitUsage = 2
)

// Run is the whole program: it returns the exit status.
func Run(argv []string, st Streams, version string, hooks Hooks) int {
	if st.In == nil {
		st.In = os.Stdin
	}
	if st.Out == nil {
		st.Out = os.Stdout
	}
	if st.Err == nil {
		st.Err = os.Stderr
	}
	errln := func(s string) { fmt.Fprintln(st.Err, s) }
	opt, err := ParseArgs(argv)
	if err != nil {
		errln(err.Error())
		return ExitUsage
	}
	cmd := ""
	if len(opt.Args) > 0 {
		cmd = opt.Args[0]
	}
	switch cmd {
	case "help":
		fmt.Fprint(st.Out, helpText)
		return ExitOK
	case "licenses", "licences":
		fmt.Fprint(st.Out, assets.Notices())
		return ExitOK
	case "version":
		fmt.Fprintf(st.Out, "akam %s (%s/%s, %s)\n", version, runtime.GOOS, runtime.GOARCH, runtime.Version())
		return ExitOK
	}
	if opt.Camera != "" && !state.NameExists(opt.Camera) {
		errln(fmt.Sprintf("akam: --camera needs a short name: lowercase letters, digits and '-', at most %d, not starting or ending with '-'", state.NameMax))
		return ExitUsage
	}
	base := opt.StateDir
	if base == "" {
		base = state.DefaultDir()
	}
	if opt.Camera != "" && !state.NameValid(opt.Camera) && !state.CameraExists(base, opt.Camera) {
		errln("akam: --camera: '" + opt.Camera + "' is a reserved name on Windows (con, prn, aux, nul, com1-9, lpt1-9); pick another one")
		return ExitUsage
	}
	cams, err := state.InitCameras(base, opt.Camera, opt.DryRun)
	if err != nil {
		errln("akam: cannot use the state directory " + base + ": " + err.Error())
		return ExitFail
	}
	lock, ok, err := state.Acquire(base)
	if err != nil {
		errln("akam: cannot lock " + base + ": " + err.Error())
		return ExitFail
	}
	if !ok {
		errln("akam: another akam is already working on " + base + " (do not run the bash wizard and the binary at the same time).")
		return ExitFail
	}
	defer lock.Release()

	secrets := &state.Secrets{}
	a := &App{Opt: opt, Env: LoadEnv(), Cams: cams, Secrets: secrets, Version: version, Streams: st, Hooks: hooks}
	var cancel context.CancelFunc
	a.Ctx, cancel = context.WithCancel(context.Background())
	defer cancel()

	// UI before the first-run questions (they are asked through it); the log is attached later.
	mk := hooks.UI
	if mk == nil {
		mk = defaultUI
	}
	a.UI = mk(opt, st, nil, secrets, opt.DryRun)

	if cmd == "" {
		if a.firstRunName() || a.firstRunLegacy() {
			cams.Remember(cams.Name)
		}
	}
	dir := cams.DirOf(cams.Name)
	a.St, err = state.Open(dir, opt.DryRun, secrets)
	if err != nil {
		errln("akam: cannot open the state in " + dir + ": " + err.Error())
		return ExitFail
	}
	a.Log, err = state.NewLog(dir, secrets)
	if err != nil {
		errln("akam: cannot open the log in " + dir + ": " + err.Error())
		return ExitFail
	}
	if !opt.DryRun {
		if state.BackupBeforeBinary(dir) {
			a.Log.Line("MIGRATE kept state.bak-before-binary")
		}
	}
	a.UI = mk(opt, st, a.Log, secrets, opt.DryRun)
	if opt.Camera != "" {
		_ = cams.Remember(cams.Name)
	}
	_ = a.St.Set("tool", "go")
	_ = a.St.Set("tool_version", version)
	a.Log.Line("CAMERA %s", cams.Name)
	dry := 0
	if opt.DryRun {
		dry = 1
	}
	label := cmd
	if label == "" {
		label = "menu"
	}
	a.Log.Line("START akam %s dry=%d", label, dry)
	if opt.DryRun {
		a.UI.Say("REHEARSAL (--dry-run): commands are shown, nothing is changed.")
		if cmd == "" {
			_ = a.St.ResetDry()
		}
	}

	err = a.dispatch(cmd)
	rc := exitCode(a, err)
	if a.exitOverride != 0 {
		rc = a.exitOverride
	}
	a.Log.Line("END rc=%d", rc)
	return rc
}

func exitCode(a *App, err error) int {
	var ue *ui.UsageError
	switch {
	case err == nil:
		return ExitOK
	case errors.As(err, &ue):
		fmt.Fprintln(a.Streams.Err, ue.Msg)
		return ExitUsage
	case errors.Is(err, ui.ErrFailed):
		return ExitFail
	case errors.Is(err, context.Canceled):
		return 130
	default:
		a.UI.Err(err.Error())
		return ExitFail
	}
}

func defaultUI(o *Options, st Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
	return NewPlainUI(o, st, log, secrets, dry)
}

// NewPlainUI builds the line-based UI on the given streams (colour only on a terminal).
func NewPlainUI(o *Options, st Streams, log *state.Log, secrets *state.Secrets, dry bool) *ui.Plain {
	p := &ui.Plain{
		In: bufio.NewReader(st.In), Out: st.Out, ErrOut: st.Err, DryRun: dry,
		ForceTT: testhook.Env("AKAM_FORCE_TTY") == "1", Log: log, Secrets: secrets,
	}
	if f, ok := st.In.(*os.File); ok {
		p.InFile = f
	}
	if f, ok := st.Err.(*os.File); ok {
		p.ErrFile = f
	}
	if f, ok := st.Out.(*os.File); ok && os.Getenv("NO_COLOR") == "" && colorTerm() {
		p.Color = isTerminal(f) && platform.VTEnabled()
	}
	return p
}

// colorTerm is false for TERM=dumb or an empty TERM; Windows consoles set no TERM and use VT mode.
func colorTerm() bool {
	if runtime.GOOS == "windows" {
		return os.Getenv("TERM") != "dumb"
	}
	return os.Getenv("TERM") != "dumb" && os.Getenv("TERM") != ""
}

func (a *App) dispatch(cmd string) error {
	args := []string(nil)
	if len(a.Opt.Args) > 1 {
		args = a.Opt.Args[1:]
	}
	switch cmd {
	case "":
		return a.cmdMenu()
	case "status":
		a.statusTable()
		return nil
	case "steps":
		if a.UI.Dry() || !a.UI.Interactive() {
			a.statusTable()
			return nil
		}
		return a.cmdStepMenu()
	case "cameras":
		return a.cmdCameras()
	case "rename-camera":
		if len(a.Opt.Args) < 2 || a.Opt.Args[1] == "" {
			return usageErr("akam: rename-camera needs the new name")
		}
		return a.cmdRenameCamera(a.Opt.Args[1])
	case "register":
		return a.cmdRegister()
	case "doctor":
		return a.cmdDoctor()
	case "self-test":
		return a.cmdSelfTest()
	case "forget-host-key":
		return a.cmdForgetHostKey(args)
	case "find-camera":
		return a.cmdFindCamera()
	case "hostname":
		name := ""
		if len(args) > 0 {
			name = args[0]
		}
		return a.cmdHostname(name)
	case "rtsp-auth":
		act := "status"
		if len(args) > 0 {
			act = args[0]
		}
		return a.cmdRtspAuth(act)
	case "led":
		return a.cmdLed(args)
	case "debug":
		return a.cmdDebug(args)
	case "onvif":
		return a.cmdOnvif(args)
	case "audio":
		return a.cmdAudio(args)
	case "picture":
		return a.cmdPicture(args)
	case "video":
		return a.cmdVideo(args)
	case "ntp":
		return a.cmdNtp(args)
	case "talk":
		return a.cmdTalk(args)
	case "say":
		return a.cmdSay(args)
	case "step":
		if len(args) < 1 || !stepValidName(args[0]) {
			return usageErr("akam: step needs a name: " + strings.Join(state.Steps, " "))
		}
		return a.runStep(args[0])
	case "get-kit":
		return a.cmdGetKit()
	case "update":
		return a.cmdUpdate()
	case "restore":
		return a.cmdRestore()
	case "verify":
		return a.cmdVerify()
	case "prepare-backup-sd":
		return a.cmdPrepareBackupSD()
	case "backup":
		return a.cmdBackup()
	case "prepare-flash-sd":
		return a.cmdPrepareFlashSD()
	case "wipe-sd":
		return a.cmdWipeSD()
	case "flash":
		return a.cmdFlash()
	case "image":
		if a.Opt.Image != "" { // an image built elsewhere (T11.13, interim until the native builder)
			return a.installImage()
		}
		return a.notYet(cmd)
	case "build":
		return a.notYet(cmd)
	}
	return usageErr(fmt.Sprintf("akam: unknown command '%s' (see %s help)", cmd, Prog))
}

// notYet explains that a command still lives in the bash wizard (phases P2 and P3 of the plan).
func (a *App) notYet(cmd string) error {
	a.UI.Err(fmt.Sprintf("'%s' is not in the native binary yet (it comes in a later phase).", cmd))
	a.UI.Hint(fmt.Sprintf("use the bash wizard from a source checkout: ./akam %s", cmd))
	return ui.ErrFailed
}

func (a *App) runStep(name string) error {
	switch name {
	case "doctor":
		return a.cmdDoctor()
	case "find-camera":
		return a.cmdFindCamera()
	case "get-kit":
		return a.cmdGetKit()
	case "verify":
		return a.cmdVerify()
	case "prepare-backup-sd":
		return a.cmdPrepareBackupSD()
	case "backup":
		return a.cmdBackup()
	case "prepare-flash-sd":
		return a.cmdPrepareFlashSD()
	case "flash":
		return a.cmdFlash()
	}
	return a.notYet("step " + name)
}

// banner is step_banner_free: a blank line and a bold title.
func (a *App) banner(title string) { a.UI.Banner(title) }

// ---- camera access ----

const dryIP = "<camera-ip>"

// camNeedIP is cam_need_ip: --ip, else the address in the state.
func (a *App) camNeedIP() (string, error) {
	ip := a.Opt.IP
	if ip == "" {
		ip = a.St.Get("camera_ip")
	}
	if ip == "" {
		if a.UI.Dry() {
			a.camIP = dryIP
			return dryIP, nil
		}
		return "", a.UI.Fail("the camera address is not known yet.", fmt.Sprintf("run: %s step find-camera (or pass --ip ADDRESS)", Prog))
	}
	a.camIP = ip
	if a.UI.Dry() {
		return ip, nil
	}
	if !validHost(ip) {
		return "", a.UI.Fail(fmt.Sprintf("'%s' is not a valid camera address.", ip), "use the IP address from your router's list.")
	}
	return ip, nil
}

// validHost is cam_valid_host: an IPv4 address or a plain host name, never an option.
func validHost(s string) bool {
	if s == "" || strings.HasPrefix(s, "-") {
		return false
	}
	for _, r := range s {
		if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '.' || r == '-') {
			return false
		}
	}
	return true
}

// keyPath is cam_key.
func (a *App) keyPath() string {
	return camssh.KeyPathFor(a.Opt.Identity, a.Opt.Pubkey, a.St.Get("ssh_pubkey"))
}

func (a *App) knownHostsFile() string { return filepath.Join(a.Cams.Base, "known_hosts") }

func userKnownHosts() string {
	h, _ := os.UserHomeDir()
	if h == "" {
		return ""
	}
	return filepath.Join(h, ".ssh", "known_hosts")
}

// dial opens the SSH connection to the camera (cam_ssh_setup + a login).
func (a *App) dial(ip string) (*camssh.Client, error) { return a.dialPort(ip, a.Env.PortSSH) }

// dialPort is dial on another port (the RAM Dropbear of the reflash): same key, and the same pin
// because the pin is stored under the camera's address.
func (a *App) dialPort(ip string, port int) (*camssh.Client, error) {
	cfg := camssh.Config{
		Host: ip, Port: port, KeyPath: a.keyPath(),
		KnownHosts: a.knownHostsFile(), UserKnownHosts: userKnownHosts(), Alias: ip,
		TrustFingerprint: a.Opt.TrustNewHostKey,
		Timeout:          10 * time.Second,
		Passphrase: func() (string, error) {
			if a.passphrase != nil { // asked once per run, not once per connection
				return *a.passphrase, nil
			}
			p, err := a.UI.Secret(fmt.Sprintf("  Passphrase of the SSH key %s (not shown):", a.keyPath()))
			if err == nil {
				a.passphrase = &p
			}
			return p, err
		},
		Confirm: a.confirmHostKey,
	}
	return camssh.Dial(cfg)
}

// confirmHostKey is the first contact with a camera: show the fingerprint, one yes/no (default No).
func (a *App) confirmHostKey(nh camssh.NewHostKey) (bool, error) {
	a.UI.Say(fmt.Sprintf("  First SSH connection to this camera. Its host key fingerprint: %s", nh.Fingerprint))
	a.UI.Say(fmt.Sprintf("  It will be remembered in %s (as %s) and checked at every connection.", nh.PinFile, nh.Alias))
	q := "  Trust this camera's host key?"
	if nh.FromUserKnownHosts {
		a.UI.Say("  The same key is already trusted in your ~/.ssh/known_hosts (as " + nh.Alias + ").")
		q = "  Import it into Akam's own list of trusted cameras?"
	}
	ok, err := a.UI.AskYN(q)
	if err != nil {
		return false, fmt.Errorf("no terminal to ask (pass --trust-new-host-key %s to accept it)", nh.Fingerprint)
	}
	return ok, nil
}

// connect is rtsp_connect: IP, key and a working SSH login, or a clear failure. who selects the
// failure wording of the caller.
func (a *App) connect(ip string, failMsg string, failHint string) (*camssh.Client, error) {
	a.Log.Line("CMD(ro) ssh root@%s true", ip)
	c, err := a.dial(ip)
	if err != nil {
		return nil, a.dialFailure(ip, err, failMsg, failHint)
	}
	return c, nil
}

func (a *App) dialFailure(ip string, err error, msg, hint string) error {
	var hk *camssh.HostKeyChangedError
	switch {
	case errors.As(err, &hk):
		a.UI.Err(fmt.Sprintf("REMOTE HOST IDENTIFICATION HAS CHANGED for %s: the camera now shows %s.", ip, hk.Fingerprint))
		a.UI.Say("  Someone may be impersonating the camera, or the camera was reinstalled (new host key).")
		if hk.KnownHosts != "" && hk.KnownHosts == userKnownHosts() {
			return a.UI.Fail("refusing to connect.", fmt.Sprintf("the old key is in %s; if you reinstalled the camera yourself, run: ssh-keygen -R %s, then try again.", hk.KnownHosts, ip))
		}
		return a.UI.Fail("refusing to connect.", fmt.Sprintf("if you reinstalled the camera yourself, run: %s forget-host-key %s, then try again.", Prog, ip))
	case errors.Is(err, camssh.ErrHostKeyRefused):
		return a.UI.Fail(fmt.Sprintf("the host key of %s was not accepted: %v", ip, err), "nothing was sent to the camera. Run the command again in a terminal to review the fingerprint.")
	}
	a.Log.Line("SSH error: %v", err)
	return a.UI.Fail(msg, hint)
}

// run executes a camera command; the safety guard refuses anything that is not a plain read or a
// helper call.
func (a *App) run(c *camssh.Client, cmd string, stdin io.Reader, logCmd string) (camssh.Result, error) {
	if why := forbiddenRemote(cmd); why != "" {
		return camssh.Result{}, errors.New(why)
	}
	if logCmd == "" {
		logCmd = cmd
	}
	a.Log.Line("CMD ssh root@%s %s", c.Host, logCmd)
	ctx, cancel := context.WithTimeout(a.Ctx, 60*time.Second)
	defer cancel()
	return c.Run(ctx, cmd, stdin)
}

func joinNonEmpty(parts ...string) string {
	var out []string
	for _, p := range parts {
		if p != "" {
			out = append(out, p)
		}
	}
	return strings.Join(out, " ")
}
