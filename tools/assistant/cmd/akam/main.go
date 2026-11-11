// Command akam is the single-binary assistant for Akam cameras (see tools/assistant/README.md).
package main

import (
	"os"

	"golang.org/x/term"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui/tui"
)

// version is set at build time: -ldflags "-X main.version=...".
var version = "1.0.0-dev"

// makeUI picks the interface: the full-screen one on a terminal; the plain one with --plain,
// AKAM_PLAIN=1, or when stdin or stderr is not a terminal.
func makeUI(o *cli.Options, st cli.Streams, log *state.Log, secrets *state.Secrets, dry bool) ui.UI {
	p := cli.NewPlainUI(o, st, log, secrets, dry)
	in, ok1 := st.In.(*os.File)
	er, ok2 := st.Err.(*os.File)
	if o.Plain || os.Getenv("AKAM_PLAIN") == "1" || !ok1 || !ok2 || !term.IsTerminal(int(in.Fd())) || !term.IsTerminal(int(er.Fd())) {
		return p
	}
	return tui.New(p)
}

func main() {
	os.Exit(cli.Run(os.Args[1:], cli.Streams{In: os.Stdin, Out: os.Stdout, Err: os.Stderr}, version, cli.Hooks{UI: makeUI}))
}
