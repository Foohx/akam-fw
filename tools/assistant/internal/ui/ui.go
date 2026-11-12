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

// FormKind is how one row of a Form is edited.
type FormKind int

const (
	FormEnum     FormKind = iota // Choices, cycled with left/right/Space (on/off included)
	FormInt                      // a number in [Min, Max]: arrows, Shift or PgUp/PgDn, typed digits
	FormText                     // a free text edited inline (Secret: never echoed, never shown)
	FormReadonly                 // shown, not editable
)

// FormRow is one option of a Form. The cli side fills it from what the camera reports; the widget
// knows nothing about cameras. Value is the current real value as text: the choice for an enum, the
// decimal number for an int, the text for a text row (empty for a Secret row, which is never shown).
type FormRow struct {
	ID          string // opaque key handed back to the apply callback
	Group       string // optional: rows with a group are shown one group at a time (Tab switches)
	Label       string
	Kind        FormKind
	Value       string
	Choices     []string // enum
	Min, Max    int      // int: the bar is drawn from them
	Secret      bool     // text: nothing typed is ever drawn
	NotSaved    bool     // "! not saved": lost at the next reboot
	Unavailable bool     // muted "needs update": not editable
	Note        string   // one line shown while the row is focused (empty: nothing)
	Detail      string   // the long text shown by "?"
}

// Former is implemented by the full-screen session only (plain does not: callers check with a type
// assertion and keep their line based path). Form edits rows in place and returns when the user
// leaves it. apply is called off the screen goroutine, never concurrently with itself, after a short
// pause without keys; it returns the row as it really is now (an empty ID keeps the row and takes the
// submitted value) or an error, in which case the row snaps back to its real value.
type Former interface {
	Form(title string, rows []FormRow, apply func(id, value string) (FormRow, error)) error
}

// FormKey is a one-letter action of a form (Label is its footer text): pressing it closes the form
// and reports the key, so the caller can run a question or a screen and open the form again.
type FormKey struct{ Key, Label string }

// FormSpec is a Form with keys. Group is the group shown first (empty: the first one).
type FormSpec struct {
	Title string
	Rows  []FormRow
	Apply func(id, value string) (FormRow, error)
	Keys  []FormKey
	Group string
}

// FormResult tells how a form ended: Key is the FormKey pressed ("" when the user left with Esc) and
// Group the group that was shown.
type FormResult struct{ Key, Group string }

// FormKeyer is a Former whose forms can carry keys (the session implements it).
type FormKeyer interface {
	FormKeys(spec FormSpec) (FormResult, error)
}

// Detailer shows a long text in a scrollable pane and returns when the user closes it.
type Detailer interface{ Details(title, text string) }

// Checklister shows physical-action steps; one key press (Space or Enter) acknowledges them all and
// returns nil. q, Esc and Ctrl-C return ErrNoInput (the user stops).
type Checklister interface {
	Checklist(title string, steps []string) error
}

// Operator is implemented by the full-screen session only (plain does not: callers check with a type
// assertion and keep their line output). It describes one long operation (backup, image build, flash,
// update, restore, verify, download) as a list of items: the running one has a spinner, the finished
// ones a short note, the pending ones are muted. Every method may be called from the flow goroutine
// at any time and never blocks.
type Operator interface {
	// OpPlan declares the items of the operation, all pending, replacing any earlier plan.
	OpPlan(items []string)
	// OpStart marks item i as running (the item running before it, if any, is marked done).
	OpStart(i int)
	// OpDone marks item i as finished with a short note ("12.4 GB free"; empty: none).
	OpDone(i int, note string)
	// OpStatus is the line under the list: what runs now ("dd if=/dev/mtdblock2 bs=64k", read only).
	OpStatus(line string)
	// OpBytes gives the size of the running item: done of total bytes (total 0: unknown). The panel
	// derives the percentage, the speed and the time left from it.
	OpBytes(done, total int64)
	// OpWriting says a write to the camera is under way: it cannot be interrupted safely, so the
	// screen offers no stop and ignores Ctrl-C until OpWriting(false).
	OpWriting(on bool)
	// OpEnd removes the list (the result stays on the strip).
	OpEnd()
}
