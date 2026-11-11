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
	Status string // done | todo | blocked | stale | tool
}
