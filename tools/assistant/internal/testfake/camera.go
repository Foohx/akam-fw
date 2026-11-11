// Package testfake is the fake camera of the tests: an in-process SSH server that runs the real
// camera helper scripts (tools/firmware_patch_templates) in a temporary root file system, a fake
// control port 8091 reached through direct-tcpip (and also listening on a real port, for port
// probes), and a fake RTSP server with Digest login and the ONVIF audio backchannel. It never
// listens outside the loopback, never runs anything but the closed set of commands below, and
// removes everything it started when the test ends.
package testfake

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	gossh "golang.org/x/crypto/ssh"
)

// Options select what the fake camera offers.
type Options struct {
	// AuthorizedKey is the public key allowed to log in (required).
	AuthorizedKey gossh.PublicKey
	// NoHostnameHelper makes "test -x /usr/sbin/akam_hostname.sh" fail (an older image).
	NoHostnameHelper bool
	// NoOnvifHelper makes akam_onvif.sh "not found".
	NoOnvifHelper bool
	// NoNtpHelper makes akam_ntp.sh "not found" (an older image).
	NoNtpHelper bool
	// NoCtl closes the control port answers (nothing answered).
	NoCtl bool
	// Flash selects the behaviour of the reflash emulation (see flash.go); zero is a healthy camera.
	Flash FlashOptions
}

// Camera is one running fake camera.
type Camera struct {
	t    testing.TB
	opts Options
	Root string // AKAM_ROOT of the helper scripts

	sshLn, ctlLn, rtspLn net.Listener
	ssh2Ln               net.Listener // the second Dropbear (RAM, port 2222 on a real camera)
	fl                   flashState
	hostKeyMu            sync.Mutex
	hostSigner           gossh.Signer

	mu       sync.Mutex
	commands []string
	ctl      map[string]string
	ctlSets  []string
	onvifOn  bool
	ntpOn    bool
	ntpList  string
	vRun     map[string]string // the video values the running ak_rtsp uses (applied at its restart)
	onvifPrt string
	restarts int
	conns    []net.Conn
	wg       sync.WaitGroup
	done     chan struct{}

	RTSP *RTSPServer
}

// SSHPort, CtlPort and RTSPPort are the real loopback ports.
func (c *Camera) SSHPort() int { return c.sshLn.Addr().(*net.TCPAddr).Port }
func (c *Camera) CtlPort() int { return c.ctlLn.Addr().(*net.TCPAddr).Port }

// SecondPort is the port of the RAM Dropbear (answers only after the camera script "prepare").
func (c *Camera) SecondPort() int { return c.ssh2Ln.Addr().(*net.TCPAddr).Port }
func (c *Camera) RTSPPort() int   { return c.rtspLn.Addr().(*net.TCPAddr).Port }

// Commands returns the exec command lines received so far.
func (c *Camera) Commands() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]string(nil), c.commands...)
}

// CtlSets returns the SET lines the control port received.
func (c *Camera) CtlSets() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]string(nil), c.ctlSets...)
}

// HostFingerprint is the SHA256 fingerprint of the current host key.
func (c *Camera) HostFingerprint() string {
	c.hostKeyMu.Lock()
	defer c.hostKeyMu.Unlock()
	return gossh.FingerprintSHA256(c.hostSigner.PublicKey())
}

// RotateHostKey gives the camera a new host key (a reinstall, or an impostor).
func (c *Camera) RotateHostKey() {
	c.hostKeyMu.Lock()
	c.hostSigner = newSigner(c.t)
	c.hostKeyMu.Unlock()
}

func newSigner(t testing.TB) gossh.Signer {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	s, err := gossh.NewSignerFromKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// TemplatesDir is tools/firmware_patch_templates of this checkout.
func TemplatesDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "..", "firmware_patch_templates")
}

// HelpersAvailable reports whether the real helper scripts can run here (a POSIX sh and the
// scripts of this checkout).
func HelpersAvailable() bool {
	if _, err := exec.LookPath("sh"); err != nil {
		return false
	}
	for _, s := range []string{"akam_rtsp_auth.sh", "akam_hostname.sh", "akam_debug.sh"} {
		if _, err := os.Stat(filepath.Join(TemplatesDir(), s)); err != nil {
			return false
		}
	}
	return true
}

const stubRtsp = `#!/bin/sh
# stub of "ak_rtsp ctl LINE..." for the fake camera (the debug flag lives in a file)
[ "$1" = ctl ] || exit 2
shift
f=$AKAM_ROOT/debug.state
case "$1 $2" in
    "DEBUG on") echo "$3" >"$f"; echo "OK debug on" ;;
    "DEBUG off") rm -f "$f"; echo "OK debug off" ;;
    "DEBUG status")
        if [ -f "$f" ]; then echo "ctl.debug=1 remaining=$(( $(cat "$f") * 60 ))"; else echo "ctl.debug=0"; fi ;;
    "ONVIF reload") echo "OK" ;;
    *) echo "ERR unknown command" ;;
esac
`

// New starts a fake camera; everything stops with the test.
func New(t testing.TB, opts Options) *Camera {
	t.Helper()
	if opts.AuthorizedKey == nil {
		t.Fatal("testfake: AuthorizedKey is required")
	}
	c := &Camera{t: t, opts: opts, done: make(chan struct{}), hostSigner: newSigner(t),
		onvifPrt: "80",
		ctl: map[string]string{
			"led.status": "auto", "led.state": "idle", "night.ir_led": "auto",
			"audio.out_volume": "60", "audio.out_enable": "1", "audio.talk_enable": "1", "audio.out_state": "idle",
			"audio.out_underruns": "0", "audio.out_drops": "0", "audio.out_test": "0",
			"isp.brightness": "0", "isp.contrast": "0", "isp.saturation": "0", "isp.sharpness": "0",
			"img.wb": "conf", "night.mode": "auto", "audio.mic_enable": "1",
			"video.bitrate_cfg": "1536", "video.fps_cfg": "15", "video.gop_cfg": "2", "video.cfg_pending": "0",
			"video.bitrate": "1480", "video.fps_actual": "150",
		}}
	c.vRun = map[string]string{"video.bitrate_cfg": "1536", "video.fps_cfg": "15", "video.gop_cfg": "2"}
	c.Root = t.TempDir()
	if err := os.MkdirAll(filepath.Join(c.Root, "etc", "config"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(c.Root, "bin"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(c.Root, "bin", "ak_rtsp"), []byte(stubRtsp), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(c.Root, "bin", "true_cmd"), []byte("#!/bin/sh\nexit 0\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	var err error
	if c.sshLn, err = net.Listen("tcp", "127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	if c.ctlLn, err = net.Listen("tcp", "127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	if c.ssh2Ln, err = net.Listen("tcp", "127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	c.initFlash()
	c.RTSP = newRTSPServer(t, filepath.Join(c.Root, "etc", "config", "akam", "credentials"))
	c.rtspLn = c.RTSP.ln
	c.serve(c.sshLn, c.handleSSH)
	c.serve(c.ssh2Ln, c.handleSSH2)
	c.serve(c.ctlLn, c.handleCtl)
	t.Cleanup(c.Close)
	return c
}

func (c *Camera) serve(ln net.Listener, h func(net.Conn)) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			c.mu.Lock()
			c.conns = append(c.conns, conn)
			c.mu.Unlock()
			c.wg.Add(1)
			go func() { defer c.wg.Done(); h(conn) }()
		}
	}()
}

// Close stops every listener and connection and waits for the goroutines.
func (c *Camera) Close() {
	select {
	case <-c.done:
		return
	default:
		close(c.done)
	}
	c.sshLn.Close()
	c.ssh2Ln.Close()
	c.ctlLn.Close()
	c.RTSP.Close()
	c.mu.Lock()
	for _, cn := range c.conns {
		cn.Close()
	}
	c.mu.Unlock()
	c.wg.Wait()
}

// ---------------------------------------------------------------- SSH

// handleSSH2 is the second Dropbear: it only exists once the camera script "prepare" has started it.
func (c *Camera) handleSSH2(nc net.Conn) {
	if !c.secondUp() {
		nc.Close()
		return
	}
	c.handleSSH(nc)
}

func (c *Camera) handleSSH(nc net.Conn) {
	cfg := &gossh.ServerConfig{
		PublicKeyCallback: func(_ gossh.ConnMetadata, key gossh.PublicKey) (*gossh.Permissions, error) {
			if bytes.Equal(key.Marshal(), c.opts.AuthorizedKey.Marshal()) {
				return nil, nil
			}
			return nil, errors.New("key refused")
		},
		ServerVersion: "SSH-2.0-dropbear_fake",
	}
	c.hostKeyMu.Lock()
	cfg.AddHostKey(c.hostSigner)
	c.hostKeyMu.Unlock()
	conn, chans, reqs, err := gossh.NewServerConn(nc, cfg)
	if err != nil {
		nc.Close()
		return
	}
	defer conn.Close()
	go gossh.DiscardRequests(reqs)
	for nch := range chans {
		switch nch.ChannelType() {
		case "session":
			ch, creqs, err := nch.Accept()
			if err != nil {
				continue
			}
			c.wg.Add(1)
			go func() { defer c.wg.Done(); c.session(ch, creqs, nc) }()
		case "direct-tcpip":
			var p struct {
				Host     string
				Port     uint32
				OrigHost string
				OrigPort uint32
			}
			if gossh.Unmarshal(nch.ExtraData(), &p) != nil || p.Host != "127.0.0.1" || p.Port != 8091 {
				_ = nch.Reject(gossh.Prohibited, "only 127.0.0.1:8091 is forwarded")
				continue
			}
			ch, creqs, err := nch.Accept()
			if err != nil {
				continue
			}
			go gossh.DiscardRequests(creqs)
			up, err := net.Dial("tcp", c.ctlLn.Addr().String())
			if err != nil {
				ch.Close()
				continue
			}
			c.wg.Add(1)
			go func() {
				defer c.wg.Done()
				defer up.Close()
				defer ch.Close()
				go io.Copy(up, ch)
				io.Copy(ch, up)
			}()
		default:
			_ = nch.Reject(gossh.UnknownChannelType, "no")
		}
	}
}

func (c *Camera) session(ch gossh.Channel, reqs <-chan *gossh.Request, nc net.Conn) {
	defer ch.Close()
	for r := range reqs {
		if r.Type != "exec" {
			if r.WantReply {
				_ = r.Reply(false, nil)
			}
			continue
		}
		var p struct{ Cmd string }
		if gossh.Unmarshal(r.Payload, &p) != nil {
			_ = r.Reply(false, nil)
			return
		}
		_ = r.Reply(true, nil)
		c.mu.Lock()
		c.commands = append(c.commands, p.Cmd)
		c.mu.Unlock()
		status := c.exec(p.Cmd, ch, ch, ch.Stderr(), nc)
		if status == dropConn {
			nc.Close()
			return
		}
		_, _ = ch.SendRequest("exit-status", false, binary.BigEndian.AppendUint32(nil, uint32(status)))
		return
	}
}

const registerCmd = `echo "REG_HOSTNAME=$(hostname)"; echo "REG_MTD7_MD5=$(dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum | cut -d" " -f1)"`

// Mtd7MD5 is what the fake camera reports for its APP partition.
const Mtd7MD5 = "0123456789abcdef0123456789abcdef"

// exec runs one of the known commands. Anything else is "not found".
func (c *Camera) exec(cmd string, stdin io.Reader, stdout, stderr io.Writer, nc net.Conn) int {
	if st, ok := c.flashExec(cmd, stdin, stdout, stderr, nc); ok {
		return st
	}
	switch {
	case cmd == "true":
		return 0
	case cmd == "test -x /usr/sbin/akam_hostname.sh":
		if c.opts.NoHostnameHelper {
			return 1
		}
		return 0
	case cmd == `p=$(pidof ak_rtsp); [ -n "$p" ] && kill $p`:
		c.mu.Lock()
		c.restarts++
		for _, k := range []string{"video.bitrate_cfg", "video.fps_cfg", "video.gop_cfg"} {
			c.vRun[k] = c.ctl[k]
		}
		c.ctl["video.cfg_pending"] = "0"
		c.mu.Unlock()
		return 0
	case cmd == registerCmd:
		fmt.Fprintf(stdout, "REG_HOSTNAME=akam-fake\nREG_MTD7_MD5=%s\n", Mtd7MD5)
		return 0
	}
	f := strings.Fields(cmd)
	if len(f) > 0 && strings.HasPrefix(f[0], "/usr/sbin/akam_") && strings.HasSuffix(f[0], ".sh") {
		name := filepath.Base(f[0])
		switch name {
		case "akam_onvif.sh":
			return c.onvif(f[1:], stdout, stderr)
		case "akam_ntp.sh":
			return c.ntp(f[1:], stdout, stderr)
		case "akam_rtsp_auth.sh", "akam_hostname.sh", "akam_debug.sh":
			if name == "akam_hostname.sh" && c.opts.NoHostnameHelper {
				fmt.Fprintf(stderr, "sh: %s: not found\n", f[0])
				return 127
			}
			return c.runScript(name, f[1:], stdin, stdout, stderr)
		}
	}
	fmt.Fprintf(stderr, "sh: %s: not found\n", cmd)
	return 127
}

// runScript runs a real helper script of the checkout in the fake root (argv, never a shell line).
func (c *Camera) runScript(name string, args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	script := filepath.Join(TemplatesDir(), name)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "sh", append([]string{script}, args...)...)
	cmd.Env = []string{
		"PATH=" + os.Getenv("PATH"),
		"AKAM_ROOT=" + c.Root,
		"AKAM_RTSP_BIN=" + filepath.Join(c.Root, "bin", "ak_rtsp"),
		"AKAM_HOSTNAME_CMD=" + filepath.Join(c.Root, "bin", "true_cmd"), // never change the host's name
		"HOME=" + c.Root,
	}
	cmd.Stdin, cmd.Stdout, cmd.Stderr = stdin, stdout, stderr
	cmd.WaitDelay = 2 * time.Second
	if err := cmd.Run(); err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			return ee.ExitCode()
		}
		fmt.Fprintln(stderr, err)
		return 126
	}
	return 0
}

// VideoRestarts is how many times the video service was restarted (the audited kill of ak_rtsp).
func (c *Camera) VideoRestarts() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.restarts
}

// ntp emulates akam_ntp.sh (texts of tools/firmware_patch_templates/akam_ntp.sh); the host grammar is
// checked again here, as the script does, so that a bad name from a client is refused like on the camera.
func (c *Camera) ntp(args []string, stdout, stderr io.Writer) int {
	if c.opts.NoNtpHelper {
		fmt.Fprintln(stderr, "sh: /usr/sbin/akam_ntp.sh: not found")
		return 127
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: akam_ntp.sh set SERVER [SERVER [SERVER]] | off | status | run    (SERVER: a host name or an IPv4 address)")
		return 2
	}
	switch args[0] {
	case "set":
		if len(args) < 2 || len(args) > 4 {
			fmt.Fprintln(stderr, "akam_ntp: give 1 to 3 servers")
			return 2
		}
		for _, s := range args[1:] {
			if !ntpHostRe.MatchString(s) {
				fmt.Fprintf(stderr, "akam_ntp: '%s' is not a host name or IPv4 address; nothing changed.\n", s)
				return 2
			}
		}
		c.ntpOn, c.ntpList = true, strings.Join(args[1:], " ")
		fmt.Fprintf(stdout, "Time sync on, servers: %s. The camera asks them for the time at start and every 6 hours (UDP 123 outgoing).\nStarted now; see: %s status\n", c.ntpList, "/usr/sbin/akam_ntp.sh")
	case "off":
		if !c.ntpOn {
			fmt.Fprintln(stdout, "Time sync was already off.")
			return 0
		}
		c.ntpOn, c.ntpList = false, ""
		fmt.Fprintln(stdout, "Time sync off: no more traffic to time servers. The clock keeps its current value until the next reboot.")
	case "status":
		if c.ntpOn {
			fmt.Fprintf(stdout, "time sync: on, servers: %s\n", c.ntpList)
		} else {
			fmt.Fprintln(stdout, "time sync: off (default)")
		}
		fmt.Fprintln(stdout, "camera clock: 2026-01-01 00:00:00 UTC\nsys.time_set: 0 (the clock is before 2024: no RTC, nothing set it yet)\nhelper: not running")
	default:
		return 2
	}
	return 0
}

var ntpHostRe = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$`)

// onvif emulates akam_onvif.sh (it is not part of this checkout's templates yet); the texts are
// those of the script on the ONVIF branch.
func (c *Camera) onvif(args []string, stdout, stderr io.Writer) int {
	if c.opts.NoOnvifHelper {
		fmt.Fprintln(stderr, "sh: /usr/sbin/akam_onvif.sh: not found")
		return 127
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: akam_onvif.sh enable [PORT] | disable | status    (PORT: 1 to 65535, default 80)")
		return 2
	}
	switch args[0] {
	case "enable":
		if _, err := os.Stat(filepath.Join(c.Root, "etc", "config", "akam", "credentials")); err != nil {
			fmt.Fprintln(stderr, "akam_onvif: ONVIF needs a password (no password set); nothing changed.")
			return 1
		}
		if len(args) > 1 {
			c.onvifPrt = args[1]
		}
		c.onvifOn = true
		fmt.Fprintf(stdout, "ONVIF enabled on port %s (TCP %s, and UDP 3702 for discovery).\nApplied now (ak_rtsp reloaded the setting).\n", c.onvifPrt, c.onvifPrt)
	case "disable":
		if !c.onvifOn {
			fmt.Fprintln(stdout, "ONVIF was already disabled.")
			return 0
		}
		c.onvifOn = false
		fmt.Fprintln(stdout, "ONVIF disabled.\nApplied now (ak_rtsp reloaded the setting).")
	case "status":
		if c.onvifOn {
			fmt.Fprintf(stdout, "ONVIF: enabled, port %s\n", c.onvifPrt)
		} else {
			fmt.Fprintln(stdout, "ONVIF: disabled (default)")
		}
		fmt.Fprintln(stdout, "password: set\nstate: unknown (this ak_rtsp does not report it)")
	default:
		return 2
	}
	return 0
}

// ---------------------------------------------------------------- control port 8091

func (c *Camera) handleCtl(nc net.Conn) {
	defer nc.Close()
	if c.opts.NoCtl {
		time.Sleep(50 * time.Millisecond)
		return
	}
	// the camera pushes a LOG line at any time: the client must drop it
	fmt.Fprintf(nc, "LOG [fake] hello\n")
	sc := bufio.NewScanner(nc)
	for sc.Scan() {
		line := strings.TrimRight(sc.Text(), "\r")
		f := strings.SplitN(line, " ", 3)
		var reply string
		c.mu.Lock()
		switch {
		case f[0] == "GET" && len(f) >= 2:
			if v, ok := c.ctl[f[1]]; ok {
				reply = f[1] + "=" + v
			} else {
				reply = "ERR unknown param " + f[1]
			}
		case f[0] == "SET" && len(f) == 3:
			c.ctlSets = append(c.ctlSets, line)
			if _, ok := c.ctl[f[1]]; !ok {
				reply = "ERR unknown param " + f[1]
			} else if strings.HasPrefix(f[1], "video.") {
				n, err := strconv.Atoi(f[2])
				lo, hi := 0, 0
				switch f[1] {
				case "video.bitrate_cfg":
					lo, hi = 500, 3000
				case "video.fps_cfg":
					lo, hi = 10, 15
				case "video.gop_cfg":
					lo, hi = 1, 4
				}
				if hi == 0 {
					reply = "ERR " + f[1] + " is read-only"
				} else if err != nil || n < lo || n > hi || (f[1] == "video.fps_cfg" && n != 10 && n != 15) {
					reply = "ERR invalid " + f[1] + " value"
				} else {
					c.ctl[f[1]] = f[2]
					pend := false
					for _, k := range []string{"video.bitrate_cfg", "video.fps_cfg", "video.gop_cfg"} {
						if c.ctl[k] != c.vRun[k] {
							pend = true
						}
					}
					c.ctl["video.cfg_pending"] = "0"
					reply = "OK " + f[1] + "=" + f[2]
					if pend {
						c.ctl["video.cfg_pending"] = "1"
						reply += " (restart to apply)"
					}
				}
			} else if strings.HasPrefix(f[1], "isp.") {
				n, err := strconv.Atoi(f[2])
				if err != nil || n < -50 || n > 50 {
					reply = "ERR out of range"
				} else {
					c.ctl[f[1]] = f[2]
					reply = "OK " + f[1] + "=" + f[2]
				}
			} else {
				c.ctl[f[1]] = f[2]
				reply = "OK " + f[1] + "=" + f[2]
				if f[1] == "audio.mic_enable" {
					reply += " (restart to apply)"
				}
			}
		default:
			reply = "ERR syntax"
		}
		c.mu.Unlock()
		fmt.Fprintf(nc, "%s\n", reply)
	}
}

// CtlValue reads a control value (tests).
func (c *Camera) CtlValue(key string) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ctl[key]
}

// SetCtl overrides a control value, or removes it when value is "-" (an older image).
func (c *Camera) SetCtl(key, value string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if value == "-" {
		delete(c.ctl, key)
		return
	}
	c.ctl[key] = value
}
