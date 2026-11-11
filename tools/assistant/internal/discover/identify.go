package discover

import (
	"bufio"
	"context"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

// Identity is what an Akam camera tells about itself on the control port. Empty fields mean the
// camera did not say (an older image answers ERR for both keys).
type Identity struct {
	Version  string // fw.version, for example "b35-2af81c5"
	Hostname string // sys.hostname, for example "akam-01"
	Akam     bool   // the port answered the line protocol
}

const (
	identifyTimeout = 2 * time.Second
	identifyLineMax = 255
	identifyLines   = 8 // lines read in all, LOG pushes included
)

// Identify asks the control port of host (read-only, no login) for fw.version and sys.hostname.
// The only words ever sent are GET and QUIT. Everything read is bounded (2 s, 8 lines, 255 bytes
// per line) and is raw network text: callers pass it through ui.Sanitize before showing it.
func Identify(ctx context.Context, host string, port int, dial DialFunc) (Identity, error) {
	if dial == nil {
		dial = realDial(identifyTimeout)
	}
	ctx, cancel := context.WithTimeout(ctx, identifyTimeout)
	defer cancel()
	conn, err := dial(ctx, "tcp", net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		return Identity{}, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(identifyTimeout))
	r := bufio.NewReader(conn)
	var id Identity
	left := identifyLines
	for _, key := range []string{"fw.version", "sys.hostname"} {
		if _, err := io.WriteString(conn, "GET "+key+"\n"); err != nil {
			return id, err
		}
		for left > 0 {
			line, err := readLine(r)
			if err != nil {
				return id, err
			}
			left--
			if strings.HasPrefix(line, "LOG ") {
				continue
			}
			id.Akam = true // a line that is not a push: someone speaks the protocol
			if v, ok := strings.CutPrefix(line, key+"="); ok {
				if key == "fw.version" {
					id.Version = v
				} else {
					id.Hostname = v
				}
			}
			break
		}
	}
	_, _ = io.WriteString(conn, "QUIT\n")
	return id, nil
}

// readLine reads one line and keeps at most identifyLineMax bytes of it; the rest is dropped.
func readLine(r *bufio.Reader) (string, error) {
	var b []byte
	for {
		chunk, isPrefix, err := r.ReadLine()
		if err != nil {
			return "", err
		}
		if room := identifyLineMax - len(b); room > 0 {
			if len(chunk) > room {
				chunk = chunk[:room]
			}
			b = append(b, chunk...)
		}
		if !isPrefix {
			return string(b), nil
		}
	}
}
