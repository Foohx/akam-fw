// Package ctl speaks the camera's control port (8091, specs/control-port.md) the way the bash wizard does
// through `printf 'GET...\n' | nc -w 2 127.0.0.1 8091` on the camera: one GET/SET line each,
// answers without the asynchronous LOG lines. The camera's Dropbear has no TCP forwarding
// (DROPBEAR_SVR_LOCALTCPFWD 0), so the lines travel as the stdin of an SSH exec session running
// busybox nc on the camera's own loopback (loopback trust model, unchanged).
package ctl

import (
	"regexp"
	"strings"
	"time"
)

// Port is the control port.
const Port = 8091

// Timeout bounds one whole exchange (send and answers), so a hung camera cannot hang the assistant.
const Timeout = 10 * time.Second

// NcCommand is the command run on the camera: busybox nc. With -w 2 it gives up connecting after 2 s and
// ends after 2 s of idle input; normally it ends sooner, when the server closes after QUIT.
const NcCommand = "nc -w 2 127.0.0.1 8091"

// Input is the stdin of NcCommand: every line, then QUIT so the server closes at once.
func Input(lines []string) string {
	var b strings.Builder
	for _, l := range lines {
		b.WriteString(l)
		b.WriteString("\n")
	}
	b.WriteString("QUIT\n")
	return b.String()
}

// Answers filters what nc printed: CRLF trimmed, empty and LOG lines dropped, the "OK bye" answer to
// QUIT removed when it is the last line.
func Answers(stdout string) []string {
	var out []string
	for _, l := range strings.Split(stdout, "\n") {
		l = strings.TrimRight(l, "\r")
		if l != "" && !strings.HasPrefix(l, "LOG ") {
			out = append(out, l)
		}
	}
	if n := len(out); n > 0 && out[n-1] == "OK bye" {
		out = out[:n-1]
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
