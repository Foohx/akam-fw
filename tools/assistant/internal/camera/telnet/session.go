// Package telnet is the PC side of the stock camera's maintenance shell (BusyBox telnetd on port 24,
// opened by the toolkit card). It is the Go version of tools/wizard/lib/camsh.sh and transfer.sh.
//
// How a command is run (identical to camsh.sh):
//   - one command per send, never longer than 350 bytes, no newline in it;
//   - the line sent is: echo "__AKAM_BEG_<nonce>_<n>_"x; CMD; echo "__AKAM_DONE_<nonce>_<n>:"$?"__".
//     Both markers are built by the camera's shell, so the echo of the typed line (which holds the quotes)
//     never matches them. Everything received before the BEG marker is dropped (the BusyBox line editor
//     echoes the typed line, wraps it every 80 columns and sends ESC[6n);
//   - the answer is read with an inactivity timeout (reset on every byte); CR and terminal escapes are
//     dropped;
//   - after 2 consecutive failures, or with --manual-telnet, the session switches to manual mode: the
//     exact command is shown, the user runs it in their own window and pastes the output back. The program
//     never invents the output of a command.
//
// The PC only ever dials out. Nothing in this package types the camera's "YES": SendLine is a plain
// transport and the callers pass what a human confirmed.
package telnet

import (
	"errors"
	"fmt"
	"net"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Limits and defaults.
const (
	MaxCmd     = 350
	MaxOut     = 1 << 20
	DefTimeout = 30 * time.Second
)

// Errors of the session.
var (
	ErrTimeout    = errors.New("no answer from the camera (timeout)")
	ErrClosed     = errors.New("the camera session is closed")
	ErrBadCommand = errors.New("a command must be one line of at most 350 bytes")
	ErrManual     = errors.New("manual mode")
)

// ManualHandler is the human in the loop of manual mode (the CLI implements it with the UI).
type ManualHandler interface {
	// Show prints the exact command to run in the user's own telnet window.
	Show(host string, port int, cmd string)
	// Collect reads the pasted output and the success answer. An error means no input is possible.
	Collect() (out string, ok bool, err error)
	// Wait asks the user to press Enter when text is done (error: the user stopped).
	Wait(text string) error
}

// Config of a session.
type Config struct {
	Host    string
	Port    int
	Timeout time.Duration // inactivity timeout of Finish and Exec (default 30 s)
	// DialTimeout is the connection timeout (default 5 s).
	DialTimeout time.Duration
	// Manual is asked in manual mode; nil makes manual mode fail with an error.
	Manual ManualHandler
	// ForceManual starts in manual mode (--manual-telnet).
	ForceManual bool
	// NoNegotiate drops IAC negotiation bytes silently (no reply), as the bash version does.
	NoNegotiate bool
	// Warn shows a warning to the user; Log writes a line to the log.
	Warn func(string)
	Log  func(format string, a ...any)
	// Dial replaces net.DialTimeout (tests).
	Dial func(network, addr string, timeout time.Duration) (net.Conn, error)
}

// Session is one connection (or manual mode) to the camera shell.
type Session struct {
	cfg    Config
	conn   net.Conn
	open   bool
	manual bool
	fails  int
	seq    int
	nonce  string

	rx   []byte // received, not yet processed
	part string // unfinished line
	iac  int
	sent map[[2]byte]bool

	// pending command
	re       *regexp.Regexp
	beg      string
	begSeen  bool
	echoSkip string

	// Out and RC are the output and exit code of the last command (manual mode: 0 or 1).
	Out string
	RC  int

	streamFn func(string)
	skip     *regexp.Regexp
	progFn   func(label string, pct int)
}

// New makes a session; nothing is opened yet.
func New(cfg Config) *Session {
	if cfg.Timeout <= 0 {
		cfg.Timeout = DefTimeout
	}
	if cfg.DialTimeout <= 0 {
		cfg.DialTimeout = 5 * time.Second
	}
	if cfg.Dial == nil {
		cfg.Dial = net.DialTimeout
	}
	s := &Session{cfg: cfg, manual: cfg.ForceManual, sent: map[[2]byte]bool{}}
	return s
}

func (s *Session) logf(format string, a ...any) {
	if s.cfg.Log != nil {
		s.cfg.Log(format, a...)
	}
}

func (s *Session) warn(m string) {
	if s.cfg.Warn != nil {
		s.cfg.Warn(m)
	}
}

// Host and Port are the camera's address.
func (s *Session) Host() string { return s.cfg.Host }
func (s *Session) Port() int    { return s.cfg.Port }

// IsManual reports manual mode.
func (s *Session) IsManual() bool { return s.manual }

// IsOpen reports a live connection.
func (s *Session) IsOpen() bool { return s.open }

// Open connects (manual mode: nothing to open).
func (s *Session) Open() error {
	s.Close()
	s.part, s.rx, s.iac = "", nil, 0
	s.nonce = strconv.Itoa(int(time.Now().UnixNano()%900000) + 100000)
	if s.manual {
		return nil
	}
	addr := net.JoinHostPort(s.cfg.Host, strconv.Itoa(s.cfg.Port))
	s.logf("CAM open %s", addr)
	c, err := s.cfg.Dial("tcp", addr, s.cfg.DialTimeout)
	if err != nil {
		s.logf("CAM open failed")
		return err
	}
	s.conn = c
	s.open = true
	return nil
}

// Close ends the connection.
func (s *Session) Close() {
	if s.conn != nil {
		s.conn.Close()
		s.conn = nil
		s.logf("CAM close")
	}
	s.open = false
}

// ManualOn switches to manual mode for the rest of the run.
func (s *Session) ManualOn() {
	s.Close()
	s.manual = true
}

// StreamSet shows the lines of the next commands as they arrive (fn nil: stop), minus those matching
// skip. prog receives the progress bars of flash_tool.
func (s *Session) StreamSet(fn func(string), skip *regexp.Regexp, prog func(label string, pct int)) {
	s.streamFn, s.skip, s.progFn = fn, skip, prog
}

// Line is line n (1-based) of Out, CR removed.
func (s *Session) Line(n int) string {
	ls := strings.Split(s.Out, "\n")
	if n >= 1 && n <= len(ls) {
		return strings.TrimRight(ls[n-1], "\r")
	}
	return ""
}

// OutHas reports whether the output of the last command contains text.
func (s *Session) OutHas(text string) bool { return strings.Contains(s.Out, text) }

var (
	ansiCSI = regexp.MustCompile("\x1b\\[[0-9;?]*[ -/]*[@-~]")
	ansiSet = regexp.MustCompile("\x1b[()][A-Za-z0-9]")
	progRe  = regexp.MustCompile(`^\s*([A-Za-z][A-Za-z +/-]*): \[[=> ]*\]\s*([0-9]{1,3})%`)
)

// StripANSI removes terminal escapes (BusyBox's ls colours its names even over telnet).
func StripANSI(s string) string {
	if !strings.Contains(s, "\x1b") {
		return s
	}
	return ansiSet.ReplaceAllString(ansiCSI.ReplaceAllString(s, ""), "")
}

func (s *Session) progress(line string) bool {
	m := progRe.FindStringSubmatch(line)
	if m == nil {
		return false
	}
	if s.progFn != nil {
		n, _ := strconv.Atoi(m[2])
		s.progFn(m[1], n)
	}
	return true
}

func (s *Session) show(line string) {
	if s.streamFn == nil || (s.skip != nil && s.skip.MatchString(line)) {
		return
	}
	s.streamFn(line)
}

// handleLine processes one completed line; true when it is the end marker.
func (s *Session) handleLine(line string) bool {
	line = StripANSI(line)
	if s.re != nil {
		if m := s.re.FindStringSubmatch(line); m != nil {
			s.RC, _ = strconv.Atoi(m[1])
			s.re = nil
			pre := line
			if i := strings.Index(line, "__AKAM_DONE_"); i >= 0 {
				pre = line[:i]
			} else {
				pre = ""
			}
			if s.begSeen && pre != "" && len(s.Out) < MaxOut {
				s.Out += pre + "\n"
				s.show(pre)
			}
			return true
		}
	}
	if s.beg != "" && !s.begSeen {
		if strings.Contains(line, s.beg) {
			s.begSeen = true
		}
		return false
	}
	if s.progress(line) {
		return false
	}
	if s.echoSkip != "" {
		e := s.echoSkip
		s.echoSkip = ""
		if line == e {
			return false
		}
	}
	if len(s.Out) < MaxOut {
		s.Out += line + "\n"
	}
	s.show(line)
	return false
}

type readStatus int

const (
	rDone readStatus = iota
	rPrompt
	rTimeout
	rClosed
)

const (
	cIAC  = 255
	cDONT = 254
	cDO   = 253
	cWONT = 252
	cWILL = 251
	cSB   = 250
	cSE   = 240
)

// reply answers a negotiation request (RFC 854): everything is refused except what a BusyBox telnetd
// needs: it may echo and suppress go-ahead. Each answer is sent once.
func (s *Session) reply(verb, opt byte) {
	if s.cfg.NoNegotiate || s.conn == nil {
		return
	}
	var out byte
	switch verb {
	case cDO:
		out = cWONT
	case cWILL:
		if opt == 1 || opt == 3 { // ECHO, SUPPRESS-GO-AHEAD
			out = cDO
		} else {
			out = cDONT
		}
	case cDONT:
		out = cWONT
	default:
		return
	}
	k := [2]byte{out, opt}
	if s.sent[k] {
		return
	}
	s.sent[k] = true
	_, _ = s.conn.Write([]byte{cIAC, out, opt})
}

// read processes the incoming stream until the end marker of the pending command (rDone), until the
// unfinished last line ends with prompt (rPrompt), the inactivity timeout (rTimeout) or the end of the
// session (rClosed).
func (s *Session) read(tmo time.Duration, prompt string) readStatus {
	line := s.part
	s.part = ""
	deadline := time.Now().Add(tmo)
	var verb byte
	buf := make([]byte, 4096)
	for {
		if len(s.rx) == 0 {
			slice := time.Until(deadline)
			if prompt != "" && slice > time.Second {
				slice = time.Second
			}
			if slice <= 0 {
				s.part = line
				return rTimeout
			}
			_ = s.conn.SetReadDeadline(time.Now().Add(slice))
			n, err := s.conn.Read(buf)
			if n > 0 {
				s.rx = append(s.rx, buf[:n]...)
				deadline = time.Now().Add(tmo)
			}
			if err != nil && n == 0 {
				var ne net.Error
				if errors.As(err, &ne) && ne.Timeout() {
					if prompt != "" && s.begSeen && line != "" && (strings.HasSuffix(line, prompt) || strings.HasSuffix(line, strings.TrimRight(prompt, " "))) {
						s.show(line)
						s.part = ""
						return rPrompt
					}
					continue
				}
				s.part = line
				s.open = false
				return rClosed
			}
			continue
		}
		c := s.rx[0]
		s.rx = s.rx[1:]
		switch s.iac {
		case 0:
			switch c {
			case cIAC:
				s.iac = 1
			case '\r':
				// flash_tool redraws its bar with \r: a segment that is a bar is consumed now
				if s.begSeen && strings.Contains(line, "%") && s.progress(line) {
					line = ""
				}
			case '\n':
				if s.handleLine(line) {
					return rDone
				}
				line = ""
			default:
				line += string([]byte{c})
			}
		case 1:
			switch c {
			case cWILL, cWONT, cDO, cDONT:
				verb = c
				s.iac = 2
			case cSB:
				s.iac = 3
			default:
				s.iac = 0
			}
		case 2:
			s.reply(verb, c)
			s.iac = 0
		case 3:
			if c == cIAC {
				s.iac = 4
			}
		case 4:
			if c == cSE {
				s.iac = 0
			} else {
				s.iac = 3
			}
		}
	}
}

// Start sends cmd wrapped in the markers and returns at once. Manual mode shows the command.
func (s *Session) Start(cmd string) error {
	if strings.ContainsAny(cmd, "\n\r") {
		return fmt.Errorf("%w: contains a line break", ErrBadCommand)
	}
	if len(cmd) > MaxCmd {
		return fmt.Errorf("%w: %d bytes", ErrBadCommand, len(cmd))
	}
	s.Out, s.RC = "", 0
	s.logf("CAM> %s", cmd)
	if s.manual {
		if s.cfg.Manual == nil {
			return ErrManual
		}
		s.cfg.Manual.Show(s.cfg.Host, s.cfg.Port, cmd)
		return nil
	}
	if !s.open {
		return ErrClosed
	}
	s.seq++
	s.re = regexp.MustCompile(fmt.Sprintf(`__AKAM_DONE_%s_%d:([0-9]+)__`, s.nonce, s.seq))
	s.beg = fmt.Sprintf("__AKAM_BEG_%s_%d_x", s.nonce, s.seq)
	s.begSeen = false
	line := fmt.Sprintf("echo \"__AKAM_BEG_%s_%d_\"x; %s; echo \"__AKAM_DONE_%s_%d:\"$?\"__\"\n", s.nonce, s.seq, cmd, s.nonce, s.seq)
	if _, err := s.conn.Write([]byte(line)); err != nil {
		s.open = false
		return ErrClosed
	}
	return nil
}

// Finish waits for the end of the command started by Start (the exit code is in RC).
func (s *Session) Finish(tmo time.Duration) error {
	if tmo <= 0 {
		tmo = s.cfg.Timeout
	}
	if s.manual {
		return s.collect()
	}
	if !s.open {
		return ErrClosed
	}
	switch s.read(tmo, "") {
	case rDone:
		s.logf("CAM rc=%d", s.RC)
		return nil
	case rTimeout:
		s.logf("CAM no answer (timeout)")
		return ErrTimeout
	}
	s.logf("CAM no answer (closed)")
	return ErrClosed
}

// PromptResult is the outcome of WaitPrompt.
type PromptResult int

const (
	PromptSeen PromptResult = iota
	CommandEnded
	PromptTimeout
	PromptClosed
	PromptManual
)

// WaitPrompt streams the output of the started command until its unfinished last line ends with prompt.
func (s *Session) WaitPrompt(prompt string, tmo time.Duration) PromptResult {
	if tmo <= 0 {
		tmo = s.cfg.Timeout
	}
	if s.manual {
		return PromptManual
	}
	if !s.open {
		return PromptClosed
	}
	switch s.read(tmo, prompt) {
	case rPrompt:
		return PromptSeen
	case rDone:
		return CommandEnded
	case rTimeout:
		return PromptTimeout
	}
	return PromptClosed
}

// SendLine is a plain transport: callers pass what a human typed or confirmed.
func (s *Session) SendLine(text string) error {
	if !s.open {
		return ErrClosed
	}
	s.echoSkip = text
	if _, err := s.conn.Write([]byte(text + "\n")); err != nil {
		s.open = false
		return ErrClosed
	}
	return nil
}

// Interrupt sends Ctrl-C (ends a foreground command such as a stuck nc).
func (s *Session) Interrupt() error {
	if !s.open {
		return ErrClosed
	}
	if _, err := s.conn.Write([]byte{3}); err != nil {
		s.open = false
		return ErrClosed
	}
	return nil
}

// Exec runs one command and waits. It reopens the session once; after 2 failures in a row it falls back
// to manual mode and stays there. A nil error means the command ran (look at RC for its exit code).
func (s *Session) Exec(cmd string, tmo time.Duration) error {
	for !s.manual {
		var err error
		if !s.open {
			err = s.Open()
		}
		if err == nil {
			err = s.Start(cmd)
			if errors.Is(err, ErrBadCommand) {
				return err // a bug of the caller, not a failure of the camera
			}
		}
		if err == nil {
			err = s.Finish(tmo)
		}
		if err == nil {
			s.fails = 0
			return nil
		}
		s.fails++
		s.Close()
		s.warn(fmt.Sprintf("the camera did not answer (failure %d of 2).", s.fails))
		if s.fails >= 2 {
			s.warn("switching to MANUAL mode: you will paste each command in your own telnet window.")
			s.manual = true
		}
	}
	if err := s.Start(cmd); err != nil {
		return err
	}
	return s.Finish(tmo)
}

// OK is Exec and the command must also exit with 0.
func (s *Session) OK(cmd string, tmo time.Duration) error {
	if err := s.Exec(cmd, tmo); err != nil {
		return err
	}
	if s.RC != 0 {
		return fmt.Errorf("the command exited with %d", s.RC)
	}
	return nil
}

func (s *Session) collect() error {
	if s.cfg.Manual == nil {
		return ErrManual
	}
	out, ok, err := s.cfg.Manual.Collect()
	if err != nil {
		return err
	}
	s.Out = out
	s.RC = 1
	if ok {
		s.RC = 0
	}
	s.logf("CAM manual rc=%d", s.RC)
	return nil
}

// ManualWait asks the user to press Enter when text is done (manual mode only).
func (s *Session) ManualWait(text string) error {
	if s.cfg.Manual == nil {
		return ErrManual
	}
	return s.cfg.Manual.Wait(text)
}
