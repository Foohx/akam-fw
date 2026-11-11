package sdcard

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

// The UAC transport and the framing it shares with the macOS socket, on Linux: the pipe is a net.Pipe, the UAC
// prompt a function. The pipe's ACL and the real prompt are in the hardware plan (spec 11, 7.3).

var paramsRe = regexp.MustCompile(`^sd-helper --pipe (\\\\\.\\pipe\\akam-sd-[0-9a-f]{32}) --nonce ([0-9a-f]{32})$`)

type fakeProc struct {
	pid    uint32
	exited chan error
	mu     sync.Mutex
	closed bool
}

func (p *fakeProc) PID() uint32          { return p.pid }
func (p *fakeProc) Exited() <-chan error { return p.exited }
func (p *fakeProc) Close()               { p.mu.Lock(); p.closed = true; p.mu.Unlock() }

type fakeListener struct {
	mu       sync.Mutex
	pipe     chan uacConn
	clientID uint32
	pidErr   error
	closed   bool
	done     chan struct{}
}

func (l *fakeListener) Accept(timeout time.Duration) (uacConn, error) {
	select {
	case c := <-l.pipe:
		return c, nil
	case <-l.done:
		return nil, errors.New("closed")
	case <-time.After(timeout):
		return nil, errors.New("timeout")
	}
}
func (l *fakeListener) ClientPID(uacConn) (uint32, error) { return l.clientID, l.pidErr }
func (l *fakeListener) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if !l.closed {
		l.closed = true
		close(l.done)
	}
	return nil
}

// uacHarness plays UAC and the elevated helper.
type uacHarness struct {
	t        *testing.T
	rec      *recorder
	ln       *fakeListener
	proc     *fakeProc
	name     string
	params   string
	helperIn func(args []string) []string
	exit     chan int
	elevated bool
	inProc   int
	skipDial bool
}

func newUACHarness(t *testing.T, facts Facts) *uacHarness {
	return &uacHarness{t: t, rec: &recorder{facts: facts}, exit: make(chan int, 1),
		ln: &fakeListener{pipe: make(chan uacConn, 1), done: make(chan struct{}), clientID: 4242}, proc: &fakeProc{pid: 4242, exited: make(chan error, 1)}}
}

func (h *uacHarness) transport() *uacTransport {
	t := newUACTransport(
		func() bool { return h.elevated },
		func(name string) (uacListener, error) { h.name = name; return h.ln, nil },
		func(self, params string) (uacProc, error) {
			h.params = params
			m := paramsRe.FindStringSubmatch(params)
			if m == nil {
				h.t.Fatalf("parameters not understood: %q", params)
			}
			args := []string{"--pipe", m[1], "--nonce", m[2]}
			if h.helperIn != nil {
				args = h.helperIn(args)
			}
			parent, child := net.Pipe()
			go func() {
				mk := func() Helper { return Helper{Inv: h.rec, Opener: h.rec} }
				code := pipeHelper(args, mk, func(string, time.Duration) (uacConn, error) {
					if h.skipDial {
						return nil, errors.New("no pipe")
					}
					h.ln.pipe <- parent
					return child, nil
				})
				h.exit <- code
				h.proc.exited <- nil
			}()
			return h.proc, nil
		},
		func(req Request, stderr io.Writer) (Response, error) {
			h.inProc++
			return Response{OK: true, Lines: []string{"inproc"}}, nil
		})
	t.connect = 2 * time.Second
	t.prompt = 2 * time.Second
	return t
}

func wipeRequest(facts Facts) Request {
	return Request{Op: OpWipe, Device: "/dev/sdz", Token: Ident(facts), Files: []File{{Path: "wifi.conf", Data: []byte("TOP-SECRET-PASSWORD")}}}
}

func TestUACTransportOneRequestOneAnswer(t *testing.T) {
	facts := linuxCardFacts()
	h := newUACHarness(t, facts)
	var progress bytes.Buffer
	resp, err := h.transport().run(`C:\Program Files\Akam\akam.exe`, wipeRequest(facts), &progress)
	if err != nil || !resp.OK {
		t.Fatalf("%v %+v", err, resp)
	}
	if !strings.Contains(progress.String(), "wipe: 0%") {
		t.Errorf("progress not forwarded: %q", progress.String())
	}
	if got := strings.Join(h.rec.log, ","); !strings.Contains(got, "open ") || !strings.Contains(got, "write") {
		t.Errorf("helper did not run: %s", got)
	}
	if code := <-h.exit; code != 0 {
		t.Errorf("helper exit %d", code)
	}
	// the parameters: fixed words, the pipe name, the nonce; no request data, nothing secret
	m := paramsRe.FindStringSubmatch(h.params)
	if m == nil || m[1] != h.name {
		t.Fatalf("parameters %q (pipe %q)", h.params, h.name)
	}
	for _, bad := range []string{"TOP-SECRET", "wifi", "/dev/sdz", "wipe", Ident(facts)} {
		if strings.Contains(h.params, bad) {
			t.Errorf("parameters carry %q: %s", bad, h.params)
		}
	}
	if !h.ln.closed || !h.proc.closed {
		t.Errorf("not cleaned up: listener closed %v, process closed %v", h.ln.closed, h.proc.closed)
	}
}

func TestUACTransportRefusesAWrongClientProcess(t *testing.T) {
	facts := linuxCardFacts()
	h := newUACHarness(t, facts)
	h.ln.clientID = 666 // not the process that was launched
	_, err := h.transport().run("self.exe", wipeRequest(facts), io.Discard)
	if err == nil || !strings.Contains(err.Error(), "666") {
		t.Fatalf("%v", err)
	}
	if len(h.rec.log) != 0 {
		t.Fatalf("the helper ran: %v", h.rec.log)
	}
	h2 := newUACHarness(t, facts)
	h2.ln.pidErr = errors.New("denied")
	if _, err := h2.transport().run("self.exe", wipeRequest(facts), io.Discard); err == nil {
		t.Fatal("accepted a client whose process id cannot be read")
	}
}

func TestUACTransportRefusesAWrongNonceAndNeverEchoesIt(t *testing.T) {
	facts := linuxCardFacts()
	h := newUACHarness(t, facts)
	h.helperIn = func(a []string) []string { a[3] = strings.Repeat("0", 32); return a }
	_, err := h.transport().run("self.exe", wipeRequest(facts), io.Discard)
	if err == nil {
		t.Fatal("accepted a wrong nonce")
	}
	m := paramsRe.FindStringSubmatch(h.params)
	if strings.Contains(err.Error(), m[2]) || strings.Contains(err.Error(), strings.Repeat("0", 32)) {
		t.Errorf("the error echoes a nonce: %v", err)
	}
	if len(h.rec.log) != 0 {
		t.Fatalf("the helper ran: %v", h.rec.log)
	}
}

func TestUACTransportDeclinedPrompt(t *testing.T) {
	h := newUACHarness(t, linuxCardFacts())
	tr := h.transport()
	tr.launch = func(string, string) (uacProc, error) {
		return nil, &uacLaunchError{Code: 1223, Err: errors.New("The operation was canceled by the user.")}
	}
	_, err := tr.run("self.exe", Request{Op: OpList}, io.Discard)
	if !errors.Is(err, ErrUACDeclined) || !strings.Contains(err.Error(), "you declined the Windows administrator prompt") || !strings.Contains(err.Error(), "nothing was written") {
		t.Fatalf("%v", err)
	}
	if !h.ln.closed {
		t.Error("pipe left open after a declined prompt")
	}
}

func TestUACTransportLaunchFailureOffersRunAsAdministrator(t *testing.T) {
	h := newUACHarness(t, linuxCardFacts())
	tr := h.transport()
	tr.launch = func(string, string) (uacProc, error) {
		return nil, &uacLaunchError{Code: 5, Err: errors.New("Access is denied.")}
	}
	_, err := tr.run("self.exe", Request{Op: OpList}, io.Discard)
	if err == nil || errors.Is(err, ErrUACDeclined) || !strings.Contains(err.Error(), "Run as administrator") {
		t.Fatalf("%v", err)
	}
}

func TestUACTransportAlreadyElevatedRunsInProcess(t *testing.T) {
	h := newUACHarness(t, linuxCardFacts())
	h.elevated = true
	tr := h.transport()
	tr.listen = func(string) (uacListener, error) { t.Fatal("a pipe was created"); return nil, nil }
	tr.launch = func(string, string) (uacProc, error) { t.Fatal("a prompt was started"); return nil, nil }
	resp, err := tr.run("self.exe", Request{Op: OpList}, io.Discard)
	if err != nil || !resp.OK || h.inProc != 1 {
		t.Fatalf("%v %+v %d", err, resp, h.inProc)
	}
}

func TestUACTransportTimeoutsAndCleanup(t *testing.T) {
	facts := linuxCardFacts()
	// the helper never connects
	h := newUACHarness(t, facts)
	h.skipDial = true
	tr := h.transport()
	tr.connect = 100 * time.Millisecond
	tr.launch = func(string, string) (uacProc, error) { return h.proc, nil } // started, silent
	start := time.Now()
	if _, err := tr.run("self.exe", Request{Op: OpList}, io.Discard); err == nil || time.Since(start) > 3*time.Second {
		t.Fatalf("%v after %v", err, time.Since(start))
	}
	if !h.ln.closed || !h.proc.closed {
		t.Error("not cleaned up after the connect timeout")
	}
	// the prompt is never answered
	h = newUACHarness(t, facts)
	tr = h.transport()
	tr.prompt = 100 * time.Millisecond
	release := make(chan struct{})
	tr.launch = func(string, string) (uacProc, error) { <-release; return h.proc, nil }
	if _, err := tr.run("self.exe", Request{Op: OpList}, io.Discard); err == nil || !strings.Contains(err.Error(), "not answered") {
		t.Fatalf("%v", err)
	}
	close(release)
	// the helper process ends without connecting
	h = newUACHarness(t, facts)
	tr = h.transport()
	tr.launch = func(string, string) (uacProc, error) {
		h.proc.exited <- errors.New("the helper exited with status 2")
		return h.proc, nil
	}
	if _, err := tr.run("self.exe", Request{Op: OpList}, io.Discard); err == nil || !strings.Contains(err.Error(), "ended without connecting") {
		t.Fatalf("%v", err)
	}
	if !h.ln.closed || !h.proc.closed {
		t.Error("not cleaned up after an early exit")
	}
}

func TestPipeHelperRefusesBadArguments(t *testing.T) {
	good := `\\.\pipe\akam-sd-` + strings.Repeat("a", 32)
	nonce := strings.Repeat("b", 32)
	dial := func(string, time.Duration) (uacConn, error) { t.Fatal("dialled"); return nil, nil }
	mk := func() Helper { return Helper{} }
	for name, args := range map[string][]string{
		"none":             nil,
		"swapped":          {"--nonce", nonce, "--pipe", good},
		"extra":            {"--pipe", good, "--nonce", nonce, "--x"},
		"other pipe":       {"--pipe", `\\.\pipe\other`, "--nonce", nonce},
		"remote pipe":      {"--pipe", `\\evil\pipe\akam-sd-` + strings.Repeat("a", 32), "--nonce", nonce},
		"short nonce":      {"--pipe", good, "--nonce", "abc"},
		"upper-case hex":   {"--pipe", good, "--nonce", strings.Repeat("B", 32)},
		"path in pipe":     {"--pipe", good + `\..\x`, "--nonce", nonce},
		"uid flag (macOS)": {"--pipe", good, "--uid", "1", "--nonce", nonce},
	} {
		if code := pipeHelper(args, mk, dial); code != 2 {
			t.Errorf("%s: exit %d", name, code)
		}
	}
	if code := pipeHelper([]string{"--pipe", good, "--nonce", nonce}, mk, func(string, time.Duration) (uacConn, error) { return nil, errors.New("gone") }); code != 2 {
		t.Errorf("no pipe: exit %d", code)
	}
}

func TestUACParamsAndSDDL(t *testing.T) {
	if _, err := uacParams(`\\.\pipe\akam-sd-`+strings.Repeat("a", 32), "xyz"); err == nil {
		t.Error("accepted a non-hex nonce")
	}
	if _, err := uacParams(`\\.\pipe\x y`, strings.Repeat("a", 32)); err == nil {
		t.Error("accepted an unsafe pipe name")
	}
	got, err := pipeSDDL("S-1-5-21-1-2-3-1001")
	if err != nil || got != "D:P(A;;FRFW;;;S-1-5-21-1-2-3-1001)" {
		t.Errorf("%q %v", got, err)
	}
	if strings.Contains(got, "SY") || strings.Contains(got, "WD") || strings.Contains(got, "BA") {
		t.Errorf("the descriptor names more than the user: %s", got)
	}
	for _, sid := range []string{"", "S-1-5", "S-1-5-21-1)(A;;GA;;;WD", "x"} {
		if _, err := pipeSDDL(sid); err == nil {
			t.Errorf("accepted SID %q", sid)
		}
	}
}

func TestHelperModeFor(t *testing.T) {
	cases := []struct {
		goos string
		args []string
		want helperMode
	}{
		{"linux", nil, helperStdin},
		{"windows", nil, helperStdin},
		{"darwin", nil, helperStdin},
		{"darwin", []string{"--socket", "x"}, helperSocket},
		{"windows", []string{"--pipe", "x"}, helperPipe},
		{"linux", []string{"--pipe", "x"}, helperBad},
		{"linux", []string{"--socket", "x"}, helperBad},
		{"windows", []string{"--socket", "x"}, helperBad},
		{"darwin", []string{"--pipe", "x"}, helperBad},
		{"linux", []string{"--wipe"}, helperBad},
		{"windows", []string{"--wipe"}, helperBad},
	}
	for _, c := range cases {
		if got := helperModeFor(c.goos, c.args); got != c.want {
			t.Errorf("%s %v: %d, want %d", c.goos, c.args, got, c.want)
		}
	}
}

// ---- framing (frame.go), shared with the macOS socket ----

// serve runs serveFramed on a stream whose request side is the given bytes (then EOF) and returns its answer.
func serve(t *testing.T, in string, h Helper) (code int, answer string) {
	t.Helper()
	var out bytes.Buffer
	code = serveFramed(struct {
		io.Reader
		io.Writer
	}{strings.NewReader(in), &out}, h, nil)
	return code, out.String()
}

func TestFrameRequests(t *testing.T) {
	facts := linuxCardFacts()
	rec := &recorder{facts: facts}
	h := Helper{Inv: rec, Opener: rec}
	doc, _ := json.Marshal(Request{Op: OpList})
	for name, tc := range map[string]struct {
		in   string
		code int
		want string
	}{
		"unknown field":      {`{"op":"list","evil":1}` + "\n", 2, "bad request"},
		"second document":    {string(doc) + string(doc) + "\n", 2, "bad request"},
		"truncated, no EOL":  {`{"op":"li`, 2, "bad request"},
		"not json":           {"hello\n", 2, "bad request"},
		"empty line":         {"\n", 2, "bad request"},
		"one request":        {string(doc) + "\n", 0, `"ok":true`},
		"later line ignored": {string(doc) + "\n" + string(doc) + "\n", 0, `"ok":true`},
	} {
		code, ans := serve(t, tc.in, h)
		if code != tc.code || !strings.Contains(ans, tc.want) {
			t.Errorf("%s: code %d answer %q", name, code, ans)
		}
		if strings.Count(ans, "\n") != 1 {
			t.Errorf("%s: more than one answer: %q", name, ans)
		}
	}
}

func TestFrameOversizeRequest(t *testing.T) {
	if testing.Short() {
		t.Skip("allocates the 96 MiB cap")
	}
	// a document that never ends and never has a newline: cut at the cap
	big := io.MultiReader(strings.NewReader(`{"op":"`), io.LimitReader(repeatReader('a'), MaxRequest+16))
	if _, err := readFrame(big); err == nil {
		t.Fatal("accepted a request over the cap")
	}
	// a line one byte over the cap is refused, at the cap it is accepted by the framing
	over := io.MultiReader(io.LimitReader(repeatReader('a'), MaxRequest+1), strings.NewReader("\n"))
	if _, err := readFrame(over); err == nil {
		t.Fatal("accepted a line over the cap")
	}
}

type repeatReader byte

func (r repeatReader) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = byte(r)
	}
	return len(p), nil
}

func TestFrameAnswers(t *testing.T) {
	var out bytes.Buffer
	in := `{"progress":"step\u001b[31m 1"}` + "\n" + `{"progress":"step 2"}` + "\n" + `{"ok":true,"lines":["x"]}` + "\n"
	resp, err := readAnswer(strings.NewReader(in), &out)
	if err != nil || !resp.OK || len(resp.Lines) != 1 {
		t.Fatalf("%v %+v", err, resp)
	}
	if out.String() != "step[31m 1\nstep 2\n" {
		t.Errorf("progress %q", out.String())
	}
	for name, bad := range map[string]string{
		"truncated":     `{"progress":"a"}` + "\n" + `{"ok":tr`,
		"only progress": `{"progress":"a"}` + "\n",
		"empty":         "",
		"garbage":       "nope\n",
	} {
		if _, err := readAnswer(strings.NewReader(bad), io.Discard); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// a nil stderr is fine
	if _, err := readAnswer(strings.NewReader(`{"progress":"a"}`+"\n"+`{"ok":true}`+"\n"), nil); err != nil {
		t.Error(err)
	}
}

func TestFrameNonce(t *testing.T) {
	n := strings.Repeat("c", 32)
	if err := readNonce(strings.NewReader(n+"\n"), n); err != nil {
		t.Fatal(err)
	}
	for _, in := range []string{n, strings.Repeat("d", 32) + "\n", "\n", "", n + n + n + "\n", n + "x\n"} {
		err := readNonce(strings.NewReader(in), n)
		if err == nil {
			t.Errorf("accepted %q", in)
			continue
		}
		if strings.Contains(err.Error(), n) || (strings.TrimSpace(in) != "" && strings.Contains(err.Error(), strings.TrimSpace(in))) {
			t.Errorf("error echoes input: %v", err)
		}
	}
}

func TestProgressWriterLines(t *testing.T) {
	var b bytes.Buffer
	if _, err := (progressWriter{&b}).Write([]byte("a\nb\n")); err != nil {
		t.Fatal(err)
	}
	if b.String() != `{"progress":"a"}`+"\n"+`{"progress":"b"}`+"\n" {
		t.Errorf("%q", b.String())
	}
}
