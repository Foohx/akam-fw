// Package audio runs the optional external tools of talk and say: ffmpeg or arecord for the
// microphone, ffmpeg to decode files the binary cannot read itself. Everything else (WAV, G.711,
// RTSP) is built in. A capture process is always stopped on every way out: Close kills its whole
// process group, and on Linux the kernel kills it too if this process dies (Pdeathsig).
package audio

import (
	"context"
	"errors"
	"io"
	"os/exec"
	"runtime"
	"strconv"
	"sync"
	"time"
)

// ErrNoTool means no microphone or conversion tool was found.
var ErrNoTool = errors.New("no capture tool")

// Capturer starts a source of raw 8 kHz mono signed 16-bit little-endian audio.
type Capturer interface {
	// Mic captures the default microphone for at most seconds (0 = until closed).
	Mic(ctx context.Context, seconds int) (io.ReadCloser, error)
	// Convert decodes any file ffmpeg reads.
	Convert(ctx context.Context, path string) (io.ReadCloser, error)
}

// Exec is the real Capturer.
type Exec struct {
	LookPath func(string) (string, error)
	// Shell is a command from AKAM_TALK_CAPTURE_CMD (run with sh -c); empty uses the detection.
	Shell string
	GOOS  string
}

func (e *Exec) look(n string) bool {
	lp := e.LookPath
	if lp == nil {
		lp = exec.LookPath
	}
	_, err := lp(n)
	return err == nil
}

// MicCommand builds the capture command line (talk_capture in steps_camera.sh).
func (e *Exec) MicCommand(seconds int) ([]string, error) {
	if e.Shell != "" {
		return []string{"sh", "-c", e.Shell}, nil
	}
	goos := e.GOOS
	if goos == "" {
		goos = runtime.GOOS
	}
	var cmd []string
	switch goos {
	case "linux":
		switch {
		case e.look("ffmpeg") && e.look("pactl") && exec.Command("pactl", "info").Run() == nil:
			cmd = []string{"ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "pulse", "-i", "default"}
		case e.look("arecord"):
			cmd = []string{"arecord", "-q", "-t", "raw", "-f", "S16_LE", "-r", "8000", "-c", "1"}
			if seconds != 0 {
				cmd = append(cmd, "-d", strconv.Itoa(seconds))
			}
			return cmd, nil
		case e.look("ffmpeg"):
			cmd = []string{"ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "alsa", "-i", "default"}
		}
	case "darwin":
		if e.look("ffmpeg") {
			cmd = []string{"ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "avfoundation", "-i", ":default"}
		}
	case "windows":
		if e.look("ffmpeg") {
			cmd = []string{"ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "dshow", "-i", "audio=default"}
		}
	}
	if len(cmd) == 0 {
		return nil, ErrNoTool
	}
	if seconds != 0 {
		cmd = append(cmd, "-t", strconv.Itoa(seconds))
	}
	// Speech normaliser: a headset microphone is far quieter than the test tone (tuned by ear in
	// the bash wizard, kept as is).
	cmd = append(cmd, "-af", "speechnorm=e=12.5:r=0.0001:l=1:p=0.6", "-ar", "8000", "-ac", "1", "-f", "s16le", "-")
	return cmd, nil
}

// ConvertCommand builds the ffmpeg command that decodes a file to 8 kHz mono s16le.
func (e *Exec) ConvertCommand(path string) ([]string, error) {
	if !e.look("ffmpeg") {
		return nil, ErrNoTool
	}
	return []string{"ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path, "-ar", "8000", "-ac", "1", "-f", "s16le", "-"}, nil
}

func (e *Exec) Mic(ctx context.Context, seconds int) (io.ReadCloser, error) {
	argv, err := e.MicCommand(seconds)
	if err != nil {
		return nil, err
	}
	return Start(ctx, argv)
}

func (e *Exec) Convert(ctx context.Context, path string) (io.ReadCloser, error) {
	argv, err := e.ConvertCommand(path)
	if err != nil {
		return nil, err
	}
	return Start(ctx, argv)
}

// Proc is a running capture process; reading gives its stdout.
type Proc struct {
	cmd     *exec.Cmd
	out     io.ReadCloser
	done    chan struct{}
	once    sync.Once
	stopped chan struct{}
}

// Start runs argv in its own process group with stdout piped and stderr discarded. The process
// is killed when ctx ends or Close is called.
func Start(ctx context.Context, argv []string) (*Proc, error) {
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Stdin = nil
	cmd.Stderr = nil
	setProcAttr(cmd)
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	p := &Proc{cmd: cmd, out: out, done: make(chan struct{}), stopped: make(chan struct{})}
	go func() {
		select {
		case <-ctx.Done():
			p.Close()
		case <-p.done:
		}
	}()
	return p, nil
}

func (p *Proc) Read(b []byte) (int, error) { return p.out.Read(b) }

// Close stops the process group (TERM, then KILL after a second) and waits for it. It is safe to
// call more than once.
func (p *Proc) Close() error {
	p.once.Do(p.stop)
	<-p.stopped
	return nil
}

func (p *Proc) stop() {
	close(p.done)
	defer close(p.stopped)
	terminate(p.cmd, false)
	waited := make(chan struct{})
	go func() { _ = p.cmd.Wait(); close(waited) }()
	select {
	case <-waited:
	case <-time.After(time.Second):
		terminate(p.cmd, true)
		<-waited
	}
}
