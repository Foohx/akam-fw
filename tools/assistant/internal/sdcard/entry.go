package sdcard

import (
	"io"
	"runtime"
)

// helperMode is which way an `sd-helper` invocation receives its request.
type helperMode int

const (
	helperBad    helperMode = iota // arguments that no mode accepts: refused (exit 2)
	helperStdin                    // `sd-helper`: Linux sudo/pkexec, request on stdin
	helperSocket                   // `sd-helper --socket P --uid N --nonce H`: macOS osascript (macsock.go)
	helperPipe                     // `sd-helper --pipe NAME --nonce H`: Windows UAC (uac.go)
)

// helperModeFor classifies the arguments after `sd-helper` for an operating system. It only looks at the shape;
// each mode validates its own values. The per-OS flags exist on their OS alone: elsewhere they are refused, so
// the Linux invocation (one argument, JSON on stdin) cannot be given a second way in.
func helperModeFor(goos string, rest []string) helperMode {
	switch {
	case len(rest) == 0:
		return helperStdin
	case goos == "darwin" && rest[0] == "--socket":
		return helperSocket
	case goos == "windows" && rest[0] == "--pipe":
		return helperPipe
	}
	return helperBad
}

// HelperEntry is the entry of the privileged mode: it handles `sd-helper` and reports whether args named it.
// Nothing else runs in this mode: no state directory, no lock, no UI.
func HelperEntry(args []string, in io.Reader, out, progress io.Writer) (code int, handled bool) {
	if len(args) == 0 || args[0] != "sd-helper" {
		return 0, false
	}
	p := newPlatform()
	mk := func() Helper { return Helper{Inv: p.Inventory, Opener: p.Opener} }
	switch helperModeFor(runtime.GOOS, args[1:]) {
	case helperSocket:
		return socketHelper(args[1:], mk, peerUIDOS, ownerUID), true
	case helperPipe:
		return pipeHelper(args[1:], mk, dialPipeOS), true
	case helperStdin:
		h := Helper{Inv: p.Inventory, Opener: p.Opener, Progress: progress}
		if s, ok := p.Inventory.(Sys); ok {
			h.Sys = s
		}
		return HelperMain(in, out, h), true
	}
	return 2, true
}
