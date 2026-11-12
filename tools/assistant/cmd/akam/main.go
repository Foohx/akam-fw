// Command akam is the single-binary assistant for Akam cameras (see tools/assistant/README.md).
package main

import (
	"os"

	"golang.org/x/term"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/platform"
	"github.com/Foohx/akam-fw/tools/assistant/internal/sdcard"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

// version is set at build time: -ldflags "-X main.version=...".
var version = "1.0.0-dev"

// wantPlain is the capability decision: the plain interface with --plain or AKAM_PLAIN=1, when stdin
// or stderr is not a terminal (tui.Decide), with TERM=dumb, and when the console refused ANSI/VT
// output (an old Windows console): drawing a full-screen interface there would print escape codes.
func wantPlain(plainFlag, isTTY bool, env tui.Env, vtOK bool) bool {
	return tui.Decide(env, plainFlag, isTTY) == tui.ModePlain || !vtOK
}

// makeUI picks the interface: the full-screen one on a capable terminal, the plain one otherwise.
func makeUI(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
	p := cli.NewPlainUI(o, st, log, secrets, dry)
	in, ok1 := st.In.(*os.File)
	er, ok2 := st.Err.(*os.File)
	if !ok1 || !ok2 {
		return p
	}
	isTTY := term.IsTerminal(int(in.Fd())) && term.IsTerminal(int(er.Fd()))
	if wantPlain(o.Plain || os.Getenv("AKAM_PLAIN") == "1", isTTY, tui.CurrentEnv(er), platform.VTEnabled()) {
		return p
	}
	return tui.New(p)
}

func main() {
	platform.Init()
	// The privileged card helper: nothing else runs in this mode (no state directory, no lock, no UI).
	if code, ok := sdcard.HelperEntry(os.Args[1:], os.Stdin, os.Stdout, os.Stderr); ok {
		os.Exit(code)
	}
	os.Exit(cli.Run(os.Args[1:], cli.Streams{In: os.Stdin, Out: os.Stdout, Err: os.Stderr}, version, cli.Hooks{UI: makeUI}))
}
