package sdcard

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
)

// Framing shared by the two elevation transports that cannot use stdin (macOS Unix socket, Windows named pipe).
// The conversation is:
//
//	helper -> parent   the nonce, one line (the parent compares it in constant time)
//	parent -> helper   exactly one JSON Request document (same type and 96 MiB cap as stdin, unknown fields refused)
//	helper -> parent   zero or more {"progress":"..."} lines, then one final Response line
//
// Everything that decides is here and is tested on Linux; the OS files only move bytes.

const (
	frameMaxAnswer = 8 << 20 // the helper's answers are small: a runaway one is cut
	frameMaxNonce  = 64
)

var nonceRe = regexp.MustCompile(`^[0-9a-f]{32}$`)

func randHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// sockLine is one line of the helper's answer: progress text, or the final Response.
type sockLine struct {
	Progress *string `json:"progress"`
	Response
}

// readNonce reads the helper's nonce line (at most 64 bytes) and compares it in constant time. The error never
// carries the nonce or what was received.
func readNonce(r io.Reader, nonce string) error {
	line, err := bufio.NewReaderSize(io.LimitReader(r, frameMaxNonce), frameMaxNonce).ReadString('\n')
	if err != nil {
		return errors.New("no nonce")
	}
	if subtle.ConstantTimeCompare([]byte(strings.TrimSuffix(line, "\n")), []byte(nonce)) != 1 {
		return errors.New("wrong nonce")
	}
	return nil
}

// writeRequest sends the request document and the newline that ends the frame (JSON has no raw newline).
func writeRequest(w io.Writer, body []byte) error {
	_, err := w.Write(append(append(make([]byte, 0, len(body)+1), body...), '\n'))
	return err
}

// readAnswer reads progress lines (shown through stderr, control characters removed) up to the final Response.
func readAnswer(r io.Reader, stderr io.Writer) (Response, error) {
	dec := json.NewDecoder(io.LimitReader(r, frameMaxAnswer))
	for {
		var l sockLine
		if err := dec.Decode(&l); err != nil {
			return Response{}, errors.New("the helper gave no usable answer")
		}
		if l.Progress != nil {
			if stderr != nil {
				fmt.Fprintln(stderr, sanitizeLine(*l.Progress))
			}
			continue
		}
		return l.Response, nil
	}
}

func sanitizeLine(s string) string {
	return strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, s)
}

// ---- helper side ----

// progressWriter turns the lines a helper writes into {"progress": "..."} lines on the connection.
type progressWriter struct{ w io.Writer }

func (p progressWriter) Write(b []byte) (int, error) {
	for _, ln := range strings.Split(strings.TrimRight(string(b), "\n"), "\n") {
		j, _ := json.Marshal(map[string]string{"progress": ln})
		if _, err := p.w.Write(append(j, '\n')); err != nil {
			return 0, err
		}
	}
	return len(b), nil
}

// readFrame reads the one request line, at most MaxRequest bytes. A stream that ends or overflows before the
// newline is refused.
func readFrame(r io.Reader) ([]byte, error) {
	br := bufio.NewReaderSize(io.LimitReader(r, MaxRequest+2), 64<<10)
	line, err := br.ReadBytes('\n')
	if err != nil || len(line) > MaxRequest+1 {
		return nil, errors.New("bad request frame")
	}
	return line, nil
}

// serveFramed answers one framed request on a stream that has no half-close (a named pipe): it reads the one
// request line, then HelperMain applies the same one-document rules as on stdin. The second line of a stream is
// never read. afterRead (may be nil) runs once the frame is in, e.g. to lift a deadline.
func serveFramed(c io.ReadWriter, h Helper, afterRead func()) int {
	line, err := readFrame(c)
	if afterRead != nil {
		afterRead()
	}
	if err != nil {
		_ = json.NewEncoder(c).Encode(fail("bad request"))
		return 2
	}
	return HelperMain(bytes.NewReader(line), c, h)
}
