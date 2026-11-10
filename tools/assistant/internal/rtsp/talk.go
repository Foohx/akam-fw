package rtsp

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

// Exit codes of akam-talk, kept as the codes of TalkError.
const (
	CodeNet     = 2 // network
	CodeRefused = 3 // the camera refuses talk (551, or no backchannel in the SDP)
	CodeLogin   = 4 // login refused
	CodeProto   = 5 // protocol error
	CodeNoAudio = 6 // the input held no audio
)

// TalkError is a failure with the exit code of akam-talk.
type TalkError struct {
	Code int
	Msg  string
}

func (e *TalkError) Error() string { return e.Msg }

func terr(code int, format string, a ...any) *TalkError {
	return &TalkError{Code: code, Msg: fmt.Sprintf(format, a...)}
}

const (
	backchannelTag = "www.onvif.org/ver20/backchannel"
	ptimeMS        = 20
	samplesPerPkt  = 8 * ptimeMS
	keepalive      = 25 * time.Second
	respTimeout    = 8 * time.Second
	maxResp        = 16384
)

// PCMSource yields 8 kHz mono 16-bit samples; it returns io.EOF at the end.
type PCMSource interface {
	// ReadSamples fills p (up to len(p) samples) and returns how many; (0, io.EOF) at the end.
	ReadSamples(p []int16) (int, error)
}

// SliceSource plays a decoded buffer (a WAV file or a tone).
type SliceSource struct {
	S []int16
	i int
}

func (s *SliceSource) ReadSamples(p []int16) (int, error) {
	if s.i >= len(s.S) {
		return 0, io.EOF
	}
	n := copy(p, s.S[s.i:])
	s.i += n
	return n, nil
}

// S16LESource reads raw signed 16-bit little-endian samples (microphone or ffmpeg output).
type S16LESource struct {
	R   io.Reader
	buf []byte
}

func (s *S16LESource) ReadSamples(p []int16) (int, error) {
	need := len(p) * 2
	if cap(s.buf) < need {
		s.buf = make([]byte, need)
	}
	b := s.buf[:need]
	n, err := io.ReadFull(s.R, b)
	n &^= 1
	for i := 0; i < n/2; i++ {
		p[i] = int16(binary.LittleEndian.Uint16(b[2*i:]))
	}
	if n == 0 && err != nil {
		return 0, io.EOF
	}
	return n / 2, nil
}

// TalkConfig describes one talk session.
type TalkConfig struct {
	Host     string
	Port     int
	User     string
	Password string
	Source   PCMSource
	// Codec forces "pcmu" or "pcma"; empty picks PCMU when the camera offers it.
	Codec string
	// Unpaced sends as fast as possible (tests only).
	Unpaced bool
	Logf    func(format string, a ...any)
	// Rand supplies the RTP start values (tests make them deterministic).
	Rand func() uint32
	// Dial overrides net.Dial (tests).
	Dial func(ctx context.Context, addr string) (net.Conn, error)
}

type conn struct {
	c       net.Conn
	br      *bufio.Reader
	base    string
	d       Digest
	session string
	cseq    int
	logf    func(string, ...any)
}

type response struct {
	code    int
	headers map[string]string // lower-case names
	body    string
}

func (r *response) h(name string) string { return r.headers[strings.ToLower(name)] }

// readResponse takes the next RTSP response, discarding interleaved frames ('$' ch len data).
func (c *conn) readResponse(timeout time.Duration) (*response, error) {
	_ = c.c.SetReadDeadline(time.Now().Add(timeout))
	for {
		b, err := c.br.Peek(1)
		if err != nil {
			return nil, err
		}
		switch b[0] {
		case '\r', '\n':
			_, _ = c.br.ReadByte()
			continue
		case '$':
			var h [4]byte
			if _, err := io.ReadFull(c.br, h[:]); err != nil {
				return nil, err
			}
			n := int(h[2])<<8 | int(h[3])
			if _, err := io.CopyN(io.Discard, c.br, int64(n)); err != nil {
				return nil, err
			}
			continue
		}
		break
	}
	line, err := c.br.ReadString('\n')
	if err != nil {
		return nil, err
	}
	line = strings.TrimRight(line, "\r\n")
	if !strings.HasPrefix(line, "RTSP/") {
		return nil, errors.New("not an RTSP response")
	}
	f := strings.Fields(line)
	if len(f) < 2 {
		return nil, errors.New("bad status line")
	}
	code, err := strconv.Atoi(f[1])
	if err != nil {
		return nil, errors.New("bad status line")
	}
	r := &response{code: code, headers: map[string]string{}}
	total := 0
	for {
		l, err := c.br.ReadString('\n')
		if err != nil {
			return nil, err
		}
		total += len(l)
		if total > maxResp {
			return nil, errors.New("response too large")
		}
		l = strings.TrimRight(l, "\r\n")
		if l == "" {
			break
		}
		if i := strings.IndexByte(l, ':'); i > 0 {
			k := strings.ToLower(strings.TrimSpace(l[:i]))
			if _, dup := r.headers[k]; !dup {
				r.headers[k] = strings.TrimSpace(l[i+1:])
			}
		}
	}
	if cl := r.headers["content-length"]; cl != "" {
		n, err := strconv.Atoi(cl)
		if err != nil || n < 0 || n > maxResp {
			return nil, errors.New("bad Content-Length")
		}
		body := make([]byte, n)
		if _, err := io.ReadFull(c.br, body); err != nil {
			return nil, err
		}
		r.body = string(body)
	}
	return r, nil
}

func (c *conn) write(b []byte) error {
	_ = c.c.SetWriteDeadline(time.Now().Add(respTimeout))
	_, err := c.c.Write(b)
	return err
}

// request sends one request and reads its response, answering a 401 once with Digest.
func (c *conn) request(method, uri, extra string) (*response, error) {
	for attempt := 0; attempt < 2; attempt++ {
		c.cseq++
		var b strings.Builder
		fmt.Fprintf(&b, "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: akam-talk\r\n", method, uri, c.cseq)
		if c.session != "" {
			fmt.Fprintf(&b, "Session: %s\r\n", c.session)
		}
		if h := c.d.Header(method, uri); h != "" {
			b.WriteString(h + "\r\n")
		}
		b.WriteString(extra)
		b.WriteString("\r\n")
		if err := c.write([]byte(b.String())); err != nil {
			return nil, err
		}
		r, err := c.readResponse(respTimeout)
		if err != nil {
			return nil, err
		}
		if r.code == 401 && attempt == 0 && c.d.Pass != "" {
			if c.d.ParseChallenge(r.h("WWW-Authenticate")) {
				continue
			}
		}
		return r, nil
	}
	return nil, errors.New("login refused")
}

// parseURLBase builds rtsp://host:port with no credentials.
func baseURL(host string, port int) string {
	if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}
	return "rtsp://" + host + ":" + strconv.Itoa(port)
}

type sdpTrack struct {
	control   string
	pcmu, pcma bool
}

// sdpBackchannel finds the media section m=audio with a=sendonly (the camera listens).
func sdpBackchannel(sdp string) (sdpTrack, bool) {
	sections := strings.Split("\n"+sdp, "\nm=")
	for _, sec := range sections[1:] {
		if !strings.HasPrefix(sec, "audio") || !strings.Contains(sec, "a=sendonly") {
			continue
		}
		i := strings.Index(sec, "a=control:")
		if i < 0 {
			continue
		}
		ctl := sec[i+len("a=control:"):]
		if e := strings.IndexAny(ctl, "\r\n"); e >= 0 {
			ctl = ctl[:e]
		}
		return sdpTrack{control: ctl, pcmu: strings.Contains(sec, "PCMU/8000"), pcma: strings.Contains(sec, "PCMA/8000")}, true
	}
	return sdpTrack{}, false
}

func defaultRand() uint32 {
	var b [4]byte
	_, _ = rand.Read(b[:])
	return binary.BigEndian.Uint32(b[:])
}

// Talk runs one session: DESCRIBE (backchannel), SETUP, PLAY, RTP every 20 ms, RTCP BYE,
// TEARDOWN. Cancelling ctx ends it cleanly (BYE and TEARDOWN are still sent).
func Talk(ctx context.Context, cfg TalkConfig) error {
	logf := cfg.Logf
	if logf == nil {
		logf = func(string, ...any) {}
	}
	rnd := cfg.Rand
	if rnd == nil {
		rnd = defaultRand
	}
	if cfg.Port == 0 {
		cfg.Port = 554
	}
	addr := net.JoinHostPort(cfg.Host, strconv.Itoa(cfg.Port))
	dial := cfg.Dial
	if dial == nil {
		dial = func(ctx context.Context, a string) (net.Conn, error) {
			d := net.Dialer{Timeout: 10 * time.Second}
			return d.DialContext(ctx, "tcp", a)
		}
	}
	nc, err := dial(ctx, addr)
	if err != nil {
		return terr(CodeNet, "cannot connect to %s port %d", cfg.Host, cfg.Port)
	}
	// closing the socket is the one cleanup: it also ends the reader goroutine below
	defer nc.Close()
	if tc, ok := nc.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}
	logf("connected to %s", addr)
	c := &conn{c: nc, br: bufio.NewReaderSize(nc, 4096), base: baseURL(cfg.Host, cfg.Port), logf: logf}
	c.d.User, c.d.Pass = cfg.User, cfg.Password

	r, err := c.request("DESCRIBE", c.base, "Require: "+backchannelTag+"\r\nAccept: application/sdp\r\n")
	if err != nil {
		return terr(CodeNet, "no answer to DESCRIBE")
	}
	switch {
	case r.code == 551:
		return terr(CodeRefused, "the camera does not offer talk: the RTSP password must be on (./akam rtsp-auth set), audio out enabled and audio.talk_enable=1")
	case r.code == 401 && cfg.Password == "":
		return terr(CodeLogin, "the camera asks for a password: set AKAM_RTSP_PASSWORD")
	case r.code == 401:
		return terr(CodeLogin, "login refused (wrong user or password)")
	case r.code != 200:
		return terr(CodeProto, "DESCRIBE answered %d", r.code)
	}
	trk, ok := sdpBackchannel(r.body)
	if !ok {
		return terr(CodeRefused, "the camera sent no audio backchannel in its description")
	}
	pcmu := trk.pcmu
	switch cfg.Codec {
	case "":
		if !trk.pcmu && !trk.pcma {
			return terr(CodeRefused, "the camera offers neither PCMU nor PCMA")
		}
	case "pcmu", "pcma":
		pcmu = cfg.Codec == "pcmu"
		if (pcmu && !trk.pcmu) || (!pcmu && !trk.pcma) {
			return terr(CodeRefused, "the camera does not offer %s", cfg.Codec)
		}
	default:
		return terr(CodeProto, "unknown codec %q", cfg.Codec)
	}
	base := c.base
	if cb := r.h("Content-Base"); cb != "" {
		base = strings.TrimRight(cb, "/")
	}
	track := trk.control
	if !strings.HasPrefix(strings.ToLower(track), "rtsp://") {
		track = base + "/" + track
	}
	r, err = c.request("SETUP", track, "Require: "+backchannelTag+"\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
	if err != nil {
		return terr(CodeNet, "no answer to SETUP")
	}
	if r.code == 551 || r.code == 404 {
		return terr(CodeRefused, "the camera refused the talk track (%d)", r.code)
	}
	if r.code != 200 {
		return terr(CodeProto, "SETUP answered %d", r.code)
	}
	rtpCh, rtcpCh := 0, 1
	if tv := r.h("Transport"); tv != "" {
		if i := strings.Index(tv, "interleaved="); i >= 0 {
			spec := tv[i+len("interleaved="):]
			if e := strings.IndexAny(spec, "; "); e >= 0 {
				spec = spec[:e]
			}
			a, b, two := strings.Cut(spec, "-")
			x, e1 := strconv.Atoi(a)
			y, e2 := x+1, error(nil)
			if two {
				y, e2 = strconv.Atoi(b)
			}
			if e1 != nil || e2 != nil {
				return terr(CodeProto, "SETUP answered an invalid interleaved channel pair")
			}
			rtpCh, rtcpCh = x, y
		}
		if rtpCh < 0 || rtpCh > 255 || rtcpCh < 0 || rtcpCh > 255 || rtpCh == rtcpCh {
			return terr(CodeProto, "SETUP answered an invalid interleaved channel pair")
		}
	}
	sess := r.h("Session")
	if sess == "" {
		return terr(CodeProto, "SETUP gave no Session")
	}
	if i := strings.IndexByte(sess, ';'); i >= 0 {
		sess = sess[:i]
	}
	if len(sess) > 63 {
		sess = sess[:63]
	}
	c.session = sess
	logf("session set up, RTP on channel %d", rtpCh)

	r, err = c.request("PLAY", base, "Require: "+backchannelTag+"\r\nRange: npt=0.000-\r\n")
	if err != nil {
		return terr(CodeNet, "no answer to PLAY")
	}
	if r.code != 200 {
		return terr(CodeProto, "PLAY answered %d", r.code)
	}
	logf("talking")

	// Reader: notices a closed connection and swallows keep-alive answers and RTCP.
	dead := make(chan struct{})
	go func() {
		defer close(dead)
		for {
			if _, err := c.readResponse(time.Hour); err != nil {
				return
			}
		}
	}()

	seq := uint16(rnd())
	ts := rnd()
	ssrc := rnd()
	t0 := time.Now()
	lastKA := t0
	packets, sent := 0, 0
	var rcErr error
	pcm := make([]int16, samplesPerPkt)
	frame := make([]byte, 4+12+samplesPerPkt)
	stopped := false
loop:
	for {
		select {
		case <-ctx.Done():
			stopped = true
			break loop
		case <-dead:
			rcErr = terr(CodeNet, "the camera closed the connection")
			break loop
		default:
		}
		n, _ := cfg.Source.ReadSamples(pcm)
		if n == 0 {
			break
		}
		pl := frame[16 : 16+n]
		for i := 0; i < n; i++ {
			if pcmu {
				pl[i] = UlawEncode(pcm[i])
			} else {
				pl[i] = AlawEncode(pcm[i])
			}
		}
		pt := byte(8)
		if pcmu {
			pt = 0
		}
		if packets == 0 {
			pt |= 0x80
		}
		frame[0], frame[1], frame[2], frame[3] = '$', byte(rtpCh), 0, byte(12+n)
		frame[4], frame[5] = 0x80, pt
		binary.BigEndian.PutUint16(frame[6:], seq)
		binary.BigEndian.PutUint32(frame[8:], ts)
		binary.BigEndian.PutUint32(frame[12:], ssrc)
		if err := c.write(frame[:16+n]); err != nil {
			rcErr = terr(CodeNet, "the camera closed the connection")
			break
		}
		seq++
		ts += uint32(n)
		sent += n
		packets++
		if !cfg.Unpaced {
			due := t0.Add(time.Duration(packets) * ptimeMS * time.Millisecond)
			if d := time.Until(due); d > 0 {
				select {
				case <-time.After(d):
				case <-ctx.Done():
				}
			} else if -d > 200*time.Millisecond { // a live source that fell behind: resync
				t0 = time.Now().Add(-time.Duration(packets) * ptimeMS * time.Millisecond)
			}
		}
		if time.Since(lastKA) >= keepalive {
			c.cseq++
			ka := fmt.Sprintf("GET_PARAMETER %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n", base, c.cseq, c.session)
			if err := c.write([]byte(ka)); err != nil {
				rcErr = terr(CodeNet, "the camera closed the connection")
				break
			}
			lastKA = time.Now()
		}
	}
	if rcErr == nil || stopped {
		bye := []byte{'$', byte(rtcpCh), 0, 8, 0x81, 203, 0, 1, byte(ssrc >> 24), byte(ssrc >> 16), byte(ssrc >> 8), byte(ssrc)}
		_ = c.write(bye)
		c.cseq++
		td := fmt.Sprintf("TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\nRequire: %s\r\n\r\n", base, c.cseq, c.session, backchannelTag)
		if h := c.d.Header("TEARDOWN", base); h != "" {
			td = strings.Replace(td, "\r\n\r\n", "\r\n"+h+"\r\n\r\n", 1)
		}
		_ = c.write([]byte(td))
		// give the camera a moment to answer before the socket is closed
		select {
		case <-dead:
		case <-time.After(500 * time.Millisecond):
		}
	}
	logf("sent %d samples in %d packets", sent, packets)
	if rcErr != nil {
		return rcErr
	}
	if packets == 0 && !stopped {
		return terr(CodeNoAudio, "no audio was read from the input")
	}
	return nil
}
