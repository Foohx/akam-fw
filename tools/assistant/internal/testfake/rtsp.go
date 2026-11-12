package testfake

import (
	"bufio"
	"crypto/md5" //nolint:gosec // RTSP Digest
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// RTSPServer is the fake RTSP endpoint: Digest login against the credentials file the real
// akam_rtsp_auth.sh wrote, the ONVIF audio backchannel (SDP with a sendonly track, interleaved
// RTP), and a record of what the client sent.
type RTSPServer struct {
	t         testing.TB
	ln        net.Listener
	credsFile string

	mu sync.Mutex
	// Refuse551 answers DESCRIBE with 551 (the camera does not offer talk).
	Refuse551 bool
	// Busy answers DESCRIBE with 503 (another client is connected: the camera serves one at a time),
	// with the Retry-After header when RetryAfter is not empty.
	Busy       bool
	RetryAfter string
	// NoBackchannel serves an SDP without the sendonly audio track.
	NoBackchannel bool
	// Codec is the payload type the SDP offers first: "PCMU" (default) or "PCMA".
	Codec string
	// Packets are the RTP packets received (header + payload, without the '$' framing).
	packets         [][]byte
	rtcp            [][]byte
	requests        []string // "METHOD uri" of every request, in order
	teardown        bool
	conns           []net.Conn
	wg              sync.WaitGroup
	nonceN          int
	describeRequire string
}

func newRTSPServer(t testing.TB, credsFile string) *RTSPServer {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &RTSPServer{t: t, ln: ln, credsFile: credsFile}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			s.mu.Lock()
			s.conns = append(s.conns, c)
			s.mu.Unlock()
			s.wg.Add(1)
			go func() { defer s.wg.Done(); s.serve(c) }()
		}
	}()
	return s
}

// Close stops the server and its connections.
func (s *RTSPServer) Close() {
	s.ln.Close()
	s.mu.Lock()
	for _, c := range s.conns {
		c.Close()
	}
	s.mu.Unlock()
	s.wg.Wait()
}

// Packets returns the RTP packets received.
func (s *RTSPServer) Packets() [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([][]byte(nil), s.packets...)
}

// RTCP returns the RTCP packets received.
func (s *RTSPServer) RTCP() [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([][]byte(nil), s.rtcp...)
}

// Requests returns "METHOD uri" for every request received.
func (s *RTSPServer) Requests() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.requests...)
}

// TornDown reports whether a TEARDOWN arrived.
func (s *RTSPServer) TornDown() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.teardown
}

func (s *RTSPServer) creds() (user, pass string, ok bool) {
	b, err := os.ReadFile(s.credsFile)
	if err != nil {
		return "", "", false
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, "user="); ok {
			user = v
		}
		if v, ok := strings.CutPrefix(l, "pass="); ok {
			pass = v
		}
	}
	return user, pass, user != ""
}

func md5hex(parts ...string) string {
	h := md5.New() //nolint:gosec
	for _, p := range parts {
		h.Write([]byte(p))
	}
	return hex.EncodeToString(h.Sum(nil))
}

func parseKV(h string) map[string]string {
	h = strings.TrimSpace(strings.TrimPrefix(h, "Digest"))
	out := map[string]string{}
	for _, p := range splitComma(h) {
		k, v, ok := strings.Cut(strings.TrimSpace(p), "=")
		if ok {
			out[strings.ToLower(k)] = strings.Trim(v, `"`)
		}
	}
	return out
}

func splitComma(s string) []string {
	var out []string
	in := false
	start := 0
	for i, r := range s {
		switch {
		case r == '"':
			in = !in
		case r == ',' && !in:
			out = append(out, s[start:i])
			start = i + 1
		}
	}
	return append(out, s[start:])
}

func (s *RTSPServer) serve(c net.Conn) {
	defer c.Close()
	br := bufio.NewReader(c)
	session := "SESS" + strconv.Itoa(int(time.Now().UnixNano()%100000))
	nonce := ""
	for {
		_ = c.SetReadDeadline(time.Now().Add(20 * time.Second))
		b, err := br.Peek(1)
		if err != nil {
			return
		}
		if b[0] == '$' { // interleaved frame from the client
			var h [4]byte
			if _, err := io.ReadFull(br, h[:]); err != nil {
				return
			}
			n := int(h[2])<<8 | int(h[3])
			body := make([]byte, n)
			if _, err := io.ReadFull(br, body); err != nil {
				return
			}
			s.mu.Lock()
			if h[1] == 0 {
				s.packets = append(s.packets, body)
			} else {
				s.rtcp = append(s.rtcp, body)
			}
			s.mu.Unlock()
			continue
		}
		line, err := br.ReadString('\n')
		if err != nil {
			return
		}
		f := strings.Fields(line)
		if len(f) < 3 {
			continue
		}
		method, uri := f[0], f[1]
		hdr := map[string]string{}
		for {
			l, err := br.ReadString('\n')
			if err != nil {
				return
			}
			l = strings.TrimRight(l, "\r\n")
			if l == "" {
				break
			}
			if k, v, ok := strings.Cut(l, ":"); ok {
				hdr[strings.ToLower(k)] = strings.TrimSpace(v)
			}
		}
		s.mu.Lock()
		s.requests = append(s.requests, method+" "+uri)
		if method == "DESCRIBE" {
			s.describeRequire = hdr["require"]
		}
		refuse, nobc, codec := s.Refuse551, s.NoBackchannel, s.Codec
		busy, retryAfter := s.Busy, s.RetryAfter
		s.mu.Unlock()
		cseq := hdr["cseq"]
		reply := func(code int, text string, extra string, body string) {
			var sb strings.Builder
			fmt.Fprintf(&sb, "RTSP/1.0 %d %s\r\nCSeq: %s\r\n%s", code, text, cseq, extra)
			if body != "" {
				fmt.Fprintf(&sb, "Content-Type: application/sdp\r\nContent-Length: %d\r\n", len(body))
			}
			sb.WriteString("\r\n" + body)
			_, _ = c.Write([]byte(sb.String()))
		}
		// Digest login
		user, pass, have := s.creds()
		if have {
			ok := false
			if a := hdr["authorization"]; a != "" {
				kv := parseKV(a)
				ha1 := md5hex(user, ":akam:", pass)
				ha2 := md5hex(method, ":", kv["uri"])
				var want string
				if kv["qop"] == "auth" {
					want = md5hex(ha1, ":", nonce, ":", kv["nc"], ":", kv["cnonce"], ":auth:", ha2)
				} else {
					want = md5hex(ha1, ":", nonce, ":", ha2)
				}
				ok = nonce != "" && kv["nonce"] == nonce && kv["username"] == user && kv["response"] == want
			}
			if !ok {
				s.mu.Lock()
				s.nonceN++
				nonce = md5hex("nonce", strconv.Itoa(s.nonceN), time.Now().String())
				s.mu.Unlock()
				reply(401, "Unauthorized", fmt.Sprintf("WWW-Authenticate: Digest realm=\"akam\", nonce=\"%s\", qop=\"auth\"\r\n", nonce), "")
				continue
			}
		}
		switch method {
		case "DESCRIBE":
			if busy {
				extra := ""
				if retryAfter != "" {
					extra = "Retry-After: " + retryAfter + "\r\n"
				}
				reply(503, "Service Unavailable", extra, "")
				continue
			}
			if refuse || !have { // no stream password, no talk (the camera never offers it)
				reply(551, "Option not supported", "", "")
				continue
			}
			sdp := "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=fake\r\nt=0 0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=recvonly\r\na=control:track0\r\n"
			if !nobc {
				pt := "m=audio 0 RTP/AVP 0 8\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n"
				if codec == "PCMA" {
					pt = "m=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000\r\n"
				}
				sdp += pt + "a=sendonly\r\na=control:track2\r\n"
			}
			reply(200, "OK", "Content-Base: rtsp://127.0.0.1/\r\n", sdp)
		case "SETUP":
			reply(200, "OK", "Session: "+session+";timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", "")
		case "PLAY":
			reply(200, "OK", "Session: "+session+"\r\n", "")
		case "GET_PARAMETER":
			reply(200, "OK", "", "")
		case "TEARDOWN":
			s.mu.Lock()
			s.teardown = true
			s.mu.Unlock()
			reply(200, "OK", "", "")
			return
		default:
			reply(405, "Method Not Allowed", "", "")
		}
	}
}

// DescribeRequire is the Require header of the last DESCRIBE.
func (s *RTSPServer) DescribeRequire() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.describeRequire
}

// SetBusy makes DESCRIBE answer 503 (with Retry-After when retryAfter is not empty), or stops doing so.
func (s *RTSPServer) SetBusy(busy bool, retryAfter string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.Busy, s.RetryAfter = busy, retryAfter
}

// Configure sets the server's behaviour under its lock.
func (s *RTSPServer) Configure(refuse551, noBackchannel bool, codec string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.Refuse551, s.NoBackchannel, s.Codec = refuse551, noBackchannel, codec
}
