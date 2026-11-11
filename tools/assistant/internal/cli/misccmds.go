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
	"github.com/Foohx/akam-fw/tools/assistant/internal/rtsp"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// ---------------------------------------------------------------- doctor

func (a *App) cmdDoctor() error {
	a.stepBanner("doctor")
	os_ := hostOS()
	arch := runtime.GOARCH
	switch arch {
	case "amd64":
		arch = "x86_64"
	case "arm64":
		arch = "aarch64"
	}
	a.UI.Say(fmt.Sprintf("  Host: %s %s", os_, arch))
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
	opt("ffmpeg", "microphone and files other than WAV for talk and say")
	opt("arecord", "microphone for talk when ffmpeg has no sound server")
	opt("mksquashfs", "image building in a later phase")
	for _, t := range []string{"pkexec", "sudo"} {
		if _, err := a.lookPath(t); err == nil {
			a.UI.Say(fmt.Sprintf("  Can elevate with %s (only the SD card step ever needs it, in a later phase).", t))
			break
		}
	}
	if os_ == "unknown" {
		a.UI.Warn("only Arch Linux and Debian/Ubuntu are supported (macOS is planned for later). Tools are still checked.")
	}
	_ = a.St.Set("doctor_profile", "kit")
	_ = a.St.MarkDone("doctor")
	a.UI.Good("  Host check passed (profile: kit).")
	return nil
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

type menuEntry struct {
	item ui.MenuItem
	args []string
}

func (a *App) menuEntries() []menuEntry {
	var out []menuEntry
	for i, s := range state.Steps {
		st := a.stepStatus(s)
		out = append(out, menuEntry{ui.MenuItem{Label: fmt.Sprintf("%d. %s", i+1, stepTitle(s)), Detail: stepWhy(s), Status: st}, []string{"step", s}})
	}
	tool := func(label, detail string, args ...string) {
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
	for {
		entries := a.menuEntries()
		items := make([]ui.MenuItem, len(entries))
		for i, e := range entries {
			items[i] = e.item
		}
		var idx int
		var err error
		if m, ok := a.UI.(Menuer); ok {
			idx, err = m.Menu(fmt.Sprintf("Akam - camera: %s", a.Cams.Name), items)
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
		if e := a.dispatch(entries[idx].args[0]); e != nil {
			var ue *ui.UsageError
			switch {
			case e == ui.ErrFailed:
			case asUsage(e, &ue):
				a.UI.Err(ue.Msg)
			default:
				a.UI.Err(e.Error())
			}
		}
		if _, ok := a.UI.(Menuer); ok {
			_, _ = a.UI.Prompt("Press Enter to go back to the menu.", "")
		}
	}
}

func asUsage(err error, target **ui.UsageError) bool { return errors.As(err, target) }
