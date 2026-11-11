package sdcard

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
	"time"
)

// The Windows elevation (spec 11, 4.4.5): the unelevated program creates a named pipe that only the current user
// can open, starts itself again elevated through the UAC prompt (ShellExecuteExW, verb runas), and talks to that
// helper over the pipe with the framing of frame.go:
//
//	parent (user)                                         helper (elevated, same user)
//	 pipe \\.\pipe\akam-sd-<random>: first instance,
//	 DACL = this user only, no remote clients
//	 runas "<self> sd-helper --pipe NAME --nonce N"  --->  connects (identification level, no impersonation)
//	 the client's process id must be the launched one <--- nonce line
//	 constant-time nonce check, then the request    --->  exactly one request, one operation
//	 progress lines and the final answer             <---
//
// The command line carries the pipe name and a random nonce and nothing else: the Wi-Fi password and the files of
// the card travel on the pipe only, never on disk and never on a command line. The decisions live in this file and
// are tested on Linux with injected fakes; the Windows files (pipe_windows.go, transport_windows.go) only call the
// system.

const (
	uacConnectTTL   = 30 * time.Second // the helper must connect this long after the prompt was answered
	uacPromptTTL    = 5 * time.Minute  // the user may take a while to find the prompt
	uacHandshakeTTL = 15 * time.Second
	uacRequestTTL   = 2 * time.Minute // sending (or reading) the request: at most 96 MiB on a local pipe
	uacExitGrace    = 5 * time.Second // after the helper's exit, how long its last words may still be read
	uacErrCancelled = 1223            // ERROR_CANCELLED: the user declined the prompt
)

var sidRe = regexp.MustCompile(`^S-1-[0-9]+(-[0-9]+)+$`)

var pipeNameRe = regexp.MustCompile(`^\\\\\.\\pipe\\akam-sd-[0-9a-f]{32}$`)

// ErrUACDeclined is what the user gets when the Windows administrator prompt is declined.
var ErrUACDeclined = errors.New("you declined the Windows administrator prompt: no administrator rights, nothing was written")

// uacConn is one end of the pipe. SetDeadline makes every blocked call end by itself.
type uacConn interface {
	io.ReadWriteCloser
	SetDeadline(t time.Time) error
}

// uacListener is the single-instance pipe the parent created.
type uacListener interface {
	// Accept waits for the one client, at most timeout.
	Accept(timeout time.Duration) (uacConn, error)
	// ClientPID is the process id of the client of a connection Accept returned (GetNamedPipeClientProcessId).
	ClientPID(c uacConn) (uint32, error)
	Close() error
}

// uacProc is the elevated process that was started.
type uacProc interface {
	PID() uint32
	// Exited gets the exit status once, when the process ends.
	Exited() <-chan error
	Close()
}

// uacLaunchError is a failed start of the elevated process; Code is the Win32 error.
type uacLaunchError struct {
	Code uint32
	Err  error
}

func (e *uacLaunchError) Error() string { return e.Err.Error() }
func (e *uacLaunchError) Unwrap() error { return e.Err }

// uacTransport is the parent side. Everything that touches the OS is a field, so a test can play the helper.
type uacTransport struct {
	elevated  func() bool
	listen    func(name string) (uacListener, error)
	launch    func(self, params string) (uacProc, error)
	inProcess func(req Request, stderr io.Writer) (Response, error)
	connect   time.Duration
	prompt    time.Duration
}

func newUACTransport(elevated func() bool, listen func(string) (uacListener, error), launch func(string, string) (uacProc, error), inProcess func(Request, io.Writer) (Response, error)) *uacTransport {
	return &uacTransport{elevated: elevated, listen: listen, launch: launch, inProcess: inProcess, connect: uacConnectTTL, prompt: uacPromptTTL}
}

// uacParams is the whole parameter string of the elevated start: the helper verb, the pipe name (a fixed prefix
// and hex) and the nonce (hex). Nothing in it needs quoting, and no request data or secret can get into it.
func uacParams(pipe, nonce string) (string, error) {
	if !pipeNameRe.MatchString(pipe) || !nonceRe.MatchString(nonce) {
		return "", errors.New("internal error: unsafe pipe name or nonce")
	}
	return "sd-helper --pipe " + pipe + " --nonce " + nonce, nil
}

// pipeSDDL is the security descriptor of the pipe: protected DACL (nothing inherited) with one entry, read and
// write for the current user's SID. SYSTEM is deliberately not listed: the helper runs as the user (an elevated
// token of the same account), SYSTEM never talks to this pipe, and a smaller list is a smaller surface. The
// over-the-shoulder case (another administrator account answers the prompt) is thus refused, with a clear
// message, rather than served.
func pipeSDDL(sid string) (string, error) {
	if !sidRe.MatchString(sid) {
		return "", errors.New("unexpected user SID")
	}
	return "D:P(A;;FRFW;;;" + sid + ")", nil
}

// run is RunElevated for the UAC transport.
func (t *uacTransport) run(self string, req Request, stderr io.Writer) (Response, error) {
	if t.elevated() {
		// "Run as administrator": no prompt, no pipe
		return t.inProcess(req, stderr)
	}
	var err error
	if self == "" {
		if self, err = os.Executable(); err != nil {
			return Response{}, err
		}
	}
	body, err := json.Marshal(req)
	if err != nil {
		return Response{}, err
	}
	if len(body) > MaxRequest { // the helper would refuse it: say so before a prompt is shown
		return Response{}, errors.New("the request is too large for the administrator helper")
	}
	rnd, err := randHex(16)
	if err != nil {
		return Response{}, err
	}
	nonce, err := randHex(16)
	if err != nil {
		return Response{}, err
	}
	name := `\\.\pipe\akam-sd-` + rnd
	params, err := uacParams(name, nonce)
	if err != nil {
		return Response{}, err
	}
	ln, err := t.listen(name)
	if err != nil {
		return Response{}, fmt.Errorf("cannot create the private channel to the administrator step: %v", scrub(err.Error(), nonce))
	}
	defer ln.Close()

	proc, err := t.start(self, params)
	if err != nil {
		return Response{}, err
	}
	defer proc.Close()

	type accepted struct {
		c   uacConn
		err error
	}
	got := make(chan accepted, 1)
	go func() {
		c, err := ln.Accept(t.connect)
		got <- accepted{c, err}
	}()
	exited := proc.Exited()
	var conn uacConn
	select {
	case a := <-got:
		if a.err != nil {
			return Response{}, errors.New("the administrator helper did not connect")
		}
		conn = a.c
	case err := <-exited:
		_ = ln.Close()
		if a := <-got; a.c != nil {
			a.c.Close()
		}
		msg := "the administrator step ended without connecting"
		if err != nil {
			msg += ": " + scrub(err.Error(), nonce)
		}
		return Response{}, errors.New(msg)
	}
	defer conn.Close()

	if err := authenticateHelper(ln, conn, proc, nonce); err != nil {
		return Response{}, err
	}
	// the helper's own exit ends a conversation that would otherwise wait for it for ever
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		select {
		case <-exited:
			select {
			case <-time.After(uacExitGrace):
				_ = conn.Close()
			case <-stop:
			}
		case <-stop:
		}
	}()
	_ = conn.SetDeadline(time.Now().Add(uacRequestTTL))
	if err := writeRequest(conn, body); err != nil {
		return Response{}, errors.New("sending the request to the administrator helper failed")
	}
	_ = conn.SetDeadline(time.Time{})
	return readAnswer(conn, stderr)
}

// start launches the elevated helper, giving the user uacPromptTTL to answer the prompt.
func (t *uacTransport) start(self, params string) (uacProc, error) {
	type started struct {
		p   uacProc
		err error
	}
	ch := make(chan started, 1)
	go func() {
		p, err := t.launch(self, params)
		ch <- started{p, err}
	}()
	select {
	case s := <-ch:
		if s.err != nil {
			var le *uacLaunchError
			if errors.As(s.err, &le) && le.Code == uacErrCancelled {
				return nil, ErrUACDeclined
			}
			return nil, fmt.Errorf("the administrator prompt did not start: %v (you can start Akam as Administrator instead: right click, Run as administrator)", s.err)
		}
		return s.p, nil
	case <-time.After(t.prompt):
		go func() { // a late start is cleaned up: its helper finds the pipe closed and exits
			if s := <-ch; s.p != nil {
				s.p.Close()
			}
		}()
		return nil, errors.New("the Windows administrator prompt was not answered in time")
	}
}

// authenticateHelper checks the client of the pipe before a byte of the request is sent: it is the process that was
// launched, and it knows the nonce. The pipe has one instance, so there is nobody to retry with: any failure ends.
func authenticateHelper(ln uacListener, c uacConn, proc uacProc, nonce string) error {
	pid, err := ln.ClientPID(c)
	if err != nil {
		return fmt.Errorf("cannot read the process id of the pipe client: %v", err)
	}
	if pid != proc.PID() {
		return fmt.Errorf("refusing: the pipe client is process %d, not the administrator helper that was started", pid)
	}
	_ = c.SetDeadline(time.Now().Add(uacHandshakeTTL))
	if err := readNonce(c, nonce); err != nil {
		return fmt.Errorf("refusing: the pipe client failed the handshake (%v)", err)
	}
	_ = c.SetDeadline(time.Time{})
	return nil
}

// ---- helper side ----

// pipeHelper is `sd-helper --pipe NAME --nonce H` (elevated, started through the UAC prompt). dial connects to the
// pipe, retrying for about uacConnectTTL. It returns the exit status.
func pipeHelper(args []string, mk func() Helper, dial func(name string, wait time.Duration) (uacConn, error)) int {
	if len(args) != 4 || args[0] != "--pipe" || args[2] != "--nonce" {
		return 2
	}
	name, nonce := args[1], args[3]
	if !pipeNameRe.MatchString(name) || !nonceRe.MatchString(nonce) {
		return 2
	}
	c, err := dial(name, uacConnectTTL)
	if err != nil {
		return 2
	}
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(uacHandshakeTTL))
	if _, err := io.WriteString(c, nonce+"\n"); err != nil {
		return 2
	}
	_ = c.SetDeadline(time.Now().Add(uacRequestTTL)) // the parent sends the request right after the handshake
	h := mk()
	h.Progress = progressWriter{c}
	return serveFramed(c, h, func() { _ = c.SetDeadline(time.Time{}) }) // the operation itself may take long
}
