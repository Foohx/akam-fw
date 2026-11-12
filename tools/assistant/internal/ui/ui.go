// Package ui is the boundary between the commands and the terminal. Commands only see the UI
// interface; the plain implementation (line based, same strings as the bash wizard) and the
// bubbletea one (package ui/tui) are interchangeable, and tests drive a Plain with scripted input.
package ui

import "errors"

// ErrFailed means the error was already shown to the user (exit status 1).
var ErrFailed = errors.New("failed")

// ErrNoTTY means a question needs a terminal and there is none (nothing was asked).
var ErrNoTTY = errors.New("no terminal")

// ErrNoInput means the input ended before an answer was given.
var ErrNoInput = errors.New("no input")

// UsageError is a command-line mistake: the message is printed on stderr and the exit status is 2.
type UsageError struct{ Msg string }

func (e *UsageError) Error() string { return e.Msg }

// Usage builds a UsageError.
func Usage(msg string) error { return &UsageError{Msg: msg} }

// UI is everything a command may show or ask.
type UI interface {
	// Output (stdout unless noted).
	Say(s string)
	Bold(s string)
	Good(s string)
	Warn(s string) // "WARNING: s"
	Err(s string)  // "ERROR: s" on stderr
	Hint(s string) // "  What to do: s"
	Blank()
	// Fail shows an error and what to do, and returns ErrFailed.
	Fail(msg, hint string) error

	// Questions. Every yes/no defaults to No (Enter, Esc, q, end of input).
	Prompt(prompt, def string) (string, error)
	Secret(prompt string) (string, error)
	AskYN(prompt string) (bool, error)
	// Danger is the red-box confirmation of an irreversible action: refuses without a terminal.
	Danger(title, question string, lines ...string) (bool, error)
	Pick(title string, items []string) (int, error)

	Progress(label string, pct int)
	ProgressClose()

	// Banner prints the heading of a command ("Step N/T - title" or just the title).
	Banner(title string)

	// RequireTTY refuses (error shown) when no terminal is attached; a rehearsal is exempt.
	RequireTTY(action string) error
	Interactive() bool
	Dry() bool
}

// MenuItem is one line of the main menu.
type MenuItem struct {
	Label  string
	Detail string
	Status string // done | todo | blocked | stale | tool | plain (no tag, the home screen)
	Key    string // optional one-letter shortcut (not q, j, k, g, G or ?)
}

// MenuResult is the outcome of the last action, shown on the strip above the footer of the menu the
// user returns to. Level is "ok", "warn" or "error"; an empty Text means nothing to show.
type MenuResult struct {
	Level string
	Text  string
}

// MenuView is a full-screen menu with the result of the previous action.
type MenuView struct {
	Title  string
	Items  []MenuItem
	Result MenuResult
}

// HeaderBadge is one health badge of the session header. Level is "ok", "warn", "error", "info" or
// "muted"; Text is one short word (12 columns at most).
type HeaderBadge struct {
	Level string
	Text  string
}

// Header is the first lines of the full-screen session: the product line and the chosen camera with
// its health badges. Badges are listed by priority; a narrow window drops them from the right.
// An empty Name and IP mean "no camera".
type Header struct {
	Product  string // "Akam 0.9  linux/amd64"; empty means "Akam"
	Name     string
	IP       string
	Firmware string
	Badges   []HeaderBadge
}

// HeaderSetter is implemented by the session UI: SetHeader replaces the header (it may be called
// from the flow goroutine at any time).
type HeaderSetter interface{ SetHeader(h Header) }
