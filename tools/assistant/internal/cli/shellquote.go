package cli

import (
	"runtime"
	"strings"
)

// shellPath quotes a path for a hint the user will copy-paste into a shell: left bare when it only has safe
// characters, else single-quoted (POSIX) or double-quoted (Windows).
func shellPath(p string) string { return shellQuoteFor(runtime.GOOS, p) }

func shellQuoteFor(goos, p string) string {
	safe := p != ""
	for i := 0; i < len(p) && safe; i++ {
		c := p[i]
		safe = c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.IndexByte("_./:@%+=,-", c) >= 0
		if c == '\\' && goos == "windows" {
			safe = true
		}
	}
	if safe {
		return p
	}
	if goos == "windows" {
		return `"` + strings.ReplaceAll(p, `"`, `\"`) + `"`
	}
	return "'" + strings.ReplaceAll(p, "'", `'\''`) + "'"
}
