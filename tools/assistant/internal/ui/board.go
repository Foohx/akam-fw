package ui

// A board is a persistent list screen of the full-screen session (specs/installer.md): the home
// menu and the camera chooser. The flow owns its content and replaces it whenever something changes
// (a probe answered), the session draws it in place and reports the keys the user presses.

// BoardRow is one line of a board.
type BoardRow struct {
	ID     string      // stable identity (an address): the cursor follows it when the rows change
	Label  string      // first column
	Detail string      // second column
	Badge  HeaderBadge // third column; an empty Text means none
	Note   string      // last column, muted
	Hot    string      // hidden one-key shortcut that picks this row (a digit); never shown
}

// BoardKey is a footer hint and a key the board reports to the flow ("s" scan, "Esc" back). A key
// without a Word is reported but not shown (the hidden shortcut "8" of the home screen).
type BoardKey struct{ Key, Word string }

// Board is what the session shows.
type Board struct {
	Title       string
	Rows        []BoardRow
	DetailMuted bool       // the second column is secondary text
	Empty       string     // shown (muted) when there is no row
	Help        []string   // lines shown under the list while "?" is on; none: no "?" key
	Keys        []BoardKey // footer hints after the implicit "up/down move"
	Status      string     // a line under the list (a search in progress); empty: none
	StatusLevel string     // level of the status badge: ok, warn, error, info, muted
}

// Boarder is implemented by the session UI. Without it the flows keep their Pick based screens.
type Boarder interface {
	// SetBoard shows the board, or replaces the one shown; it never blocks.
	SetBoard(b Board)
	// BoardKey blocks until the user presses a reported key: "enter" (or the Hot key of a row) with
	// that row's index, one of the Keys (lower case, "esc" for Esc) with the index of the row under
	// the cursor, or "esc" on Ctrl-C. The index is -1 without rows.
	BoardKey() (key string, row int, err error)
	// WaitBoardKey is BoardKey that also returns when stop is closed (with an empty key): the flow
	// listens for Esc while a search runs.
	WaitBoardKey(stop <-chan struct{}) (key string, row int, err error)
	// CloseBoard hides the board (the flow is about to show text or ask a question on the panel).
	CloseBoard()
}
