package image

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/childenv"
	"os/exec"
	"regexp"
	"strings"
	"time"
)

// The three text patches of the image, ported line for line from the awk scripts of
// tools/firmware_patch_templates (patch_service_sh.sh, patch_ethernet_hostname.sh) and from the sed of
// build_firmware.sh (drop_caches.sh). They work on bytes in memory. Like awk, they split records on "\n" only
// (a "\r" stays in the line) and always end the output with a newline.

// patchExit mirrors the exit codes of the scripts: 2 = a shape the patch refuses to guess about (nothing
// written), 3 = nothing to patch.
type patchError struct {
	code int
	msg  string
}

func (e *patchError) Error() string { return e.msg }

const (
	maintFlag   = "/mnt/AKAM_MAINTENANCE"
	maintLegacy = "/mnt/CAMYN_MAINTENANCE"
	maintMark   = "/tmp/akam_maintenance"
)

var (
	svcDaemonRe  = regexp.MustCompile(`(^|[^A-Za-z0-9_])(utelnetd|telnetd|bftpd|vsftpd|ftpd)([^A-Za-z0-9_-]|$)`)
	svcHookRe    = regexp.MustCompile(`(_ht_ap_mode\.conf|hack\.sh|/mnt/[A-Za-z0-9_./-]*hostapd|/mnt/telnet\.sh|/mnt/\$\{?AP_MODE_FILE|(^|[ \t;])sh[ \t]+[^ \t]*tf_update\.sh)`)
	svcAssignRe  = regexp.MustCompile(`^[ \t]*[A-Za-z_][A-Za-z0-9_]*=[^ \t;&|]*[ \t]*$`)
	svcCommentRe = regexp.MustCompile(`^[ \t]*#`)
	svcBlankRe   = regexp.MustCompile(`^[ \t]*$`)
	contRe       = regexp.MustCompile(`\\[ \t]*$`)
	indentRe     = regexp.MustCompile(`^[ \t]*`)
	svcIfRe      = regexp.MustCompile(`^if[ \t]`)
	svcKeywordRe = regexp.MustCompile(`^(elif|while|until|case|for|then|else|do|fi|done|esac|\{|\()`)
	svcTrailAmp  = regexp.MustCompile(`[ \t]*&[ \t]*$`)
)

// records splits like awk: on "\n", the final newline ends the last record instead of opening an empty one.
func records(b []byte) []string {
	if len(b) == 0 {
		return nil
	}
	s := strings.Split(string(b), "\n")
	if s[len(s)-1] == "" {
		s = s[:len(s)-1]
	}
	return s
}

func joinRecords(l []string) []byte {
	if len(l) == 0 {
		return nil
	}
	return []byte(strings.Join(l, "\n") + "\n")
}

// patchService is patch_service_sh.sh. note receives what the script prints on stderr. The result is
// the new content of sbin/service.sh and the counts of the awk END block.
func patchService(in []byte, note func(string)) (out []byte, err error) {
	guard := "[ -f " + maintFlag + " ]"
	guard = "{ " + guard + " || [ -f " + maintLegacy + " ]; }"
	hasMark := strings.Contains(string(in), maintMark)
	marked := false
	var res []string
	var dnew, hnew, ddone, hdone, dcomm int
	failed := false
	emitMark := func(l string) {
		if hasMark || marked {
			return
		}
		ind := indentRe.FindString(l)
		res = append(res, ind+guard+" && : > "+maintMark)
		marked = true
	}
	bad := func(nr int, msg, line string) {
		note(fmt.Sprintf("patch_service_sh: ERROR: line %d: %s\n    %s", nr, msg, line))
		failed = true
	}
	for i, line := range records(in) {
		nr := i + 1
		switch {
		case svcCommentRe.MatchString(line):
			if svcDaemonRe.MatchString(line) {
				dcomm++
			}
			res = append(res, line)
			continue
		case svcBlankRe.MatchString(line):
			res = append(res, line)
			continue
		case svcAssignRe.MatchString(line) && !svcDaemonRe.MatchString(line):
			// Bare variable assignment: only names the file, runs nothing.
			res = append(res, line)
			continue
		case strings.Contains(line, "AKAM-DISABLED:") || strings.Contains(line, "CAMYN-DISABLED:"):
			ddone++
			res = append(res, line)
			continue
		case strings.Contains(line, "AKAM_MAINTENANCE") || strings.Contains(line, maintFlag):
			hdone++
			if !strings.Contains(line, maintMark) {
				emitMark(line)
			}
			res = append(res, line)
			continue
		}
		if svcDaemonRe.MatchString(line) {
			if contRe.MatchString(line) {
				bad(nr, "daemon line ends with a continuation", line)
				res = append(res, line)
				continue
			}
			ind := indentRe.FindString(line)
			res = append(res, ind+": # AKAM-DISABLED: "+line[len(ind):])
			dnew++
			continue
		}
		if svcHookRe.MatchString(line) {
			if contRe.MatchString(line) {
				bad(nr, "hook line ends with a continuation", line)
				res = append(res, line)
				continue
			}
			ind := indentRe.FindString(line)
			body := line[len(ind):]
			emitMark(line)
			if svcIfRe.MatchString(body) {
				if strings.Contains(body, "||") {
					bad(nr, "hook condition contains ||: the guard would not bind", line)
					res = append(res, line)
					continue
				}
				body = strings.TrimLeft(strings.TrimPrefix(body, "if"), " \t")
				res = append(res, ind+"if "+guard+" && "+body)
			} else if svcKeywordRe.MatchString(body) {
				bad(nr, "hook line starts with an unsupported keyword", line)
				res = append(res, line)
				continue
			} else {
				tmp := svcTrailAmp.ReplaceAllString(body, "")
				if strings.ContainsAny(tmp, ";|#") {
					bad(nr, "hook line is a compound command or has a comment", line)
					res = append(res, line)
					continue
				}
				res = append(res, ind+guard+" && "+body)
			}
			hnew++
			continue
		}
		res = append(res, line)
	}
	if failed {
		return nil, &patchError{2, "unsupported service.sh layout, nothing written; review by hand"}
	}
	switch {
	case dnew+ddone == 0 && dcomm > 0 && hnew+hdone > 0:
		note(fmt.Sprintf("patch_service_sh: NOTE: no active telnet/ftp launch in sbin/service.sh (%d commented mention(s) only); patching the SD hook only", dcomm))
	case dnew+ddone == 0:
		note("patch_service_sh: WARNING: no telnet/ftp launch line found in sbin/service.sh\n    The image would NOT be locked down by this patch. File left untouched.\n    Inspect the real service.sh and extend the patterns.")
		return nil, &patchError{3, "no telnet/ftp launch line found in service.sh"}
	}
	if hnew+hdone == 0 {
		note("patch_service_sh: WARNING: no SD hook line found in sbin/service.sh\n    Daemon lines are still patched, but the SD hook (telnet:24 via hack.sh)\n    is NOT guarded. Check the real service.sh by hand.")
	}
	out = joinRecords(res)
	if err := shSyntax(out); err != nil {
		note(err.Error())
		return nil, &patchError{2, "patched service.sh fails sh -n, nothing written"}
	}
	note(fmt.Sprintf("patch_service_sh: ok: %d daemon line(s) and %d hook line(s) patched, %d/%d already patched", dnew, hnew, ddone, hdone))
	return out, nil
}

var (
	ethHostRe  = regexp.MustCompile(`-x[ \t]+"?hostname:`)
	ethUdhcpRe = regexp.MustCompile(`(^|[ \t;&|])udhcpc[ \t]`)
	ethRestRe  = regexp.MustCompile(`^([ \t;&|]|$)`)
)

// patchEthernet is patch_ethernet_hostname.sh on sbin/ethernet.sh.
func patchEthernet(in []byte, note func(string)) (out []byte, err error) {
	var res []string
	var neu, done int
	var lines []string
	failed := false
	bad := func(nr int, msg, line string) {
		note(fmt.Sprintf("patch_ethernet_hostname: ERROR: line %d: %s\n    %s", nr, msg, line))
		failed = true
	}
	const tok = "-x hostname:IPCamera"
	for i, line := range records(in) {
		nr := i + 1
		if svcCommentRe.MatchString(line) || !ethHostRe.MatchString(line) {
			res = append(res, line)
			continue
		}
		if strings.Contains(line, "akam_hostname.sh") {
			done++
			res = append(res, line)
			continue
		}
		if contRe.MatchString(line) {
			bad(nr, "hostname line ends with a continuation", line)
			res = append(res, line)
			continue
		}
		if !ethUdhcpRe.MatchString(line) {
			bad(nr, "hostname option outside an udhcpc command", line)
			res = append(res, line)
			continue
		}
		p := strings.Index(line, tok)
		if p < 0 {
			bad(nr, "hostname option with an unexpected value", line)
			res = append(res, line)
			continue
		}
		rest := line[p+len(tok):]
		if !ethRestRe.MatchString(rest) {
			bad(nr, "unexpected text right after the hostname value", line)
			res = append(res, line)
			continue
		}
		res = append(res, line[:p]+`-x "hostname:$(/usr/sbin/akam_hostname.sh show 2>/dev/null || echo akam)"`+rest)
		lines = append(lines, fmt.Sprint(nr))
		neu++
	}
	if failed {
		return nil, &patchError{2, "unsupported ethernet.sh layout, nothing written; review by hand"}
	}
	if neu+done == 0 {
		note("patch_ethernet_hostname: WARNING: no active 'udhcpc -x hostname:' line in sbin/ethernet.sh; file left untouched")
		return nil, &patchError{3, "no active 'udhcpc -x hostname:' line in ethernet.sh"}
	}
	out = joinRecords(res)
	if err := shSyntax(out); err != nil {
		note(err.Error())
		return nil, &patchError{2, "patched ethernet.sh fails sh -n, nothing written"}
	}
	ln := "none"
	if len(lines) > 0 {
		ln = strings.Join(lines, " ")
	}
	note(fmt.Sprintf("patch_ethernet_hostname: ok: %d line(s) patched (line %s), %d already patched", neu, ln, done))
	return out, nil
}

var (
	dropSleep5  = regexp.MustCompile(`^([[:space:]]*)sleep 5[[:space:]]*$`)
	dropSleep60 = regexp.MustCompile(`^[[:space:]]*sleep 60[[:space:]]*$`)
)

// patchDropCaches slows sbin/drop_caches.sh from 5 s to 60 s. changed=false means "left as it is";
// reason says why when the layout is unknown.
func patchDropCaches(in []byte) (out []byte, changed bool, reason string) {
	text := string(in)
	lines := strings.Split(text, "\n") // sed edits line by line and keeps a missing final newline
	has5, has60 := false, false
	for _, l := range lines {
		if dropSleep5.MatchString(l) {
			has5 = true
		}
		if dropSleep60.MatchString(l) {
			has60 = true
		}
	}
	switch {
	case has5:
		for i, l := range lines {
			if m := dropSleep5.FindStringSubmatch(l); m != nil {
				lines[i] = m[1] + "sleep 60"
			}
		}
		return []byte(strings.Join(lines, "\n")), true, ""
	case has60:
		return in, false, ""
	}
	return in, false, "sbin/drop_caches.sh has no 'sleep 5' line: not patched"
}

// klogdNew replaces "klogd -n &": restart the filter when a signal kills it (up to 5 times), run the stock klogd
// when it cannot start or after the last crash. Identical to KLOG_NEW in build_firmware.sh.
const klogdNew = `{ _k=0; while [ "$_k" -lt 5 ]; do /usr/bin/ak_rtsp klog; [ $? -ge 128 ] || break; _k=$((_k + 1)); sleep $((_k * 2)); done; klogd -n; } & # AKAM: filtered kernel log (stock: klogd -n)`

var klogdRe = regexp.MustCompile(`^([[:space:]]*)klogd -n &([[:space:]]*#.*)?[[:space:]]*$`)

// patchKlogd is the sed of build_firmware.sh on sbin/main.sh: "klogd -n &" becomes the kernel log filter with
// the stock klogd as fallback. changed=false means "left as it is"; reason says why when the layout is unknown.
func patchKlogd(in []byte) (out []byte, changed bool, reason string) {
	lines := strings.Split(string(in), "\n")
	found := false
	for i, l := range lines {
		if m := klogdRe.FindStringSubmatch(l); m != nil {
			lines[i] = m[1] + klogdNew
			found = true
		}
	}
	switch {
	case found:
		return []byte(strings.Join(lines, "\n")), true, ""
	case strings.Contains(string(in), "ak_rtsp klog"):
		return in, false, ""
	}
	return in, false, "sbin/main.sh has no 'klogd -n &' line: kernel log filter not installed (stock klogd runs unfiltered)"
}

var errNoSh = errors.New("no sh on this computer")

// shSyntax runs `sh -n` on the script (read from standard input, nothing is written to disk): the syntax
// guard of the bash patches. When the computer has no sh (Windows) the check is skipped.
func shSyntax(script []byte) error {
	sh, err := exec.LookPath("sh")
	if err != nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, sh, "-n")
	cmd.Env = childenv.Env()
	cmd.Stdin = bytes.NewReader(script)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s", strings.TrimSpace(stderr.String()))
	}
	return nil
}
