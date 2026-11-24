// Package ctl is the language the commands and the option engine speak to a 1080p: one GET or SET line per setting, in the
// key names the camera's text control port has always had, and answers in the same shape ("key=value", "OK key=value
// (note)", "ERR text"). The text port (8091) is closed to the network now: legacy.go turns those lines into calls of the
// signed API (port 8092) and the camera's answers back, and the parsers below read the answers (the same code that read
// the old port's, so the texts and the notes did not change).
package ctl

import (
	"regexp"
	"strings"
	"time"
)

// Timeout bounds one whole exchange with the camera, so that a hung camera cannot hang the assistant.
const Timeout = 10 * time.Second

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
