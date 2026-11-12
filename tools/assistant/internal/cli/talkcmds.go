package cli

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/audio"
	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/rtsp"
)

// talk and say: speak to the camera's speaker with the ONVIF audio backchannel on the RTSP port
// (specs/07 section 1). RTSP, Digest, G.711 and WAV are built in; ffmpeg or arecord are only used
// for the microphone and for files that are not WAV. The stream password is typed without echo,
// or taken from AKAM_RTSP_PASSWORD / --password-file; it is never in an argument, the state, or
// the log.

func (a *App) capturer() audio.Capturer {
	if a.Hooks.Capture != nil {
		return a.Hooks.Capture
	}
	return a.execCapture()
}

func (a *App) execCapture() *audio.Exec {
	return &audio.Exec{LookPath: a.lookPath, Shell: os.Getenv("AKAM_TALK_CAPTURE_CMD"), Builtin: audio.DefaultBackend()}
}

// talkLogin is talk_login: the stream password must be on; returns the user name and password.
func (a *App) talkLogin(ip string) (user, pass string, err error) {
	c, err := a.connectTools(ip)
	if err != nil {
		return "", "", err
	}
	defer c.Close()
	state, _ := a.rtspStatus(c)
	if state == "disabled" {
		a.UI.Say("  Talk needs the video stream password: the camera never offers talk without one.")
		a.UI.Say("  (Anyone on your network could otherwise speak through the camera.)")
		set := false
		if a.UI.Interactive() {
			set, _ = a.UI.AskYN("  Set a password now?")
		}
		if !set {
			return "", "", a.UI.Fail("no stream password, no talk.", fmt.Sprintf("run %s rtsp-auth set, then try again.", Prog))
		}
		if err := a.rtspAuthSet(c, ip); err != nil {
			return "", "", err
		}
		state, _ = a.rtspStatus(c)
	}
	switch state {
	case "enabled":
	case "invalid":
		return "", "", a.UI.Fail("the camera's credentials file is invalid.", fmt.Sprintf("run %s rtsp-auth set or disable.", Prog))
	default:
		return "", "", a.UI.Fail(fmt.Sprintf("the camera did not answer like Akam (%s missing?).", rtspAuthCmd), fmt.Sprintf("update the image (%s update).", Prog))
	}
	res, _ := a.rtspRemote(c, nil, "show")
	out := string(res.Stdout)
	user = strings.TrimPrefix(strings.TrimRight(out, "\n"), "user: ")
	if f := strings.Fields(user); len(f) > 0 {
		user = f[0]
	} else {
		user = ""
	}
	if !rtspUserValid(user) {
		return "", "", a.UI.Fail("could not read the stream user name from the camera.", fmt.Sprintf("check %s rtsp-auth status.", Prog))
	}
	pass = a.envSecret("AKAM_RTSP_PASSWORD")
	if a.Opt.PasswordFile != "" && pass == "" {
		b, rerr := os.ReadFile(a.Opt.PasswordFile)
		if rerr != nil {
			return "", "", a.UI.Fail("cannot read the password file.", "check the path given with --password-file.")
		}
		pass = strings.TrimRight(strings.SplitN(string(b), "\n", 2)[0], "\r")
	}
	if pass == "" {
		pass, err = a.UI.Secret(fmt.Sprintf("  Stream password of user %s (not shown):", user))
		if err != nil {
			return "", "", a.UI.Fail("no input.", "")
		}
	}
	a.Secrets.Add(pass)
	return user, pass, nil
}

// talkClient runs the RTSP client and maps its failures to the texts of talk_run_client.
func (a *App) talkClient(ctx context.Context, ip, user, pass string, src rtsp.PCMSource) error {
	a.Log.Line("CMD talk --user %s rtsp://%s:%d  (password not logged)", user, ip, a.Env.PortRTSP)
	err := rtsp.Talk(ctx, rtsp.TalkConfig{
		Host: ip, Port: a.Env.PortRTSP, User: user, Password: pass, Source: src,
		Unpaced: os.Getenv("AKAM_TALK_UNPACED") == "1",
		Logf:    func(f string, x ...any) { a.Log.Line("talk: "+f, x...) },
	})
	if err == nil {
		return nil
	}
	var te *rtsp.TalkError
	if !errors.As(err, &te) {
		return a.UI.Fail(err.Error(), "")
	}
	switch te.Code {
	case rtsp.CodeNet:
		return a.UI.Fail(fmt.Sprintf("the camera did not answer on port %d.", a.Env.PortRTSP), fmt.Sprintf("check %s status and the network.", Prog))
	case rtsp.CodeRefused:
		return a.UI.Fail("the camera does not offer talk.", "it needs the stream password (it is on), audio out enabled (audio.out_enable=1) and audio.talk_enable=1; see docs/troubleshooting.md (Talk).")
	case rtsp.CodeLogin:
		return a.UI.Fail("the camera refused the user name or password.", fmt.Sprintf("check %s rtsp-auth status; run %s rtsp-auth set to choose a new one.", Prog, Prog))
	case rtsp.CodeNoAudio:
		return a.UI.Fail("no audio was read from the input.", "for a file: ffmpeg could not decode it; for the microphone: check that it is not muted and is the default input.")
	}
	return a.UI.Fail(fmt.Sprintf("talk stopped with status %d: %s.", te.Code, te.Msg), "see docs/troubleshooting.md (Talk).")
}

// micMissing is the failure text when no microphone source works.
func micMissing() string {
	switch runtime.GOOS {
	case "linux":
		return "no way to read the microphone here (no PulseAudio or PipeWire server reachable, and ffmpeg or arecord not found)."
	case "windows":
		return "no way to read the microphone here (Windows audio capture failed, and ffmpeg not found)."
	}
	return "no way to read the microphone here (ffmpeg not found; macOS needs it for the microphone)."
}

func (a *App) noToolFail(what string) error {
	hint := fmt.Sprintf("%s; or set AKAM_TALK_CAPTURE_CMD to a command that writes raw 8 kHz mono s16le on stdout. %s talk test and %s say FILE.wav do not need a microphone or ffmpeg.", pkgHint(a, "ffmpeg"), Prog, Prog)
	return a.UI.Fail(what, hint)
}

// talkContext ends the talk on Ctrl-C (INT) and on TERM/HUP (then the program exits 143/129).
func (a *App) talkContext() (context.Context, context.CancelFunc) {
	ctx, cancel := context.WithCancel(a.Ctx)
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, platform.SignalsToIgnore()...)
	stop := make(chan struct{})
	go func() {
		select {
		case s := <-ch:
			if code := platform.ExitCodeFor(s); code != 130 {
				a.exitOverride = code
			}
			cancel()
		case <-stop:
		}
	}()
	return ctx, func() { signal.Stop(ch); close(stop); cancel() }
}

func (a *App) cmdTalk(args []string) error {
	arg := ""
	secs, test := 0, false
	if len(args) > 1 {
		return usageErr(fmt.Sprintf("akam: talk takes at most one word (see %s help)", Prog))
	}
	if len(args) == 1 {
		arg = args[0]
	}
	switch {
	case arg == "":
	case arg == "test":
		test = true
	case !allDigits(arg) || strings.HasPrefix(arg, "0"):
		return usageErr(fmt.Sprintf("akam: talk needs nothing, a number of seconds (1 to 3600) or 'test' (see %s help)", Prog))
	default:
		n, _ := strconv.Atoi(arg)
		if n > 3600 {
			return usageErr("akam: talk: at most 3600 seconds")
		}
		secs = n
	}
	a.banner("Talk to the camera")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD check over SSH that the stream password is on (%s status) and read the user name (show).", rtspAuthCmd))
		a.UI.Say("  WOULD ask the stream password (not shown) unless AKAM_RTSP_PASSWORD is set.")
		if test {
			a.UI.Say(fmt.Sprintf("  WOULD run: akam-talk --user <user> --tone 2 rtsp://%s:%d   (password in the environment)", ip, a.Env.PortRTSP))
		} else {
			a.UI.Say(fmt.Sprintf("  WOULD run: <microphone as 8 kHz mono s16le> | akam-talk --user <user> --format s16le rtsp://%s:%d", ip, a.Env.PortRTSP))
		}
		return nil
	}
	// Check the microphone tool before asking for anything.
	var mic io.ReadCloser
	ctx, done := a.talkContext()
	defer done()
	if !test {
		ch, e := a.micProbe(secs)
		if e != nil {
			return a.noToolFail(micMissing())
		}
		a.Log.Line("talk: microphone source: %s", ch.Name)
		a.UI.Say("  Microphone: " + ch.Name)
	}
	user, pass, err := a.talkLogin(ip)
	if err != nil {
		return err
	}
	if test {
		a.UI.Say("  Sending a 2 second test tone to the speaker ...")
		if err := a.talkClient(ctx, ip, user, pass, &rtsp.SliceSource{S: rtsp.Tone(2)}); err != nil {
			return err
		}
		a.UI.Good(fmt.Sprintf("  Done. Heard it? If not: %s audio (volume, beep) and docs/troubleshooting.md, audio output.", Prog))
		return nil
	}
	mic, err = a.capturer().Mic(ctx, secs)
	if err != nil {
		return a.noToolFail(micMissing())
	}
	if secs == 0 {
		a.UI.Say("  Speak now. The camera plays your voice; its own microphone is muted while you speak.")
		a.UI.Say("  Press Ctrl-C to stop.")
	} else {
		a.UI.Say(fmt.Sprintf("  Speak now, for at most %d seconds.", secs))
	}
	a.Log.Line("CMD capture | talk")
	tctx := ctx
	if secs > 0 {
		var c2 context.CancelFunc
		tctx, c2 = context.WithTimeout(ctx, secondsDuration(secs))
		defer c2()
	}
	// Ctrl-C (or the end of the time) closes the capture first, so a blocked read returns. The
	// goroutine is always finished before this command returns.
	stopMic := make(chan struct{})
	micDone := make(chan struct{})
	go func() {
		defer close(micDone)
		select {
		case <-tctx.Done():
		case <-stopMic:
		}
		mic.Close()
	}()
	defer func() { close(stopMic); <-micDone }()
	if err := a.talkClient(tctx, ip, user, pass, &rtsp.S16LESource{R: mic}); err != nil {
		return err
	}
	a.UI.Good("  Talk ended.")
	return nil
}

// micProbe only checks that a microphone source exists (the real capture starts after the login).
func (a *App) micProbe(secs int) (audio.Choice, error) {
	if a.Hooks.Capture != nil {
		return audio.Choice{Name: "test"}, nil
	}
	return a.execCapture().Choose(secs)
}

// maxSayFile bounds the audio file read into memory by say (a 30 minute stereo WAV is about 320 MB at
// 48 kHz, but the camera speaker needs far less: longer files should be cut).
const maxSayFile = 256 << 20

func (a *App) cmdSay(args []string) error {
	if len(args) != 1 || args[0] == "" {
		return usageErr(fmt.Sprintf("akam: say needs one audio file (see %s help)", Prog))
	}
	file := args[0]
	a.banner("Play a file on the camera")
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD decode %s to 8 kHz mono (built in for WAV, MP3, FLAC and Ogg Vorbis; ffmpeg for anything else) and send it to rtsp://%s:%d (stream password needed).", file, ip, a.Env.PortRTSP))
		return nil
	}
	if st, serr := os.Stat(file); serr == nil && st.Size() > maxSayFile {
		return a.UI.Fail(fmt.Sprintf("%s is too large (%d MiB; the limit is %d MiB).", file, st.Size()>>20, maxSayFile>>20), "cut it first, or convert it with ffmpeg to a smaller file.")
	}
	data, rerr := os.ReadFile(file)
	if rerr != nil {
		return a.UI.Fail(fmt.Sprintf("cannot read %s.", file), "give the path of an audio file.")
	}
	var src rtsp.PCMSource
	var conv io.ReadCloser
	ctx, done := a.talkContext()
	defer done()
	if bytes.HasPrefix(data, []byte("RIFF")) {
		if pcm, werr := rtsp.ReadWAV(bytes.NewReader(data)); werr == nil {
			src = &rtsp.SliceSource{S: pcm}
		} else if !a.have("ffmpeg") && a.Hooks.Capture == nil {
			return a.UI.Fail(fmt.Sprintf("cannot use %s: %v.", file, werr), "give a PCM WAV, MP3, FLAC or Ogg Vorbis file, or install ffmpeg to convert other formats: "+pkgHint(a, "ffmpeg"))
		}
	}
	if src == nil {
		// MP3, FLAC and Ogg Vorbis are decoded in process (found by their first bytes, not the name)
		pcm, derr := audio.Decode(data)
		switch {
		case derr == nil:
			src = &rtsp.SliceSource{S: pcm}
		case !errors.Is(derr, audio.ErrUnsupported):
			if !a.have("ffmpeg") && a.Hooks.Capture == nil {
				return a.UI.Fail(fmt.Sprintf("cannot use %s: %v.", file, derr), "if the file is fine, install ffmpeg to convert it: "+pkgHint(a, "ffmpeg"))
			}
		}
	}
	data = nil
	if src == nil {
		if a.Hooks.Capture == nil && !a.have("ffmpeg") {
			return a.UI.Fail(fmt.Sprintf("ffmpeg is needed to convert %s.", file), pkgHint(a, "ffmpeg")+"; or give a WAV, MP3, FLAC or Ogg Vorbis file.")
		}
		var cerr error
		conv, cerr = a.capturer().Convert(ctx, file)
		if cerr != nil {
			return a.UI.Fail(fmt.Sprintf("ffmpeg is needed to convert %s.", file), pkgHint(a, "ffmpeg")+"; or give a WAV, MP3, FLAC or Ogg Vorbis file.")
		}
		defer conv.Close()
		src = &rtsp.S16LESource{R: conv}
	}
	user, pass, err := a.talkLogin(ip)
	if err != nil {
		return err
	}
	a.UI.Say(fmt.Sprintf("  Playing %s on the camera ...", file))
	if err := a.talkClient(ctx, ip, user, pass, src); err != nil {
		return err
	}
	a.UI.Good("  Done.")
	return nil
}

var _ = camssh.ErrAuth
