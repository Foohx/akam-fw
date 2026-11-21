package akamapi

import (
	"bufio"
	"context"
	"encoding/hex"
	"errors"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"
)

type block map[string]string

func vectors(t *testing.T) []block {
	t.Helper()
	f, err := os.Open(filepath.Join("..", "..", "..", "..", "..", "tests", "fixtures", "control_api_vectors.txt"))
	if err != nil {
		t.Skip("skip: not a source checkout")
	}
	defer f.Close()
	var out []block
	cur := block{}
	flush := func() {
		if len(cur) > 0 {
			out = append(out, cur)
		}
		cur = block{}
	}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		l := sc.Text()
		if strings.HasPrefix(l, "#") {
			continue
		}
		if strings.TrimSpace(l) == "" {
			flush()
			continue
		}
		k, v, _ := strings.Cut(l, ":")
		cur[k] = strings.TrimPrefix(v, " ")
	}
	flush()
	return out
}

func TestSignAgainstTheSharedVectors(t *testing.T) {
	n, rej := 0, 0
	for _, b := range vectors(t) {
		if _, ok := b["case"]; ok {
			sec, _ := hex.DecodeString(b["secret"])
			var body []byte
			if bf := b["body_fill"]; bf != "" {
				bt, cnt, _ := strings.Cut(bf, ":")
				c, _ := strconv.Atoi(cnt)
				x, _ := hex.DecodeString(bt)
				body = []byte(strings.Repeat(string(x), c))
			} else if b["body_hex"] != "" {
				body, _ = hex.DecodeString(b["body_hex"])
			}
			cq, err := CanonicalQuery(b["query"])
			if err != nil || cq != b["canonical_query"] {
				t.Errorf("%s: canonical query %q (%v), want %q", b["case"], cq, err, b["canonical_query"])
			}
			h, err := Sign(Key{ID: b["key_id"], Secret: sec}, b["method"], b["path"], b["query"], body, b["timestamp"], b["nonce"])
			if err != nil {
				t.Errorf("%s: %v", b["case"], err)
				continue
			}
			if got := h.Get("X-Akam-Signature"); got != b["signature"] {
				t.Errorf("%s: signature %s, want %s", b["case"], got, b["signature"])
			}
			if strings.HasPrefix(b["timestamp"], "challenge:") != (h.Get("X-Akam-Challenge") != "") {
				t.Errorf("%s: challenge header", b["case"])
			}
			n++
		}
		if _, ok := b["reject"]; ok {
			var err error
			if b["kind"] == "path" {
				_, err = Sign(Key{ID: "3fa91c07", Secret: make([]byte, 32)}, "GET", b["value"], "", nil, "1", "2")
			} else {
				_, err = CanonicalQuery(b["value"])
			}
			if err == nil {
				t.Errorf("%s %q was not refused", b["kind"], b["value"])
			}
			rej++
		}
	}
	if n < 10 || rej < 10 {
		t.Fatalf("only %d cases and %d rejects read", n, rej)
	}
}

func TestKeyParsingAndNoSecretInText(t *testing.T) {
	sec := strings.Repeat("ab", 32)
	k, err := ParseKey("3fa91c07:" + sec + "\n")
	if err != nil || k.ID != "3fa91c07" || len(k.Secret) != 32 {
		t.Fatal(k, err)
	}
	if strings.Contains(k.String(), sec) {
		t.Fatal("the secret is shown")
	}
	for _, bad := range []string{"", "3fa91c07", "3FA91C07:" + sec, "3fa91c07:" + sec[:62], "xx:yy"} {
		if _, err := ParseKey(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	p := filepath.Join(t.TempDir(), "k")
	if err := os.WriteFile(p, []byte("3fa91c07:"+sec), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadKey(p); err == nil && runtime.GOOS != "windows" {
		t.Fatal("a key readable by others was accepted")
	}
	_ = os.Chmod(p, 0o600)
	if _, err := LoadKey(p); err != nil {
		t.Fatal(err)
	}
}

type reqSeen struct {
	path string
	h    Header
}

func reply(code int, body string) (int, []byte, error) { return code, []byte(body), nil }

const chal = "00112233445566778899aabbccddeeff"

func newFake(infoCode int, seen *reqSeen) *Client {
	sec, _ := hex.DecodeString(strings.Repeat("01", 32))
	return &Client{HostPort: "192.0.2.10:8092", Key: Key{ID: "3fa91c07", Secret: sec},
		Do: func(_ context.Context, _ string, path string, h Header) (int, []byte, error) {
			switch path {
			case "/akam/v1/hello":
				if h.Get("X-Akam-Signature") != "" {
					return reply(400, "")
				}
				return reply(200, `{"api":1,"clock_set":false,"challenge":"`+chal+`"}`)
			case "/akam/v1/info":
				if seen != nil {
					*seen = reqSeen{path, h}
				}
				return reply(infoCode, `{"api":1,"model":"lsc-rotatable-2k-ak3918av130","app":"0.2.0-abc","app_uptime_s":12,"clock_set":false,"key_id":"3fa91c07","scope":"read"}`)
			}
			return reply(404, "")
		}}
}

func TestInfoSignsWithTheChallenge(t *testing.T) {
	var seen reqSeen
	in, err := newFake(200, &seen).Info(context.Background())
	if err != nil || in.Model != "lsc-rotatable-2k-ak3918av130" || in.App != "0.2.0-abc" {
		t.Fatal(in, err)
	}
	if seen.h.Get("X-Akam-Challenge") != chal || seen.h.Get("X-Akam-Timestamp") != "" || len(seen.h.Get("X-Akam-Signature")) != 64 {
		t.Fatalf("headers %v", seen.h)
	}
	want, _ := Sign(newFake(200, nil).Key, "GET", "/akam/v1/info", "", nil, "challenge:"+chal, "challenge:"+chal)
	if want.Get("X-Akam-Signature") != seen.h.Get("X-Akam-Signature") {
		t.Fatal("signature differs")
	}
}

func TestInfoErrors(t *testing.T) {
	if _, err := newFake(401, nil).Info(context.Background()); !errors.Is(err, ErrAuth) {
		t.Fatalf("401: %v", err)
	}
	if _, err := newFake(500, nil).Info(context.Background()); err == nil || errors.Is(err, ErrAuth) {
		t.Fatalf("500: %v", err)
	}
	c := newFake(200, nil)
	c.Do = func(context.Context, string, string, Header) (int, []byte, error) {
		return 0, nil, errors.New("connection refused")
	}
	if _, err := c.Info(context.Background()); !errors.Is(err, ErrClosed) {
		t.Fatalf("refused: %v", err)
	}
	c.Do = func(context.Context, string, string, Header) (int, []byte, error) { return reply(404, "") }
	if _, err := c.Info(context.Background()); !errors.Is(err, ErrClosed) {
		t.Fatalf("404: %v", err)
	}
	c.Do = func(context.Context, string, string, Header) (int, []byte, error) { return reply(200, "<html>") }
	if _, err := c.Info(context.Background()); err == nil {
		t.Fatal("a non-Akam hello was accepted")
	}
}

// the raw client against a local server: request line, headers, bounded read, status and body
func TestRawClientAgainstALocalServer(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip("skip: no loopback")
	}
	defer ln.Close()
	got := make(chan string, 1)
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		buf := make([]byte, 4096)
		n, _ := c.Read(buf)
		got <- string(buf[:n])
		_, _ = c.Write([]byte("HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"))
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	code, body, err := rawGet(ctx, ln.Addr().String(), "/akam/v1/info", Header{"X-Akam-Key": "3fa91c07"})
	if err != nil || code != 401 || string(body) != "{}" {
		t.Fatal(code, string(body), err)
	}
	req := <-got
	if !strings.HasPrefix(req, "GET /akam/v1/info HTTP/1.1\r\n") || !strings.Contains(req, "X-Akam-Key: 3fa91c07\r\n") || !strings.Contains(req, "Connection: close") {
		t.Fatalf("request %q", req)
	}
	if _, _, err := rawGet(ctx, "127.0.0.1:1", "/x", nil); err == nil {
		t.Fatal("a closed port answered")
	}
}
