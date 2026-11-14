package cli

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/rtsp"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// ---------------------------------------------------------------- doctor

func (a *App) cmdDoctor() error {
	a.stepBanner("doctor")
	os_ := hostOS()
	a.UI.Say("  " + doctorHost(runtime.GOOS, runtime.GOARCH, os_, platform.OSVersion()))
	a.UI.Say(fmt.Sprintf("  Program: akam %s (%s, no runtime dependency: SSH, RTSP, G.711 and WAV are built in)", a.Version, runtime.Version()))
	a.UI.Say(fmt.Sprintf("  State folder: %s", a.Cams.Base))
	if _, err := os.Stat(a.keyPath()); err == nil {
		a.UI.Say(fmt.Sprintf("  SSH key: %s found.", a.keyPath()))
	} else {
		a.UI.Say(fmt.Sprintf("  SSH key: %s not found (the flash-card step creates it; --identity FILE uses another one).", a.keyPath()))
	}
	if st, err := os.Stat(filepath.Join(a.Cams.Base, "kit")); err == nil && st.IsDir() {
		a.UI.Say("  Kit: present.")
	} else {
		a.UI.Say("  Kit: not downloaded yet.")
	}
	opt := func(tool, why string) {
		if p, err := a.lookPath(tool); err == nil {
			a.UI.Say(fmt.Sprintf("  Optional %s: found (%s) - %s", tool, p, why))
		} else {
			a.UI.Say(fmt.Sprintf("  Optional %s: not found - %s. Install: %s", tool, why, pkgHint(a, tool)))
		}
	}
	a.UI.Say("  " + a.doctorAudio())
	for _, o := range doctorOptional(runtime.GOOS) {
		opt(o.tool, o.why)
	}
	a.UI.Say("  " + doctorElevation(runtime.GOOS, a.have))
	if runtime.GOOS == "windows" {
		if platform.VTEnabled() {
			a.UI.Say("  Console: colours and the full-screen menu work (virtual terminal mode is on).")
		} else {
			a.UI.Say("  Console: this console refuses colours; the plain text screens are used (use Windows Terminal for the full-screen menu).")
		}
	}
	if os_ == "unknown" && runtime.GOOS == "linux" {
		a.UI.Warn("only Arch Linux and Debian/Ubuntu packages are named here; other Linux systems work, install the tools with your package manager.")
	}
	_ = a.St.Set("doctor_profile", "kit")
	_ = a.St.MarkDone("doctor")
	a.UI.Good("  Host check passed (profile: kit).")
	return nil
}

// doctorHost is the "Host:" line: the operating system with its version, then the architecture.
func doctorHost(goos, goarch, linuxID, version string) string {
	arch := goarch
	switch arch {
	case "amd64":
		arch = "x86_64"
	case "arm64":
		arch = "aarch64"
	}
	switch goos {
	case "windows":
		if version == "" {
			version = "Windows"
		}
		return fmt.Sprintf("Host: %s %s", version, arch)
	case "darwin":
		if version == "" {
			version = "macOS"
		}
		return fmt.Sprintf("Host: %s %s", version, arch)
	}
	if version != "" {
		return fmt.Sprintf("Host: %s %s (%s)", linuxID, arch, version)
	}
	return fmt.Sprintf("Host: %s %s", linuxID, arch)
}

type optionalTool struct{ tool, why string }

// doctorOptional lists what is optional on each OS. xz is built in and never listed; the squashfs
// tools are only needed by the default image builder (--builder native needs none).
func doctorOptional(goos string) []optionalTool {
	// WAV, MP3, FLAC and Ogg Vorbis files and (Linux, Windows) the microphone are built in.
	why := "files other than WAV, MP3, FLAC and Ogg Vorbis for say"
	if goos == "darwin" {
		why = "the microphone for talk (macOS has no built-in capture), and other file formats for say"
	}
	out := []optionalTool{{"ffmpeg", why}}
	if goos == "linux" {
		out = append(out, optionalTool{"arecord", "microphone fallback for talk when no PulseAudio or PipeWire server runs"})
	}
	return append(out,
		optionalTool{"mksquashfs", "image building with the default builder; `--builder native` (experimental) needs none"},
		optionalTool{"unsquashfs", "image building with the default builder; `--builder native` (experimental) needs none"})
}

// doctorAudio is the "Audio:" line: what plays files and what captures the microphone here.
func (a *App) doctorAudio() string {
	files := "files: WAV, MP3, FLAC and Ogg Vorbis built in"
	mic := ""
	if a.Hooks.Capture != nil {
		return "Audio: " + files
	}
	e := a.execCapture()
	switch c, err := e.Choose(0); {
	case os.Getenv("AKAM_TALK_CAPTURE_CMD") != "":
		mic = "microphone: AKAM_TALK_CAPTURE_CMD (your command)"
	case err != nil:
		mic = "microphone: none found (" + micAdvice(runtime.GOOS) + ")"
	default:
		mic = "microphone: " + c.Name
		if c.Builtin != nil {
			mic += " (built in)"
		}
	}
	return "Audio: " + files + "; " + mic
}

func micAdvice(goos string) string {
	switch goos {
	case "linux":
		return "no PulseAudio or PipeWire server reachable"
	case "windows":
		return "no default capture device, or Windows privacy settings block it"
	}
	return "install ffmpeg"
}

// doctorElevation says how the SD card step gets the rights it needs on this OS.
func doctorElevation(goos string, have func(string) bool) string {
	switch goos {
	case "windows":
		return "Elevation: the SD card step asks Windows (a UAC prompt) for administrator rights; nothing else needs them."
	case "darwin":
		return "Elevation: the SD card step asks for your macOS password (the system prompt); nothing else needs it."
	}
	for _, t := range []string{"pkexec", "sudo"} {
		if have(t) {
			return fmt.Sprintf("Can elevate with %s (only the SD card step ever needs it).", t)
		}
	}
	return "Elevation: neither pkexec nor sudo found; the SD card step needs one of them (AKAM_SD_ELEVATE names the helper)."
}

// ---------------------------------------------------------------- self-test

func (a *App) cmdSelfTest() error {
	n, bad := 0, 0
	check := func(name string, ok bool) {
		n++
		if ok {
			a.UI.Say("  ok    " + name)
		} else {
			bad++
			a.UI.Say("  FAIL  " + name)
		}
	}
	a.UI.Bold("Self-test (offline, changes nothing)")
	check("G.711 mu-law decode 0x80 = 32124", rtsp.UlawDecode(0x80) == 32124)
	check("G.711 mu-law encode 0 = 0xFF", rtsp.UlawEncode(0) == 0xFF)
	check("G.711 A-law decode 0xD5 = 8", rtsp.AlawDecode(0xD5) == 8)
	check("G.711 A-law encode -1 = 0x55", rtsp.AlawEncode(-1) == 0x55)
	d := rtsp.Digest{User: "Mufasa", Pass: "Circle Of Life", Realm: "testrealm@host.com",
		Nonce: "dcd98b7102dd2f0e8b11d0f600bfb0c093", QopAuth: true, CNonce: func() string { return "0a4f113b" }}
	check("Digest login (RFC 2617 example)", strings.Contains(d.Header("GET", "/dir/index.html"), `response="6629fae49393a05397450978507c4ef1"`))
	h := sha256.Sum256([]byte("abc"))
	check("SHA-256 of abc", hex.EncodeToString(h[:]) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
	dir, err := os.MkdirTemp("", "akam-selftest-")
	if err == nil {
		defer os.RemoveAll(dir)
		sec := &state.Secrets{}
		sec.Add("s3cret-value")
		s, err := state.Open(dir, false, sec)
		ok := err == nil && s.Set("camera_ip", "192.0.2.1") == nil && s.Get("camera_ip") == "192.0.2.1"
		ok = ok && s.Set("x", "has s3cret-value inside") != nil // a secret is refused
		check("state file: write, read, refuse a secret", ok)
	} else {
		check("state file", false)
	}
	safe := true
	for _, c := range allRemoteCommands() {
		if forbiddenRemote(c) != "" {
			safe = false
		}
	}
	check("camera commands: no reboot, power-off or flash write", safe)
	if bad > 0 {
		return a.UI.Fail(fmt.Sprintf("%d of %d self-test checks failed.", bad, n), "this build is broken: do not use it on a camera.")
	}
	a.UI.Good(fmt.Sprintf("  Self-test passed (%d checks).", n))
	return nil
}

// ---------------------------------------------------------------- forget-host-key

func (a *App) cmdForgetHostKey(args []string) error {
	if len(args) != 1 || args[0] == "" || !validHost(args[0]) {
		return usageErr("akam: forget-host-key needs the camera's address (see " + Prog + " help)")
	}
	ip := args[0]
	file := a.knownHostsFile()
	a.banner("Forget the remembered SSH host key of " + ip)
	a.UI.Say("  Only do this when you reinstalled the camera yourself: a camera that shows another key without a reason may be an impostor.")
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD ask: Forget the host key of %s? [y/N], then remove it from %s", ip, file))
		return nil
	}
	if err := a.UI.RequireTTY("forgetting a host key"); err != nil {
		return err
	}
	ok, _ := a.UI.AskYN(fmt.Sprintf("  Forget the remembered host key of %s?", ip))
	if !ok {
		a.UI.Say("  Left as it was.")
		return nil
	}
	n, err := camssh.ForgetHostKey(file, ip)
	if err != nil {
		return a.UI.Fail("cannot update "+file+": "+err.Error(), "")
	}
	a.Log.Line("FORGET host key of %s (%d line(s) removed from %s)", ip, n, file)
	if n == 0 {
		a.UI.Say(fmt.Sprintf("  No host key was remembered for %s.", ip))
		return nil
	}
	a.UI.Good(fmt.Sprintf("  Done. The next connection to %s shows its fingerprint and asks again.", ip))
	return nil
}

// ---------------------------------------------------------------- menu

// Menuer is implemented by user interfaces that have a full-screen menu (the bubbletea one).
type Menuer interface {
	// Menu shows the entries and returns the index chosen, or -1 for quit.
	Menu(title string, items []ui.MenuItem) (int, error)
}

// Sessioner is implemented by the TUI: Session shows the full-screen session and runs fn in a
// goroutine whose ui.UI renders inside it (Say/Good/Warn/Err on the result strip, questions as
// overlays). It returns fn's error. Without a terminal it just runs fn.
type Sessioner interface {
	Session(fn func(ui.UI) error) error
}

// inSession runs run inside the session when the UI has one, with a.UI swapped for the session's UI
// (the flows keep using a.UI). done is false when there is no session: the caller then runs run itself.
func (a *App) inSession(run func() error) (done bool, err error) {
	s, ok := a.UI.(Sessioner)
	if !ok {
		return false, nil
	}
	return true, s.Session(func(u ui.UI) error {
		outer := a.UI
		a.UI = u
		defer func() { a.UI = outer }()
		return run()
	})
}

type menuEntry struct {
	item ui.MenuItem
	args []string
}

func (a *App) menuEntries() []menuEntry {
	var out []menuEntry
	_, short := a.UI.(Menuer) // the full-screen menu shows one clause; plain keeps the full text
	for i, s := range state.Steps {
		st := a.stepStatus(s)
		why := stepWhy(s)
		if short {
			why = stepWhyShort(s)
		}
		out = append(out, menuEntry{ui.MenuItem{Label: fmt.Sprintf("%d. %s", i+1, stepTitle(s)), Detail: why, Status: st}, []string{"step", s}})
	}
	tool := func(label, detail string, args ...string) {
		if short {
			detail = strings.TrimSuffix(strings.SplitN(detail, " (", 2)[0], ".")
		}
		out = append(out, menuEntry{ui.MenuItem{Label: label, Detail: detail, Status: "tool"}, args})
	}
	tool("Camera: show LEDs", "Front LED and infrared LEDs (read-only).", "led")
	tool("Camera: show speaker", "Volume, speaker and talk switches (read-only).", "audio")
	tool("Camera: show picture settings", "Brightness, contrast, saturation, sharpness (read-only).", "picture")
	tool("Camera: video password status", "Whether the stream asks for a password (read-only).", "rtsp-auth", "status")
	tool("Camera: ONVIF status", "Whether ONVIF is on (read-only).", "onvif", "status")
	tool("Camera: debug status", "Whether port 8091 accepts writes from the network (read-only).", "debug", "status")
	tool("Camera: talk test tone", "Plays a 2 second tone on the camera's speaker.", "talk", "test")
	tool("Cameras", "List the cameras you set up.", "cameras")
	return out
}

func (a *App) cmdMenu() error {
	// ./akam --camera NAME --ip ADDRESS on a camera without progress: offer to record it.
	if a.Opt.Camera != "" && a.Opt.IP != "" && !a.UI.Dry() && a.St.Get("camera_kind") == "" && a.UI.Interactive() {
		ok, _ := a.UI.AskYN(fmt.Sprintf("Camera '%s' has no progress yet. Is it a camera that ALREADY runs Akam at %s (register it, no backup or flash)?", a.Cams.Name, a.Opt.IP))
		if ok {
			if err := a.cmdRegister(); err != nil && err != ui.ErrFailed {
				return err
			}
		}
	}
	if a.UI.Dry() {
		a.statusTable()
		return nil
	}
	if !a.UI.Interactive() {
		a.statusTable()
		a.UI.Blank()
		a.UI.Say(fmt.Sprintf("No terminal attached: showing status only. Run %s in a terminal to continue.", Prog))
		return nil
	}
	return a.cmdHome()
}

// cmdStepMenu is the list of the nine steps and the read-only camera tools (akam steps): the state
// machine of the bash wizard, kept for people who want to run one step at a time.
func (a *App) cmdStepMenu() error {
	if done, err := a.inSession(a.cmdStepMenu); done { // the session UI is not a Sessioner: no recursion
		return err
	}
	var res ui.MenuResult
	for {
		entries := a.menuEntries()
		items := make([]ui.MenuItem, len(entries))
		for i, e := range entries {
			items[i] = e.item
		}
		var idx int
		var err error
		if m, ok := a.UI.(Menuer); ok {
			idx, err = a.showMenu(m, fmt.Sprintf("Akam - camera: %s", a.Cams.Name), items, res)
		} else {
			a.statusTable()
			a.UI.Blank()
			labels := make([]string, 0, len(items)+1)
			for _, it := range items {
				labels = append(labels, it.Label)
			}
			labels = append(labels, "Quit")
			idx, err = a.UI.Pick("What do you want to do?", labels)
			if err == nil && idx == len(items) {
				idx = -1
			}
		}
		if err != nil || idx < 0 {
			return nil
		}
		a.Opt.Args = entries[idx].args
		act := func() error { return a.dispatch(entries[idx].args[0]) }
		if _, ok := a.UI.(Menuer); ok {
			// The outcome is shown on the menu the user returns to, not behind a "Press Enter".
			res = a.runAction(items[idx].Label, act)
		} else {
			a.reportFlowError(act())
		}
	}
}

func asUsage(err error, target **ui.UsageError) bool { return errors.As(err, target) }
