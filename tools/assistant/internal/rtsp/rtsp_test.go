package rtsp

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"strconv"
	"strings"
	"testing"
	"time"
)

// ---- G.711: the vectors and properties of tests/native/g711_test.c ----

func refUlaw(code int) int {
	c := ^code & 0xFF
	sign, e, m := c>>7, (c>>4)&7, c&15
	mag := (((2*m + 33) << e) - 33) * 4
	if sign != 0 {
		return -mag
	}
	return mag
}

func refAlaw(code int) int {
	c := code ^ 0x55
	sign, e, m := c>>7, (c>>4)&7, c&15
	var mag int
	if e == 0 {
		mag = (2*m + 1) * 8
	} else {
		mag = ((2*m + 33) << (e - 1)) * 8
	}
	if sign != 0 {
		return mag
	}
	return -mag
}

func TestG711Exhaustive(t *testing.T) {
	for i := 0; i < 256; i++ {
		if int(UlawDecode(byte(i))) != refUlaw(i) {
			t.Fatalf("ulaw decode %02x", i)
		}
		if int(AlawDecode(byte(i))) != refAlaw(i) {
			t.Fatalf("alaw decode %02x", i)
		}
		if AlawEncode(AlawDecode(byte(i))) != byte(i) {
			t.Fatalf("alaw round trip %02x", i)
		}
		want := byte(i)
		if i == 0x7F {
			want = 0xFF
		}
		if UlawEncode(UlawDecode(byte(i))) != want {
			t.Fatalf("ulaw round trip %02x", i)
		}
	}
}

func TestG711Vectors(t *testing.T) {
	cases := []struct {
		got, want int
		name      string
	}{
		{int(UlawDecode(0xFF)), 0, "ulaw FF"}, {int(UlawDecode(0x7F)), 0, "ulaw 7F"},
		{int(UlawDecode(0x80)), 32124, "ulaw 80"}, {int(UlawDecode(0x00)), -32124, "ulaw 00"},
		{int(AlawDecode(0xD5)), 8, "alaw D5"}, {int(AlawDecode(0x55)), -8, "alaw 55"},
		{int(AlawDecode(0xAA)), 32256, "alaw AA"}, {int(AlawDecode(0x2A)), -32256, "alaw 2A"},
		{int(UlawEncode(0)), 0xFF, "ulaw enc 0"}, {int(AlawEncode(0)), 0xD5, "alaw enc 0"},
		{int(AlawEncode(-1)), 0x55, "alaw enc -1"}, {int(UlawEncode(32767)), 0x80, "ulaw enc max"},
		{int(UlawEncode(-32768)), 0x00, "ulaw enc min"}, {int(AlawEncode(32767)), 0xAA, "alaw enc max"},
		{int(AlawEncode(-32768)), 0x2A, "alaw enc min"},
	}
	for _, c := range cases {
		if c.got != c.want {
			t.Errorf("%s: got %d, want %d", c.name, c.got, c.want)
		}
	}
}

// ---- Digest: the example of RFC 2617 section 3.5 ----

func TestDigestRFC2617(t *testing.T) {
	d := Digest{User: "Mufasa", Pass: "Circle Of Life", CNonce: func() string { return "0a4f113b" }}
	ok := d.ParseChallenge(`Digest realm="testrealm@host.com", qop="auth,auth-int", nonce="dcd98b7102dd2f0e8b11d0f600bfb0c093", opaque="5ccc069c403ebaf9f0171e9517f40e41"`)
	if !ok || d.Realm != "testrealm@host.com" || !d.QopAuth {
		t.Fatalf("challenge not parsed: %+v", d)
	}
	h := d.Header("GET", "/dir/index.html")
	for _, want := range []string{`response="6629fae49393a05397450978507c4ef1"`, `nc=00000001`, `cnonce="0a4f113b"`, `qop=auth`, `username="Mufasa"`} {
		if !strings.Contains(h, want) {
			t.Errorf("header lacks %s: %s", want, h)
		}
	}
	if h2 := d.Header("GET", "/dir/index.html"); !strings.Contains(h2, "nc=00000002") {
		t.Errorf("nonce count did not advance: %s", h2)
	}
}

func TestDigestWithoutQop(t *testing.T) {
	d := Digest{User: "u", Pass: "p"}
	d.ParseChallenge(`Digest realm="akam", nonce="abc"`)
	h := d.Header("DESCRIBE", "rtsp://h/")
	want := md5hex(md5hex("u", ":akam:", "p"), ":abc:", md5hex("DESCRIBE", ":", "rtsp://h/"))
	if !strings.Contains(h, `response="`+want+`"`) || strings.Contains(h, "qop") {
		t.Errorf("RFC 2069 form wrong: %s", h)
	}
	if (&Digest{}).Header("GET", "/") != "" {
		t.Error("a header before any challenge")
	}
	if d.ParseChallenge("Basic realm=x") {
		t.Error("Basic is not Digest")
	}
}

// ---- WAV ----

func wavBytes(rate uint32, channels, bits uint16, data []byte) []byte {
	var b bytes.Buffer
	b.WriteString("RIFF")
	_ = binary.Write(&b, binary.LittleEndian, uint32(36+len(data)))
	b.WriteString("WAVEfmt ")
	_ = binary.Write(&b, binary.LittleEndian, uint32(16))
	_ = binary.Write(&b, binary.LittleEndian, uint16(1))
	_ = binary.Write(&b, binary.LittleEndian, channels)
	_ = binary.Write(&b, binary.LittleEndian, rate)
	_ = binary.Write(&b, binary.LittleEndian, rate*uint32(channels)*uint32(bits)/8)
	_ = binary.Write(&b, binary.LittleEndian, channels*bits/8)
	_ = binary.Write(&b, binary.LittleEndian, bits)
	b.WriteString("data")
	_ = binary.Write(&b, binary.LittleEndian, uint32(len(data)))
	b.Write(data)
	return b.Bytes()
}

func pcm16(s []int16) []byte {
	var b bytes.Buffer
	_ = binary.Write(&b, binary.LittleEndian, s)
	return b.Bytes()
}

func TestWAV8kMonoIsUntouched(t *testing.T) {
	in := []int16{0, 100, -100, 32767, -32768}
	out, err := ReadWAV(bytes.NewReader(wavBytes(8000, 1, 16, pcm16(in))))
	if err != nil || len(out) != len(in) {
		t.Fatalf("err=%v out=%v", err, out)
	}
	for i := range in {
		if out[i] != in[i] {
			t.Fatalf("sample %d: %d != %d", i, out[i], in[i])
		}
	}
}

func TestWAVStereoAndResample(t *testing.T) {
	// 16 kHz stereo, L=1000 R=3000 -> mono 2000 at 8 kHz, half as many samples
	var s []int16
	for i := 0; i < 1600; i++ {
		s = append(s, 1000, 3000)
	}
	out, err := ReadWAV(bytes.NewReader(wavBytes(16000, 2, 16, pcm16(s))))
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != 800 {
		t.Fatalf("len %d, want 800", len(out))
	}
	for i, v := range out[200:] { // after the start-up of the anti-aliasing filter
		if v < 1990 || v > 2010 {
			t.Fatalf("sample %d = %d, want about 2000", 200+i, v)
		}
	}
	// 8-bit, 44.1 kHz mono: silence stays silence
	out, err = ReadWAV(bytes.NewReader(wavBytes(44100, 1, 8, bytes.Repeat([]byte{128}, 4410))))
	if err != nil || len(out) != 800 {
		t.Fatalf("8 bit: err=%v len=%d", err, len(out))
	}
	for _, v := range out {
		if v != 0 {
			t.Fatal("silence changed")
		}
	}
}

func TestWAVRejects(t *testing.T) {
	if _, err := ReadWAV(strings.NewReader("hello world, this is not a wav")); !errors.Is(err, ErrNotWAV) {
		t.Errorf("not WAV: %v", err)
	}
	if _, err := ReadWAV(bytes.NewReader(wavBytes(8000, 1, 12, []byte{1, 2}))); err == nil {
		t.Error("12 bit accepted")
	}
	if _, err := ReadWAV(bytes.NewReader(wavBytes(8000, 1, 16, nil)[:30])); err == nil {
		t.Error("truncated header accepted")
	}
}

// ---- a tiny in-test RTSP server for the protocol details ----

func TestTalkSendsTheFramesOfAkamTalk(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var frames [][]byte
	var reqs []string
	done := make(chan struct{})
	go func() {
		defer close(done)
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		buf := make([]byte, 0, 4096)
		tmp := make([]byte, 2048)
		resp := func(code string, extra, body string) {
			c.Write([]byte("RTSP/1.0 " + code + "\r\nCSeq: 1\r\n" + extra + "\r\n" + body))
		}
		for {
			n, err := c.Read(tmp)
			if err != nil {
				return
			}
			buf = append(buf, tmp[:n]...)
			for len(buf) > 0 {
				if buf[0] == '$' {
					if len(buf) < 4 {
						break
					}
					l := int(buf[2])<<8 | int(buf[3])
					if len(buf) < 4+l {
						break
					}
					frames = append(frames, append([]byte(nil), buf[:4+l]...))
					buf = buf[4+l:]
					continue
				}
				i := bytes.Index(buf, []byte("\r\n\r\n"))
				if i < 0 {
					break
				}
				head := string(buf[:i])
				buf = buf[i+4:]
				reqs = append(reqs, strings.SplitN(head, "\r\n", 2)[0])
				switch {
				case strings.HasPrefix(head, "DESCRIBE"):
					sdp := "v=0\r\nm=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendonly\r\na=control:track2\r\n"
					resp("200 OK", "Content-Length: "+itoa(len(sdp))+"\r\n", sdp)
				case strings.HasPrefix(head, "SETUP"):
					resp("200 OK", "Session: S1\r\nTransport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n", "")
				case strings.HasPrefix(head, "PLAY"):
					resp("200 OK", "Session: S1\r\n", "")
				case strings.HasPrefix(head, "TEARDOWN"):
					resp("200 OK", "", "")
					return
				}
			}
		}
	}()
	pcm := make([]int16, 400) // 2.5 packets of 160 samples
	for i := range pcm {
		pcm[i] = int16(i * 50)
	}
	rnd := []uint32{0xAAAA1234, 0x01020304, 0xDEADBEEF}
	k := 0
	err = Talk(context.Background(), TalkConfig{
		Host: "127.0.0.1", Port: ln.Addr().(*net.TCPAddr).Port, User: "u", Password: "pw",
		Source: &SliceSource{S: pcm}, Unpaced: true,
		Rand: func() uint32 { v := rnd[k%3]; k++; return v },
	})
	if err != nil {
		t.Fatal(err)
	}
	<-done
	if len(frames) != 4 { // 3 RTP packets + RTCP BYE
		t.Fatalf("%d frames", len(frames))
	}
	for i, n := range []int{160, 160, 80} {
		f := frames[i]
		if f[0] != '$' || f[1] != 4 || int(f[3]) != 12+n || len(f) != 16+n {
			t.Fatalf("packet %d framing: % x", i, f[:8])
		}
		if f[4] != 0x80 {
			t.Errorf("packet %d version byte %02x", i, f[4])
		}
		wantPT := byte(0)
		if i == 0 {
			wantPT |= 0x80 // marker on the first packet only
		}
		if f[5] != wantPT {
			t.Errorf("packet %d payload type byte %02x, want %02x", i, f[5], wantPT)
		}
		if seq := binary.BigEndian.Uint16(f[6:]); seq != uint16(0x1234+i) {
			t.Errorf("packet %d seq %04x", i, seq)
		}
		ts := binary.BigEndian.Uint32(f[8:])
		wantTS := uint32(0x01020304) + uint32(160*i)
		if ts != wantTS {
			t.Errorf("packet %d timestamp %08x, want %08x", i, ts, wantTS)
		}
		if ssrc := binary.BigEndian.Uint32(f[12:]); ssrc != 0xDEADBEEF {
			t.Errorf("ssrc %08x", ssrc)
		}
		for j := 0; j < n; j++ {
			if f[16+j] != UlawEncode(pcm[i*160+j]) {
				t.Fatalf("packet %d payload byte %d", i, j)
			}
		}
	}
	bye := frames[3]
	if !bytes.Equal(bye[:8], []byte{'$', 5, 0, 8, 0x81, 203, 0, 1}) || binary.BigEndian.Uint32(bye[8:]) != 0xDEADBEEF {
		t.Errorf("RTCP BYE: % x", bye)
	}
	joined := strings.Join(reqs, "|")
	for _, want := range []string{"DESCRIBE rtsp://127.0.0.1:", "SETUP rtsp://127.0.0.1:", "/track2 RTSP/1.0", "PLAY ", "TEARDOWN "} {
		if !strings.Contains(joined, want) {
			t.Errorf("request list lacks %q: %s", want, joined)
		}
	}
}

func itoa(n int) string { return strconv.Itoa(n) }

func TestTalkErrorCodes(t *testing.T) {
	serve := func(reply string) (int, func()) {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		go func() {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			defer c.Close()
			tmp := make([]byte, 4096)
			_, _ = c.Read(tmp)
			io.WriteString(c, reply)
			time.Sleep(100 * time.Millisecond)
		}()
		return ln.Addr().(*net.TCPAddr).Port, func() { ln.Close() }
	}
	cases := []struct {
		name, reply string
		pass        string
		code        int
	}{
		{"551", "RTSP/1.0 551 Option not supported\r\nCSeq: 1\r\n\r\n", "pw", CodeRefused},
		{"401 without a password", "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Digest realm=\"akam\", nonce=\"n\"\r\n\r\n", "", CodeLogin},
		{"500", "RTSP/1.0 500 Oops\r\nCSeq: 1\r\n\r\n", "pw", CodeProto},
		{"no backchannel", "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 4\r\n\r\nv=0\n", "pw", CodeRefused},
	}
	for _, c := range cases {
		port, stop := serve(c.reply)
		err := Talk(context.Background(), TalkConfig{Host: "127.0.0.1", Port: port, User: "u", Password: c.pass, Source: &SliceSource{S: Tone(1)}, Unpaced: true})
		stop()
		var te *TalkError
		if !errors.As(err, &te) || te.Code != c.code {
			t.Errorf("%s: err=%v, want code %d", c.name, err, c.code)
		}
	}
	// nothing listens
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()
	err := Talk(context.Background(), TalkConfig{Host: "127.0.0.1", Port: port, User: "u", Source: &SliceSource{}})
	var te *TalkError
	if !errors.As(err, &te) || te.Code != CodeNet {
		t.Errorf("connect failure: %v", err)
	}
}

func TestSDPBackchannel(t *testing.T) {
	sdp := "v=0\r\nm=video 0 RTP/AVP 96\r\na=recvonly\r\na=control:track0\r\nm=audio 0 RTP/AVP 0 8\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\na=sendonly\r\na=control:track2\r\n"
	tr, ok := sdpBackchannel(sdp)
	if !ok || tr.control != "track2" || !tr.pcmu || !tr.pcma {
		t.Fatalf("%+v %v", tr, ok)
	}
	if _, ok := sdpBackchannel("v=0\r\nm=audio 0 RTP/AVP 0\r\na=recvonly\r\na=control:x\r\n"); ok {
		t.Error("a recvonly audio track is not a backchannel")
	}
}

func TestS16LESource(t *testing.T) {
	var b bytes.Buffer
	_ = binary.Write(&b, binary.LittleEndian, []int16{1, -2, 3})
	s := &S16LESource{R: &b}
	p := make([]int16, 160)
	n, _ := s.ReadSamples(p)
	if n != 3 || p[0] != 1 || p[1] != -2 || p[2] != 3 {
		t.Errorf("n=%d %v", n, p[:3])
	}
	if n, err := s.ReadSamples(p); n != 0 || err != io.EOF {
		t.Errorf("end: %d %v", n, err)
	}
}

func TestTalkBusyCameraIsReported(t *testing.T) {
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer ln.Close()
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		tmp := make([]byte, 4096)
		_, _ = c.Read(tmp)
		io.WriteString(c, "RTSP/1.0 503 Service Unavailable\r\nCSeq: 1\r\nRetry-After: 30\r\n\r\n")
		time.Sleep(100 * time.Millisecond)
	}()
	err := Talk(context.Background(), TalkConfig{Host: "127.0.0.1", Port: ln.Addr().(*net.TCPAddr).Port, User: "u", Password: "pw", Source: &SliceSource{S: Tone(1)}, Unpaced: true})
	var te *TalkError
	if !errors.As(err, &te) || !te.Busy || te.RetryAfter != "30" || te.Code != CodeProto {
		t.Fatalf("err=%v %+v", err, te)
	}
}
