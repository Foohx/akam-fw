package ui

import (
	"bufio"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"io"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"

	"golang.org/x/term"

	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
)

// Plain is the line-based UI: the strings and the prompts of the bash wizard (ui.sh, host.sh).
type Plain struct {
	In      *bufio.Reader
	Out     io.Writer
	ErrOut  io.Writer
	Color   bool
	DryRun  bool
	ForceTT bool // AKAM_FORCE_TTY=1: pretend a terminal is attached (tests, pipes)
	// InFile and ErrFile, when they are the real terminal, select the no-echo secret reader and
	// the interactive checks. Nil in tests.
	InFile  *os.File
	ErrFile *os.File

	Log     *state.Log
	Secrets *state.Secrets

	progLabel string
	progLast  int
}

// NewPlain builds a Plain on the process terminal.
func NewPlain(dry, color bool, log *state.Log, secrets *state.Secrets) *Plain {
	return &Plain{
		In: bufio.NewReader(os.Stdin), Out: os.Stdout, ErrOut: os.Stderr,
		Color: color, DryRun: dry, ForceTT: testhook.Env("AKAM_FORCE_TTY") == "1",
		InFile: os.Stdin, ErrFile: os.Stderr, Log: log, Secrets: secrets, progLast: -1,
	}
}

func (p *Plain) logf(format string, a ...any) {
	if p.Log != nil {
		p.Log.Line(format, a...)
	}
}

func (p *Plain) paint(code, s string) string {
	if p.Color && code != "" {
		return "\033[" + code + "m" + s + "\033[0m"
	}
	return s
}

func (p *Plain) Say(s string)  { fmt.Fprintln(p.Out, s); p.logf("SAY %s", s) }
func (p *Plain) Bold(s string) { fmt.Fprintln(p.Out, p.paint("1", s)) }
func (p *Plain) Good(s string) { fmt.Fprintln(p.Out, p.paint("32", s)) }
func (p *Plain) Warn(s string) {
	fmt.Fprintln(p.Out, p.paint("33", "WARNING: "+s))
	p.logf("WARN %s", s)
}
func (p *Plain) Err(s string) {
	fmt.Fprintln(p.ErrOut, p.paint("31", "ERROR: "+s))
	p.logf("ERROR %s", s)
}
func (p *Plain) Hint(s string) { fmt.Fprintf(p.Out, "  What to do: %s\n", s) }
func (p *Plain) Blank()        { fmt.Fprintln(p.Out) }

func (p *Plain) Fail(msg, hint string) error {
	p.Err(msg)
	if hint != "" {
		p.Hint(hint)
	}
	return ErrFailed
}

func (p *Plain) Dry() bool { return p.DryRun }

// Interactive is host_is_tty: stdin and stderr are terminals (or AKAM_FORCE_TTY=1).
func (p *Plain) Interactive() bool {
	if p.ForceTT {
		return true
	}
	return p.InFile != nil && p.ErrFile != nil &&
		term.IsTerminal(int(p.InFile.Fd())) && term.IsTerminal(int(p.ErrFile.Fd()))
}

func (p *Plain) RequireTTY(action string) error {
	if p.DryRun || p.Interactive() {
		return nil
	}
	p.Err(action + " needs an interactive terminal and refuses to run without one.")
	p.Hint("run the command from a terminal, or use --dry-run to rehearse.")
	return ErrFailed
}

func (p *Plain) readLine() (string, error) {
	s, err := p.In.ReadString('\n')
	if err != nil && s == "" {
		return "", ErrNoInput
	}
	return strings.TrimRight(s, "\r\n"), nil
}

func (p *Plain) Prompt(prompt, def string) (string, error) {
	fmt.Fprintf(p.ErrOut, "%s ", prompt)
	s, err := p.readLine()
	if err != nil {
		if p.DryRun {
			fmt.Fprintln(p.ErrOut)
			return def, nil
		}
		return "", err
	}
	if s == "" {
		s = def
	}
	return s, nil
}

func (p *Plain) Secret(prompt string) (string, error) {
	fmt.Fprintf(p.ErrOut, "%s ", prompt)
	var s string
	if p.InFile != nil && term.IsTerminal(int(p.InFile.Fd())) {
		b, err := readPasswordRestoring(int(p.InFile.Fd()))
		fmt.Fprintln(p.ErrOut)
		if err != nil {
			return "", ErrNoInput
		}
		s = string(b)
	} else {
		var err error
		if s, err = p.readLine(); err != nil {
			return "", err
		}
	}
	p.Secrets.Add(s)
	return s, nil
}

// AskYN is host_confirm: [y/N] on a terminal, never auto-confirms, refuses without a terminal.
func (p *Plain) AskYN(prompt string) (bool, error) {
	if !p.Interactive() {
		fmt.Fprintf(p.ErrOut, "refusing to ask '%s' without a terminal\n", prompt)
		return false, ErrNoTTY
	}
	fmt.Fprintf(p.ErrOut, "%s [y/N] ", prompt)
	s, err := p.readLine()
	if err != nil {
		return false, err
	}
	switch s {
	case "y", "Y", "yes", "YES", "Yes":
		return true, nil
	}
	return false, nil
}

func (p *Plain) Danger(title, question string, lines ...string) (bool, error) {
	if p.DryRun {
		p.Say(fmt.Sprintf("WOULD ask: %s [y/N] (default: No) -- %s: %s", question, title, strings.Join(lines, " ")))
		return true, nil
	}
	if err := p.RequireTTY(title); err != nil {
		return false, err
	}
	p.Blank()
	p.Warn(title)
	for _, l := range lines {
		p.Say("  " + l)
	}
	return p.AskYN("  " + question)
}

func (p *Plain) Pick(title string, items []string) (int, error) {
	p.Say("  " + title)
	for i, it := range items {
		p.Say(fmt.Sprintf("    %d) %s", i+1, it))
	}
	for {
		fmt.Fprint(p.ErrOut, "  Number [1]: ")
		s, err := p.readLine()
		if err != nil {
			return -1, err
		}
		if s == "" {
			s = "1"
		}
		if n, err := strconv.Atoi(s); err == nil && n >= 1 && n <= len(items) {
			return n - 1, nil
		}
		p.Say(fmt.Sprintf("  Please type a number between 1 and %d.", len(items)))
	}
}

func (p *Plain) Banner(title string) {
	p.Blank()
	p.Bold(title)
}

// Progress prints the first value, then every 10 %, then 100 %, so a log or a pipe never floods.
func (p *Plain) Progress(label string, pct int) {
	if label != p.progLabel {
		p.ProgressClose()
		p.progLabel = label
		p.progLast = -1
	}
	if pct > 100 {
		pct = 100
	}
	if p.progLast < 0 || pct >= p.progLast+10 || (pct >= 100 && p.progLast < 100) {
		p.progLast = pct
		fmt.Fprintf(p.Out, "    | %s: %d%%\n", label, pct)
	}
}

func (p *Plain) ProgressClose() {
	if p.progLast >= 100 {
		p.progLabel = ""
		p.progLast = -1
	}
}

// readPasswordRestoring reads a password without echo; a signal (Ctrl-C, TERM, HUP) during the read
// puts the terminal back as it was before the process exits, so the user's shell keeps echoing.
func readPasswordRestoring(fd int) ([]byte, error) {
	old, gerr := term.GetState(fd)
	if gerr != nil {
		return term.ReadPassword(fd)
	}
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	done := make(chan struct{})
	defer func() { signal.Stop(ch); close(done) }()
	go func() {
		select {
		case sig := <-ch:
			_ = term.Restore(fd, old)
			code := 130
			switch sig {
			case syscall.SIGTERM:
				code = 143
			case syscall.SIGHUP:
				code = 129
			}
			os.Exit(code)
		case <-done:
		}
	}()
	return term.ReadPassword(fd)
}
