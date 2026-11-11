// Package ctl speaks the camera's control port (8091, spec 04) the way the bash wizard does
// through `printf 'GET...\n' | nc -w 2 127.0.0.1 8091` on the camera: one GET/SET line each,
// answers without the asynchronous LOG lines. The connection is an SSH direct-tcpip to the
// camera's own loopback, so the camera accepts writes (loopback trust model, unchanged).
package ctl

import (
	"bufio"
	"net"
	"regexp"
	"strings"
	"time"
)

// Idle is how long to wait for the next answer (nc -w 2).
const Idle = 2 * time.Second

// Port is the control port.
const Port = 8091

// Exchange sends every line, then reads answers until one per line arrived (LOG lines never
// count and are dropped), the camera closes the connection, or nothing came for idle.
func Exchange(conn net.Conn, lines []string, idle time.Duration) []string {
	defer conn.Close()
	var b strings.Builder
	for _, l := range lines {
		b.WriteString(l)
		b.WriteString("\n")
	}
	_ = conn.SetWriteDeadline(time.Now().Add(idle))
	if _, err := conn.Write([]byte(b.String())); err != nil {
		return nil
	}
	r := bufio.NewReader(conn)
	var out []string
	for len(out) < len(lines) {
		_ = conn.SetReadDeadline(time.Now().Add(idle))
		l, err := r.ReadString('\n')
		l = strings.TrimRight(l, "\r\n")
		if l != "" && !strings.HasPrefix(l, "LOG ") {
			out = append(out, l)
		}
		if err != nil {
			break
		}
	}
	return out
}

// Get is led_get: the value of KEY in the answers (text after "KEY=" up to the first space),
// empty when absent.
func Get(key string, answers []string) string {
	v := ""
	for _, l := range answers {
		if strings.HasPrefix(l, key+"=") {
			v = l[len(key)+1:]
			if i := strings.IndexByte(v, ' '); i >= 0 {
				v = v[:i]
			}
		}
	}
	return v
}

// SetResult is the outcome of a SET.
type SetResult struct {
	OK       bool
	Err      string // text after "ERR "
	NotSaved bool
	Restart  bool
	Line     string
	Answered bool
}

var okErrRe = regexp.MustCompile(`^(OK|ERR) `)

// ParseSet reads the answer to "SET key value" (ctl_set in steps_camera.sh: the first OK/ERR line).
func ParseSet(answers []string) SetResult {
	for _, l := range answers {
		if !okErrRe.MatchString(l) {
			continue
		}
		r := SetResult{Line: l, Answered: true}
		if strings.HasPrefix(l, "OK") {
			r.OK = true
			r.NotSaved = strings.Contains(l, "not saved")
			r.Restart = strings.Contains(l, "restart to apply")
		} else {
			r.Err = strings.TrimPrefix(l, "ERR ")
		}
		return r
	}
	return SetResult{}
}

// ParseLedSet is led_check_set: "OK key=value..." or "ERR ...key..." among the answers.
func ParseLedSet(key, value string, answers []string) SetResult {
	okPrefix := "OK " + key + "=" + value
	errRe := regexp.MustCompile("^ERR .*" + regexp.QuoteMeta(key))
	for _, l := range answers {
		if strings.HasPrefix(l, okPrefix) || errRe.MatchString(l) {
			r := SetResult{Line: l, Answered: true}
			if strings.HasPrefix(l, "OK") {
				r.OK = true
				r.NotSaved = strings.Contains(l, "not saved")
			} else {
				r.Err = strings.TrimPrefix(l, "ERR ")
			}
			return r
		}
	}
	return SetResult{}
}
