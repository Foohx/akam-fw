// Command akam is the single-binary assistant for Akam cameras (see tools/assistant/README.md).
package main

import (
	"os"

	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
)

// version is set at build time: -ldflags "-X main.version=...".
var version = "1.0.0-dev"

func main() {
	os.Exit(cli.Run(os.Args[1:], cli.Streams{In: os.Stdin, Out: os.Stdout, Err: os.Stderr}, version, cli.Hooks{}))
}
